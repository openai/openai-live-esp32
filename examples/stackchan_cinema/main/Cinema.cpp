#include "Cinema.hpp"
#include "Board.hpp"
#include "CinemaCue.hpp"
#include "CinemaEmbedding.hpp"
#include "CinemaPolicy.hpp"
#include "CinemaStorage.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

#include "CinemaAssets.hpp"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdlib>
#include <new>

extern "C" const uint8_t fallback_start[] asm("_binary_fallback_bin_start");
extern "C" const uint8_t fallback_end[] asm("_binary_fallback_bin_end");
namespace cinema {
namespace {
constexpr char TAG[] = "Cinema";
static_assert(assets::COUNT > 0 && assets::COUNT <= 30000 &&
                  assets::DIMENSIONS == 50,
              "Prepare a 1..30000-clip, 50-dimensional local cinema pack");
std::atomic<bool> enabled{false}, initialized{false};
std::atomic<bool> player_failed{false}, player_idle{true};
std::atomic<bool> photo_paused{false};
std::atomic<unsigned> requests{0}, failures{0}, selected_count{0};
std::atomic<unsigned> frames_shown{0}, frames_late{0}, decode_failures{0};
std::atomic<int> current{-1};
std::atomic<uint32_t> scene_epoch{0};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Mailbox mailbox;
Ranking ranking;
bool have_ranking = false;
int64_t ranking_expires = 0;
std::atomic<bool> *playable = nullptr;
History history;
TaskHandle_t query_task = nullptr;
struct Runtime {
  uint32_t total = 0, idle0 = 0, idle1 = 0, player = 0, gui = 0, query = 0;
};
Runtime runtime() {
  TaskStatus_t tasks[32]{};
  Runtime value;
  const auto count = uxTaskGetSystemState(tasks, 32, &value.total);
  for (unsigned i = 0; i < count; ++i) {
    const auto &task = tasks[i];
    if (!strcmp(task.pcTaskName, "IDLE0"))
      value.idle0 = task.ulRunTimeCounter;
    if (!strcmp(task.pcTaskName, "IDLE1"))
      value.idle1 = task.ulRunTimeCounter;
    if (!strcmp(task.pcTaskName, "cinema_player"))
      value.player = task.ulRunTimeCounter;
    if (!strcmp(task.pcTaskName, "cinema_queries"))
      value.query = task.ulRunTimeCounter;
    if (strstr(task.pcTaskName, "LVGL") || strstr(task.pcTaskName, "lvgl"))
      value.gui += task.ulRunTimeCounter;
  }
  if (!count)
    value.total = 0;
  return value;
}

bool enqueue(const char *text, bool priority) {
  if (!initialized.load() || !enabled.load() || player_failed.load())
    return false;
  const int64_t now = esp_timer_get_time();
  bool queued = false;
  portENTER_CRITICAL(&lock);
  queued = mailbox.submit(text, now, priority);
  portEXIT_CRITICAL(&lock);
  if (queued)
    xTaskNotifyGive(query_task);
  return queued;
}

bool embed(const char *text, float *output) {
  unsigned matched = 0;
  const bool ok =
      embed_local(text, output, assets::DIMENSIONS, assets::WORDS,
                  assets::WORD_COUNT, assets::WORD_STRINGS, &matched);
  ESP_LOGI(TAG, "Local encoder words=%u vocabulary=%u network=none", matched,
           assets::WORD_COUNT);
  return ok;
}

void queries(void *) {
  float query[assets::DIMENSIONS]{};
  int64_t next_request = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (!enabled.load() || esp_timer_get_time() < next_request)
      continue;
    char cue[257]{};
    uint32_t generation = 0;
    portENTER_CRITICAL(&lock);
    mailbox.take(cue, generation);
    portEXIT_CRITICAL(&lock);
    if (!cue[0])
      continue;
    ++requests;
    const int64_t began = esp_timer_get_time();
    const bool ok = embed(cue, query);
    next_request = esp_timer_get_time() + 2000000;
    if (!ok) {
      ++failures;
      ESP_LOGW(TAG,
               "Cue has no recognized vocabulary; offline dreams continue");
      continue;
    }
    Candidates<64> coarse;
    for (unsigned i = 0; i < assets::COUNT; ++i) {
      if ((i & 127) == 127)
        vTaskDelay(1);
      const bool usable = playable[i].load();
      if (usable)
        coarse.add(i,
                   cosine(query, assets::VECTORS[i], assets::CLIPS[i].norm,
                          assets::DIMENSIONS),
                   assets::CLIPS[i].source);
    }
    uint16_t query_words[16];
    const unsigned word_count = keywords(cue, assets::WORDS, assets::WORD_COUNT,
                                         assets::WORD_STRINGS, query_words, 16);
    Ranking next;
    for (unsigned i = 0; i < coarse.count; ++i) {
      const auto &c = coarse.best[i];
      const float detail = keyword_similarity(
          query_words, word_count, assets::CLIPS[c.id].keywords, 16,
          assets::WORDS, assets::WORD_COUNT, assets::DIMENSIONS);
      next.add(c.id, .4f * c.score + .6f * detail, c.source);
      if ((i & 7) == 7)
        vTaskDelay(1);
    }
    bool accepted = false;
    portENTER_CRITICAL(&lock);
    // A newer cue, session end or cinema-off invalidates an in-flight result.
    if (mailbox.accepts(generation) && enabled.load()) {
      ranking = next;
      have_ranking = next.count != 0;
      ranking_expires = esp_timer_get_time() + 45000000;
      accepted = true;
      ++scene_epoch;
    }
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Cue %s candidates=%u best=%d score=%.3f latency_ms=%lld",
             accepted ? "matched" : "superseded", next.count,
             next.count ? next.best[0].id : -1,
             next.count ? next.best[0].score : 0.f,
             (esp_timer_get_time() - began) / 1000);
  }
}

