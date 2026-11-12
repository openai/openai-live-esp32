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
bool ranking_fresh = false;
int64_t ranking_expires = 0;
std::atomic<bool> *playable = nullptr;
History history;
TaskHandle_t query_task = nullptr;
struct Trace {
  int64_t time_us = 0;
  uint32_t sequence = 0, epoch = 0;
  int clip = -1;
  char source = '?';
  char text[257]{};
  Ranking candidates;
};
constexpr unsigned TRACE_COUNT = 64;
Trace *traces = nullptr;
unsigned trace_count = 0, trace_sequence = 0;
bool tracing = false;
// Caller holds lock. Traces are opt-in, bounded PSRAM, never written to SD.
void trace_locked(uint32_t epoch, int clip, char source, const char *text,
                  const Ranking &candidates) {
  if (!tracing || !traces)
    return;
  auto &entry = traces[trace_sequence % TRACE_COUNT];
  entry = Trace{};
  entry.time_us = esp_timer_get_time();
  entry.sequence = ++trace_sequence;
  entry.epoch = epoch;
  entry.clip = clip;
  entry.source = source;
  if (text)
    snprintf(entry.text, sizeof(entry.text), "%s", text);
  entry.candidates = candidates;
  trace_count = std::min(trace_count + 1, TRACE_COUNT);
}

void dump_trace() {
  unsigned count, end;
  portENTER_CRITICAL(&lock);
  count = trace_count;
  end = trace_sequence;
  portEXIT_CRITICAL(&lock);
  for (unsigned n = 0; n < count; ++n) {
    const unsigned seq = end - count + n + 1;
    Trace entry;
    portENTER_CRITICAL(&lock);
    entry = traces[(seq - 1) % TRACE_COUNT];
    portEXIT_CRITICAL(&lock);
    if (entry.sequence != seq)
      continue;
    cJSON *root = cJSON_CreateObject();
    if (!root)
      return;
    cJSON_AddNumberToObject(root, "sequence", seq);
    cJSON_AddNumberToObject(root, "ms", entry.time_us / 1000);
    cJSON_AddNumberToObject(root, "epoch", entry.epoch);
    cJSON_AddStringToObject(root, "kind", entry.clip < 0 ? "query" : "played");
    if (entry.clip >= 0)
      cJSON_AddNumberToObject(root, "clip", entry.clip);
    else {
      cJSON_AddStringToObject(root, "source",
                              entry.source == 'u'   ? "user"
                              : entry.source == 'a' ? "assistant"
                                                    : "manual/tool");
      cJSON_AddStringToObject(root, "cue", entry.text);
      cJSON *candidates = cJSON_AddArrayToObject(root, "candidates");
      for (unsigned i = 0; candidates && i < entry.candidates.count; ++i) {
        auto *candidate = cJSON_CreateObject();
        if (!candidate)
          break;
        cJSON_AddNumberToObject(candidate, "clip", entry.candidates.best[i].id);
        cJSON_AddNumberToObject(candidate, "score",
                                entry.candidates.best[i].score);
        cJSON_AddItemToArray(candidates, candidate);
      }
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
      printf("CINEMA TRACE %s\n", json);
      cJSON_free(json);
    }
    cJSON_Delete(root);
    vTaskDelay(1);
  }
}
struct Runtime {
  uint32_t total = 0, idle0 = 0, idle1 = 0, player = 0, gui = 0, query = 0,
           loader = 0;
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
    if (!strcmp(task.pcTaskName, "cinema_loader"))
      value.loader = task.ulRunTimeCounter;
    if (strstr(task.pcTaskName, "LVGL") || strstr(task.pcTaskName, "lvgl"))
      value.gui += task.ulRunTimeCounter;
  }
  if (!count)
    value.total = 0;
  return value;
}