int choose_next(uint32_t &epoch) {
  Ranking next;
  const auto now = esp_timer_get_time();
  portENTER_CRITICAL(&lock);
  epoch = scene_epoch.load();
  if (have_ranking && now < ranking_expires)
    next = ranking;
  portEXIT_CRITICAL(&lock);
  Ranking available;
  for (unsigned i = 0; i < next.count; ++i) {
    const auto &c = next.best[i];
    if (playable[c.id].load())
      available.add(c.id, c.score, c.source);
  }
  next = available;
  const int previous = current.load();
  // Idle dreaming follows semantic neighbors, with occasional global
  // exploration.
  const bool explore = esp_random() % 5 == 0;
  if (!next.count && previous >= 0 && !explore) {
    for (auto i : assets::CLIPS[previous].neighbors)
      if (playable[i] && i != previous)
        next.add(i, .5f, assets::CLIPS[i].source);
  }
  if (!next.count) {
    const unsigned start = esp_random() % assets::COUNT;
    for (unsigned n = 0; n < assets::COUNT; ++n) {
      const auto i = (start + n) % assets::COUNT;
      if (playable[i])
        next.add(i, .5f, assets::CLIPS[i].source);
    }
  }
  return next.choose(history, esp_random());
}

void play(void *) {
  size_t capacity = 0;
  for (auto &clip : assets::CLIPS)
    capacity = std::max(capacity, size_t(clip.bytes));
  uint8_t *compressed =
      capacity <= MAX_FILM ? static_cast<uint8_t *>(heap_caps_malloc(
                                 capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                           : nullptr;
  uint8_t *pixels[2] = {
      static_cast<uint8_t *>(heap_caps_aligned_alloc(
          16, PIXEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      static_cast<uint8_t *>(heap_caps_aligned_alloc(
          16, PIXEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  jpeg_dec_handle_t decoder = nullptr;
  bool ok = compressed && pixels[0] && pixels[1] &&
            jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK;
  if (!ok) {
    ESP_LOGE(TAG, "Player allocation failed; captions remain available");
    free(compressed);
    free(pixels[0]);
    free(pixels[1]);
    if (decoder)
      jpeg_dec_close(decoder);
    player_failed = true;
    enabled = false;
    vTaskDeleteWithCaps(nullptr);
    return;
  }
  ESP_LOGI(TAG,
           "Player ready clips=%u compressed_capacity=%u pixels=%u fps=12 "
           "audio=none",
           assets::COUNT, unsigned(capacity), PIXEL_BYTES * 2);
  bool published = false;
  auto *card_failed = static_cast<bool *>(heap_caps_calloc(
      assets::COUNT, sizeof(bool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!card_failed) {
    player_failed = true;
    enabled = false;
    jpeg_dec_close(decoder);
    free(compressed);
    free(pixels[0]);
    free(pixels[1]);
    vTaskDeleteWithCaps(nullptr);
    return;
  }
  unsigned back = 0;
  for (;;) {
    if (photo_paused.load()) {
      player_idle = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!enabled.load()) {
      player_idle = true;
      if (published && board::view_movie_frame(nullptr))
        published = false;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (published && !board::view_movie_visible()) {
      player_idle = true;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    player_idle = false;
    uint32_t epoch = 0;
    const int id = choose_next(epoch);
    if (id < 0) {
      if (published && board::view_movie_frame(nullptr))
        published = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    const auto &clip = assets::CLIPS[id];
    Film film;
    const auto load_begin = esp_timer_get_time();
    bool loaded = false;
    if (storage_available() && !card_failed[id]) {
      const auto result = storage_read(id, compressed, clip.bytes, [] {
        return photo_paused.load() || !enabled.load();
      });
      if (result == ReadResult::cancelled)
        continue; // A temporary pause must not blacklist a valid movie.
      loaded = result == ReadResult::ok && film.parse(compressed, clip.bytes);
      card_failed[id] = !loaded;
    }
    const size_t blob_bytes = fallback_end - fallback_start;
    if (!loaded && clip.length == clip.bytes && clip.offset <= blob_bytes &&
        clip.length <= blob_bytes - clip.offset) {
      memcpy(compressed, fallback_start + clip.offset, clip.length);
      loaded = film.parse(compressed, clip.bytes);
    }
    if (!loaded) {
      ESP_LOGW(TAG, "Clip %d unavailable or invalid; disabled for this boot",
               id);
      portENTER_CRITICAL(&lock);
      playable[id] = false;
      portEXIT_CRITICAL(&lock);
      continue;
    }
    ESP_LOGI(TAG, "Loaded clip=%d bytes=%u load_ms=%u", id,
             unsigned(clip.bytes),
             unsigned((esp_timer_get_time() - load_begin) / 1000));
    current = id;
    history.add(id, clip.source);
    ++selected_count;
    const int64_t begin = esp_timer_get_time();
    unsigned frame = 0, decoded = 0, late = 0;
    uint64_t decode_us = 0;
    while (frame < film.count && enabled.load() && !photo_paused.load()) {
      // A new association can cut after a short dwell instead of waiting up to
      // another entire clip. Transcript throttling prevents rapid visual churn.
      if (scene_epoch.load() != epoch && frame >= film.fps * 2)
        break;
      // Do no decoding behind the camera preview, usage page or test overlay.
      if (frame > 1 && published && !board::view_movie_visible()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        break;
      }
      const int64_t due = begin + int64_t(frame) * 1000000 / film.fps;
      const auto wait = due - esp_timer_get_time();
      if (wait > 1000)
        vTaskDelay(pdMS_TO_TICKS(wait / 1000));
      const unsigned target =
          frame_at(esp_timer_get_time() - begin, film.fps, film.count);
      if (target > frame) {
        late += target - frame;
        frame = target;
      }
      if (frame >= film.count)
        break;
      const auto &f = film.frames[frame];
      jpeg_dec_io_t io{};
      io.inbuf = compressed + f.offset;
      io.inbuf_len = f.size;
      io.outbuf = pixels[back];
      jpeg_dec_header_info_t header{};
      int required = 0;
      const int64_t start = esp_timer_get_time();
      ok = jpeg_dec_parse_header(decoder, &io, &header) == JPEG_ERR_OK &&
           header.width == 320 && header.height == 240 &&
           jpeg_dec_get_outbuf_len(decoder, &required) == JPEG_ERR_OK &&
           required == int(PIXEL_BYTES) &&
           jpeg_dec_process(decoder, &io) == JPEG_ERR_OK;
      if (!ok) {
        ++decode_failures;
        portENTER_CRITICAL(&lock);
        playable[id] = false;
        portEXIT_CRITICAL(&lock);
        break;
      }
      decode_us += esp_timer_get_time() - start;
      ++decoded;
      if (!enabled.load())
        break;
      if (board::view_movie_frame(pixels[back], back)) {
        back ^= 1;
        published = true;
        ++frames_shown;
      } else
        ++late;
      ++frame;
    }
    frames_late.fetch_add(late);
    if (decoded > 1)
      ESP_LOGI(TAG, "Clip=%d decoded=%u late=%u decode_avg_us=%u", id, decoded,
               late, unsigned(decode_us / decoded));
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
} // namespace

void before_display() { storage_init(); }
void init() {
  playable = static_cast<std::atomic<bool> *>(heap_caps_malloc(
      sizeof(*playable) * assets::COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!playable)
    return;
  for (unsigned i = 0; i < assets::COUNT; ++i)
    new (playable + i)
        std::atomic<bool>(storage_available() || assets::CLIPS[i].length != 0);
  if (xTaskCreatePinnedToCoreWithCaps(
          queries, "cinema_queries", 16384, nullptr, 1, &query_task, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return;
  enabled = true;
  if (xTaskCreatePinnedToCoreWithCaps(
          play, "cinema_player", 12288, nullptr, 2, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    enabled = false;
    vTaskDeleteWithCaps(query_task);
    query_task = nullptr;
    return;
  }
  initialized = true;
}
bool available() { return initialized.load() && !player_failed.load(); }
bool pause_for_photo() {
  photo_paused = true;
  const auto deadline = esp_timer_get_time() + 2000000;
  while (available() && !player_idle.load() && esp_timer_get_time() < deadline)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (available() && !player_idle.load()) {
    photo_paused = false;
    return false;
  }
  return true;
}
void resume_after_photo() { photo_paused = false; }
void tick() {
  if (!initialized.load())
    return;
  static bool active = false;
  static uint32_t observed = 0, submitted = 0;
  static int64_t changed = 0, last = 0;
  live_esp32_status_t state{};
  live_esp32_status(&state);
  const auto now = esp_timer_get_time();
  if (active && !state.active) {
    portENTER_CRITICAL(&lock);
    mailbox.invalidate();
    have_ranking = false;
    portEXIT_CRITICAL(&lock);
  }
  if (!active && state.active) {
    submitted = UINT32_MAX;
    last = now - 10000000;
  }
  active = state.active;
  if (!active || !enabled.load())
    return;
  if (state.transcript_revision != observed) {
    observed = state.transcript_revision;
    changed = now;
  }
  if (observed == submitted || now - last < 10000000 ||
      (now - changed < 700000 && now - last < 15000000))
    return;
  live_transcript_t transcript{};
  live_esp32_transcripts(&transcript);
  if (!transcript.count)
    return;
  char cue_text[257]{};
  // The latest actual words, not the source film titles, drive the association.
  const auto &block = transcript.blocks[transcript.count - 1];
  transcript_cue(block.text, cue_text);
  if (strlen(cue_text) < 16)
    return;
  if (enqueue(cue_text, false)) {
    submitted = observed;
    last = now;
  }
}
esp_err_t cue(const char *arguments, char *result, size_t capacity) {
  char text[257];
  if (!result || capacity < 150 || !parse_cue(arguments, text))
    return ESP_ERR_INVALID_ARG;
  const bool queued = enqueue(text, true);
  if (!queued)
    return ESP_ERR_INVALID_STATE;
  snprintf(result, capacity,
           "{\"queued\":true,\"meaning\":\"An archival visual association, not "
           "a current camera view or factual evidence.\"}");
  return ESP_OK;
}
bool command(const char *line) {
  if (strncmp(line, "cinema ", 7))
    return false;
  if (!strncmp(line + 7, "sd ", 3)) {
    live_esp32_status_t state{};
    live_esp32_status(&state);
    if (enabled.load() || state.busy || !player_idle.load()) {
      printf("CINEMA ERROR stop-live-and-cinema-first\n");
      return true;
    }
    return storage_command(line + 10);
  }
  if (!strcmp(line + 7, "off")) {
    enabled = false;
    board::view_movie_frame(nullptr);
    portENTER_CRITICAL(&lock);
    mailbox.invalidate();
    have_ranking = false;
    portEXIT_CRITICAL(&lock);
  } else if (!strcmp(line + 7, "on"))
    enabled = available();
  else if (!strncmp(line + 7, "cue ", 4))
    enqueue(line + 11, true);
  else if (strcmp(line + 7, "status"))
    ESP_LOGW(TAG, "Commands: cinema on/off/status/cue <description>");
  unsigned usable = 0;
  uint32_t rendered;
  int64_t gap;
  board::view_movie_stats(rendered, gap);
  if (playable)
    for (unsigned i = 0; i < assets::COUNT; ++i)
      usable += playable[i].load();
  ESP_LOGI(TAG,
           "enabled=%d sd=%d catalog=%u playable=%u current=%d clips=%u "
           "frames=%u rendered=%lu max_gap_ms=%lld late=%u decode_errors=%u "
           "requests=%u failures=%u",
           enabled.load(), storage_available(), assets::COUNT, usable,
           current.load(), selected_count.load(), frames_shown.load(),
           (unsigned long)rendered, gap / 1000, frames_late.load(),
           decode_failures.load(), requests.load(), failures.load());
  static Runtime previous;
  static int64_t previous_time = 0;
  const auto wall_time = esp_timer_get_time();
  const auto now = runtime();
  const double elapsed = uint32_t(now.total - previous.total);
  if (previous.total && now.total && elapsed > 0 &&
      wall_time - previous_time < 1800000000)
    ESP_LOGI(TAG,
             "cpu_window core0_busy=%.1f%% core1_busy=%.1f%% player=%.1f%% "
             "gui=%.1f%% query=%.1f%%",
             100. * (1. - uint32_t(now.idle0 - previous.idle0) / elapsed),
             100. * (1. - uint32_t(now.idle1 - previous.idle1) / elapsed),
             100. * uint32_t(now.player - previous.player) / elapsed,
             100. * uint32_t(now.gui - previous.gui) / elapsed,
             100. * uint32_t(now.query - previous.query) / elapsed);
  previous = now;
  previous_time = wall_time;
  return true;
}
} // namespace cinema