bool enqueue(const char *text, bool priority, char source = 't') {
  if (!initialized.load() || !enabled.load() || player_failed.load())
    return false;
  const int64_t now = esp_timer_get_time();
  bool queued = false;
  portENTER_CRITICAL(&lock);
  queued = mailbox.submit(text, now, priority, source);
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
    char source = '?';
    portENTER_CRITICAL(&lock);
    mailbox.take(cue, generation, &source);
    portEXIT_CRITICAL(&lock);
    if (!cue[0])
      continue;
    ++requests;
    const int64_t began = esp_timer_get_time();
    const bool ok = embed(cue, query);
    next_request = esp_timer_get_time() + 2000000;
    if (!ok) {
      ++failures;
      portENTER_CRITICAL(&lock);
      if (mailbox.accepts(generation)) {
        have_ranking = ranking_fresh = false;
        trace_locked(++scene_epoch, -1, source, cue, Ranking{});
      }
      portEXIT_CRITICAL(&lock);
      ESP_LOGW(TAG,
               "Cue has no recognized vocabulary; offline dreams continue");
      continue;
    }
    uint16_t query_words[16];
    const unsigned word_count = keywords(cue, assets::WORDS, assets::WORD_COUNT,
                                         assets::WORD_STRINGS, query_words, 16);
    constexpr unsigned document_words =
        sizeof(assets::CLIPS[0].keywords) / sizeof(uint16_t);
    Candidates<64> semantic;
    Candidates<32> lexical;
    for (unsigned i = 0; i < assets::COUNT; ++i) {
      if ((i & 127) == 127)
        vTaskDelay(1);
      const bool usable = playable[i].load();
      if (usable) {
        semantic.add(i,
                     cosine(query, assets::VECTORS[i], assets::CLIPS[i].norm,
                            assets::DIMENSIONS),
                     assets::CLIPS[i].source);
        const float exact =
            exact_coverage(query_words, word_count, assets::CLIPS[i].keywords,
                           document_words, assets::WORDS, assets::WORD_COUNT);
        if (exact > 0)
          lexical.add(i, exact, assets::CLIPS[i].source);
      }
    }
    Candidates<96> coarse;
    for (unsigned i = 0; i < semantic.count; ++i)
      coarse.add(semantic.best[i].id, semantic.best[i].score,
                 semantic.best[i].source);
    for (unsigned i = 0; i < lexical.count; ++i) {
      const auto &c = lexical.best[i];
      bool present = false;
      for (unsigned j = 0; j < semantic.count; ++j)
        present |= semantic.best[j].id == c.id;
      if (!present)
        coarse.add(c.id,
                   cosine(query, assets::VECTORS[c.id],
                          assets::CLIPS[c.id].norm, assets::DIMENSIONS),
                   c.source);
    }
    Ranking next;
    for (unsigned i = 0; i < coarse.count; ++i) {
      const auto &c = coarse.best[i];
      const float detail = keyword_similarity(
          query_words, word_count, assets::CLIPS[c.id].keywords, document_words,
          assets::WORDS, assets::WORD_COUNT, assets::DIMENSIONS);
      const float exact =
          exact_coverage(query_words, word_count, assets::CLIPS[c.id].keywords,
                         document_words, assets::WORDS, assets::WORD_COUNT);
      next.add(c.id, .25f * c.score + .6f * detail + .15f * exact, c.source);
      if ((i & 7) == 7)
        vTaskDelay(1);
    }
    bool accepted = false;
    portENTER_CRITICAL(&lock);
    // A newer cue, session end or cinema-off invalidates an in-flight result.
    if (mailbox.accepts(generation) && enabled.load()) {
      ranking = next;
      have_ranking = next.count && next.best[0].score >= MIN_RELEVANCE;
      ranking_fresh = have_ranking;
      ranking_expires = esp_timer_get_time() + 45000000;
      accepted = true;
      trace_locked(++scene_epoch, -1, source, cue, next);
    }
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG,
             "Cue %s candidates=%u best=%d score=%.3f latency_ms=%lld close=%d",
             accepted ? "matched" : "superseded", next.count,
             next.count ? next.best[0].id : -1,
             next.count ? next.best[0].score : 0.f,
             (esp_timer_get_time() - began) / 1000,
             next.count && next.best[0].score >= MIN_RELEVANCE);
  }
}

int choose_next(uint32_t &epoch) {
  Ranking next;
  bool fresh = false;
  const auto now = esp_timer_get_time();
  portENTER_CRITICAL(&lock);
  epoch = scene_epoch.load();
  if (have_ranking && now < ranking_expires) {
    next = ranking;
    fresh = ranking_fresh;
  }
  portEXIT_CRITICAL(&lock);
  Ranking available;
  for (unsigned i = 0; i < next.count; ++i) {
    const auto &c = next.best[i];
    if (playable[c.id].load() && c.score >= MIN_RELEVANCE)
      available.add(c.id, c.score, c.source);
  }
  next = available;
  if (fresh && next.count)
    return next.best[0].id;
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

enum class LoadState { idle, requested, loading, ready };
struct LoadJob {
  std::atomic<LoadState> state{LoadState::idle};
  std::atomic<bool> cancel{false};
  uint8_t *buffer = nullptr;
  bool *card_failed = nullptr;
  unsigned slot = 0;
  int id = -1;
  uint32_t epoch = 0;
  bool ok = false, retry = false;
  Film film;
};
LoadJob *load_job = nullptr;
TaskHandle_t loader_task = nullptr;
std::atomic<unsigned> boundary_max_ms{0}, looped_frames{0};

void load_movie(void *) {
  auto &job = *load_job;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    auto expected = LoadState::requested;
    if (!job.state.compare_exchange_strong(expected, LoadState::loading))
      continue;
    const int id = job.id;
    const auto &clip = assets::CLIPS[id];
    const auto began = esp_timer_get_time();
    job.ok = false;
    job.retry = false;
    if (!job.cancel.load() && storage_available() && !job.card_failed[id]) {
      const auto result = storage_read(id, job.buffer, clip.bytes, [] {
        return photo_paused.load() || !enabled.load() ||
               load_job->cancel.load();
      });
      // A pause/new cue can race the final SD operation. Do not remember a
      // cancelled read as a permanently bad card file.
      if (result == ReadResult::cancelled || job.cancel.load() ||
          photo_paused.load() || !enabled.load())
        job.cancel = true;
      else if (result == ReadResult::retry)
        job.retry = true;
      else {
        job.ok =
            result == ReadResult::ok && job.film.parse(job.buffer, clip.bytes);
        job.card_failed[id] = !job.ok;
      }
    }
    const size_t blob_bytes = fallback_end - fallback_start;
    if (!job.cancel.load() && !job.ok && clip.length == clip.bytes &&
        clip.offset <= blob_bytes && clip.length <= blob_bytes - clip.offset) {
      memcpy(job.buffer, fallback_start + clip.offset, clip.length);
      job.ok = job.film.parse(job.buffer, clip.bytes);
      if (job.ok)
        job.retry = false;
    }
    if (!job.cancel.load())
      ESP_LOGI(TAG, "Prefetched clip=%d bytes=%u load_ms=%u valid=%d retry=%d",
               id, unsigned(clip.bytes),
               unsigned((esp_timer_get_time() - began) / 1000), job.ok,
               job.retry);
    // All fields and compressed bytes become readable before ready is observed.
    // Only the player returns this slot to idle; it never overwrites a live
    // job.
    job.state.store(LoadState::ready, std::memory_order_release);
  }
}

void play(void *) {
  size_t capacity = 0;
  for (auto &clip : assets::CLIPS)
    capacity = std::max(capacity, size_t(clip.bytes));
  uint8_t *compressed[2]{};
  uint8_t *pixels[2]{};
  for (unsigned i = 0; i < 2; ++i) {
    if (capacity <= MAX_FILM)
      compressed[i] = static_cast<uint8_t *>(
          heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pixels[i] = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, PIXEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  void *job_memory =
      heap_caps_malloc(sizeof(LoadJob), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (job_memory)
    load_job = new (job_memory) LoadJob;
  if (load_job)
    load_job->card_failed = static_cast<bool *>(heap_caps_calloc(
        assets::COUNT, sizeof(bool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  jpeg_dec_handle_t decoder = nullptr;
  bool ok = compressed[0] && compressed[1] && pixels[0] && pixels[1] &&
            load_job && load_job->card_failed &&
            jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK;
  if (ok)
    // Use the other core's spare time; microphone/speaker tasks remain higher
    // priority. CPU1 already carries Live control, GUI, decode and search work.
    ok = xTaskCreatePinnedToCoreWithCaps(
             load_movie, "cinema_loader", 6144, nullptr, 2, &loader_task, 0,
             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
  if (!ok) {
    ESP_LOGE(TAG, "Player allocation failed; captions remain available");
    for (unsigned i = 0; i < 2; ++i) {
      free(compressed[i]);
      free(pixels[i]);
    }
    if (decoder)
      jpeg_dec_close(decoder);
    if (load_job) {
      free(load_job->card_failed);
      load_job->~LoadJob();
      free(load_job);
      load_job = nullptr;
    }
    player_failed = true;
    enabled = false;
    vTaskDeleteWithCaps(nullptr);
    return;
  }
  ESP_LOGI(TAG,
           "Player ready clips=%u compressed_capacity=%u buffers=2 pixels=%u "
           "fps=12 audio=none",
           assets::COUNT, unsigned(capacity), PIXEL_BYTES * 2);
  auto &job = *load_job;
  Film film;
  int active_id = -1;
  unsigned active_slot = 0, back = 0;
  uint32_t active_epoch = 0;
  uint64_t tick = 0;
  int64_t begin = 0, last_presented = 0, paused_at = 0;
  bool published = false, boundary = false, seen_visible = false;
  for (;;) {
    auto state = job.state.load(std::memory_order_acquire);
    const bool hidden = visibility_paused(
        enabled.load(), board::view_movie_visible(), seen_visible);
    if (!enabled.load() || photo_paused.load() || hidden) {
      job.cancel = true;
      if (state == LoadState::ready) {
        job.state = LoadState::idle;
        state = LoadState::idle;
      }
      // The camera/upload barrier includes pending SD reads, not only decoding.
      player_idle = state == LoadState::idle;
      if (!paused_at)
        paused_at = esp_timer_get_time();
      // Retain the active film across a photo/usage overlay. Restarting it
      // would require another SD read and freeze on the first resumed frame.
      if (!enabled.load())
        active_id = -1;
      last_presented = 0;
      if (!enabled.load() && published && board::view_movie_frame(nullptr))
        published = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (paused_at) {
      begin += esp_timer_get_time() - paused_at;
      paused_at = 0;
    }
    player_idle = false;
    const auto epoch = scene_epoch.load();
    if (state != LoadState::idle && job.epoch != epoch)
      job.cancel = true;
    if (state == LoadState::ready) {
      if (job.cancel.load() || job.retry || !job.ok ||
          !playable[job.id].load()) {
        if (!job.cancel.load() && !job.retry) {
          playable[job.id] = false;
          ESP_LOGW(TAG,
                   "Clip %d unavailable or invalid; disabled for this boot",
                   job.id);
        }
        job.state = LoadState::idle;
        state = LoadState::idle;
      } else if (active_id < 0 || transition_due(tick, film.fps, film.count,
                                                 active_epoch != epoch)) {
        boundary = active_id >= 0;
        active_id = job.id;
        active_slot = job.slot;
        active_epoch = job.epoch;
        film = job.film;
        job.state = LoadState::idle;
        state = LoadState::idle;
        current = active_id;
        history.add(active_id, assets::CLIPS[active_id].source);
        ++selected_count;
        portENTER_CRITICAL(&lock);
        if (active_epoch == scene_epoch.load())
          ranking_fresh = false;
        trace_locked(active_epoch, active_id, '?', nullptr, Ranking{});
        portEXIT_CRITICAL(&lock);
        begin = esp_timer_get_time();
        tick = 0;
        ESP_LOGI(TAG, "Playing clip=%d epoch=%lu", active_id,
                 (unsigned long)active_epoch);
      }
    }
    if (state == LoadState::idle) {
      uint32_t next_epoch = 0;
      const int next = choose_next(next_epoch);
      if (next >= 0) {
        job.id = next;
        job.epoch = next_epoch;
        job.cancel = false;
        job.ok = false;
        job.retry = false;
        if (next == active_id) {
          job.slot = active_slot;
          job.film = film;
          job.ok = true;
          job.state.store(LoadState::ready, std::memory_order_release);
        } else {
          job.slot = active_id < 0 ? 0 : active_slot ^ 1;
          job.buffer = compressed[job.slot];
          job.state.store(LoadState::requested, std::memory_order_release);
          xTaskNotifyGive(loader_task);
        }
      }
    }
    if (active_id < 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const int64_t due = begin + int64_t(tick) * 1000000 / film.fps;
    const auto wait = due - esp_timer_get_time();
    if (wait > 1000)
      vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    if (!enabled.load() || photo_paused.load())
      continue;
    const auto target =
        uint64_t(std::max<int64_t>(0, esp_timer_get_time() - begin)) *
        film.fps / 1000000;
    if (target > tick) {
      frames_late.fetch_add(unsigned(target - tick));
      tick = target;
    }
    // A slow card repeats moving frames instead of holding the final still.
    // Switch as soon as the validated spare buffer is ready after the dwell.
    if (tick >= film.count)
      ++looped_frames;
    const auto &frame = film.frames[loop_frame(tick, film.count)];
    jpeg_dec_io_t io{};
    io.inbuf = compressed[active_slot] + frame.offset;
    io.inbuf_len = frame.size;
    io.outbuf = pixels[back];
    jpeg_dec_header_info_t header{};
    int required = 0;
    ok = jpeg_dec_parse_header(decoder, &io, &header) == JPEG_ERR_OK &&
         header.width == 320 && header.height == 240 &&
         jpeg_dec_get_outbuf_len(decoder, &required) == JPEG_ERR_OK &&
         required == int(PIXEL_BYTES) &&
         jpeg_dec_process(decoder, &io) == JPEG_ERR_OK;
    if (!ok) {
      ++decode_failures;
      playable[active_id] = false;
      active_id = -1;
      continue;
    }
    if (!enabled.load() || photo_paused.load())
      continue;
    if (board::view_movie_frame(pixels[back], back)) {
      const auto presented = esp_timer_get_time();
      if (boundary && last_presented)
        boundary_max_ms =
            std::max(boundary_max_ms.load(),
                     unsigned((presented - last_presented) / 1000));
      boundary = false;
      last_presented = presented;
      back ^= 1;
      published = true;
      ++frames_shown;
    } else
      ++frames_late;
    ++tick;
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
  // Share time with playback so complex clips cannot starve a pending search.
  // Both remain below the audio tasks; search also yields between batches.
  constexpr UBaseType_t background_priority = 2;
  if (xTaskCreatePinnedToCoreWithCaps(
          queries, "cinema_queries", 16384, nullptr, background_priority,
          &query_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return;
  enabled = true;
  if (xTaskCreatePinnedToCoreWithCaps(
          play, "cinema_player", 12288, nullptr, background_priority, nullptr,
          1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
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
    have_ranking = ranking_fresh = false;
    ++scene_epoch;
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
  if (observed == submitted || now - last < 5000000 ||
      (now - changed < 500000 && now - last < 8000000))
    return;
  live_transcript_t transcript{};
  live_esp32_transcripts(&transcript);
  if (!transcript.count)
    return;
  char cue_text[257]{};
  // The latest actual words, not the source film titles, drive the association.
  // Block creation order is not update order when speakers overlap.
  unsigned index = transcript.count - 1;
  for (unsigned i = transcript.count; i; --i)
    if (transcript.blocks[i - 1].speaker == transcript.last_speaker) {
      index = i - 1;
      break;
    }
  const auto &block = transcript.blocks[index];
  transcript_cue(block.text, cue_text);
  if (strlen(cue_text) < 3)
    return;
  if (enqueue(cue_text, false,
              block.speaker == LIVE_SPEAKER_USER ? 'u' : 'a')) {
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
  if (!strncmp(line + 7, "trace ", 6)) {
    const char *operation = line + 13;
    if (!strcmp(operation, "dump"))
      dump_trace();
    else if (!strcmp(operation, "on") || !strcmp(operation, "off") ||
             !strcmp(operation, "clear")) {
      if (!strcmp(operation, "on") && !traces) {
        void *memory = heap_caps_malloc(sizeof(Trace) * TRACE_COUNT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) {
          traces = static_cast<Trace *>(memory);
          for (unsigned i = 0; i < TRACE_COUNT; ++i)
            new (traces + i) Trace;
        }
      }
      portENTER_CRITICAL(&lock);
      if (!strcmp(operation, "clear")) {
        trace_count = trace_sequence = 0;
        if (traces)
          for (unsigned i = 0; i < TRACE_COUNT; ++i)
            traces[i] = Trace{};
      } else
        tracing = !strcmp(operation, "on") && traces;
      portEXIT_CRITICAL(&lock);
      ESP_LOGI(TAG, "Trace %s; RAM only, capacity=%u; dump/clear available",
               tracing ? "on" : "off", TRACE_COUNT);
    } else
      ESP_LOGW(TAG, "cinema trace on/off/dump/clear");
    return true;
  }
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
    have_ranking = ranking_fresh = false;
    ++scene_epoch;
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
  ESP_LOGI(
      TAG,
      "enabled=%d sd=%d catalog=%u playable=%u current=%d clips=%u "
      "frames=%u rendered=%lu max_gap_ms=%lld late=%u decode_errors=%u "
      "requests=%u failures=%u boundary_max_ms=%u looped_frames=%u sd_waits=%u",
      enabled.load(), storage_available(), assets::COUNT, usable,
      current.load(), selected_count.load(), frames_shown.load(),
      (unsigned long)rendered, gap / 1000, frames_late.load(),
      decode_failures.load(), requests.load(), failures.load(),
      boundary_max_ms.load(), looped_frames.load(), storage_contention());
  static Runtime previous;
  static int64_t previous_time = 0;
  const auto wall_time = esp_timer_get_time();
  const auto now = runtime();
  const double elapsed = uint32_t(now.total - previous.total);
  if (previous.total && now.total && elapsed > 0 &&
      wall_time - previous_time < 1800000000)
    ESP_LOGI(TAG,
             "cpu_window core0_busy=%.1f%% core1_busy=%.1f%% player=%.1f%% "
             "gui=%.1f%% query=%.1f%% loader=%.1f%%",
             100. * (1. - uint32_t(now.idle0 - previous.idle0) / elapsed),
             100. * (1. - uint32_t(now.idle1 - previous.idle1) / elapsed),
             100. * uint32_t(now.player - previous.player) / elapsed,
             100. * uint32_t(now.gui - previous.gui) / elapsed,
             100. * uint32_t(now.query - previous.query) / elapsed,
             100. * uint32_t(now.loader - previous.loader) / elapsed);
  previous = now;
  previous_time = wall_time;
  return true;
}
} // namespace cinema
