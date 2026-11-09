#include "Cinema.hpp"
#include "Board.hpp"
#include "CinemaCue.hpp"
#include "CinemaPolicy.hpp"
#include "CinemaStorage.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

#ifdef STACKCHAN_CINEMA
#include "CinemaAssets.hpp"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <atomic>
#include <cstdlib>

extern "C" const uint8_t fallback_start[] asm("_binary_fallback_bin_start");
extern "C" const uint8_t fallback_end[] asm("_binary_fallback_bin_end");
namespace cinema {
namespace {
constexpr char TAG[] = "Cinema";
static_assert(assets::COUNT > 0 && assets::COUNT <= 1000 &&
                  assets::DIMENSIONS == 256,
              "Prepare a 1..1000-clip, 256-dimensional cinema pack");
const char *key = nullptr;
std::atomic<bool> enabled{false}, initialized{false};
std::atomic<bool> player_failed{false};
std::atomic<unsigned> requests{0}, failures{0}, selected_count{0};
std::atomic<unsigned> frames_shown{0}, frames_late{0}, decode_failures{0};
std::atomic<int> current{-1};
std::atomic<uint32_t> scene_epoch{0};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Mailbox mailbox;
Ranking ranking;
bool have_ranking = false;
int64_t ranking_expires = 0;
bool playable[assets::COUNT]{};
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

struct Response {
  char *data = nullptr;
  size_t used = 0;
  bool overflow = false;
  static constexpr size_t CAPACITY = 16384;
};
esp_err_t http_event(esp_http_client_event_t *event) {
  auto &response = *static_cast<Response *>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    if (event->data_len < 0 ||
        size_t(event->data_len) > Response::CAPACITY - 1 - response.used) {
      response.overflow = true;
      return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response.data + response.used, event->data, event->data_len);
    response.used += event->data_len;
    response.data[response.used] = 0;
  }
  return ESP_OK;
}
bool embed(const char *text, float *output) {
  Response response;
  response.data = static_cast<char *>(heap_caps_malloc(
      Response::CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!response.data)
    return false;
  response.data[0] = 0;
  cJSON *body = cJSON_CreateObject();
  bool ok = body && cJSON_AddStringToObject(body, "model", assets::MODEL) &&
            cJSON_AddNumberToObject(body, "dimensions", assets::DIMENSIONS) &&
            cJSON_AddStringToObject(body, "encoding_format", "base64") &&
            cJSON_AddStringToObject(body, "input", text);
  char *payload = ok ? cJSON_PrintUnformatted(body) : nullptr;
  cJSON_Delete(body);
  esp_http_client_config_t config{};
  config.url = "https://api.openai.com/v1/embeddings";
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = http_event;
  config.user_data = &response;
  config.disable_auto_redirect = true;
  auto *client = payload ? esp_http_client_init(&config) : nullptr;
  if (client) {
    // API key is passed directly from private configuration, never logged.
    const size_t n = strlen(key) + 8;
    auto *authorization = static_cast<char *>(malloc(n));
    ok = authorization != nullptr;
    if (ok) {
      snprintf(authorization, n, "Bearer %s", key);
      ok = esp_http_client_set_header(client, "Authorization", authorization) ==
               ESP_OK &&
           esp_http_client_set_header(client, "Content-Type",
                                      "application/json") == ESP_OK &&
           esp_http_client_set_post_field(client, payload, strlen(payload)) ==
               ESP_OK;
    }
    free(authorization);
    if (ok)
      ok = esp_http_client_perform(client) == ESP_OK && !response.overflow &&
           esp_http_client_get_status_code(client) == 200;
    esp_http_client_cleanup(client);
  } else
    ok = false;
  cJSON_free(payload);
  cJSON *root =
      ok ? cJSON_ParseWithOpts(response.data, nullptr, true) : nullptr;
  auto *model = cJSON_GetObjectItemCaseSensitive(root, "model");
  auto *data = cJSON_GetObjectItemCaseSensitive(root, "data");
  auto *row = cJSON_GetArrayItem(data, 0);
  auto *index = cJSON_GetObjectItemCaseSensitive(row, "index");
  auto *value = cJSON_GetObjectItemCaseSensitive(row, "embedding");
  ok = cJSON_IsString(model) && !strcmp(model->valuestring, assets::MODEL) &&
       cJSON_IsArray(data) && cJSON_GetArraySize(data) == 1 &&
       cJSON_IsNumber(index) && index->valuedouble == 0 &&
       cJSON_IsString(value);
  if (ok) {
    static_assert(sizeof(float) == 4, "OpenAI base64 embeddings use float32");
    size_t bytes = 0;
    // ESP32-S3 is little endian, matching the API's float32 representation.
    ok = mbedtls_base64_decode(
             reinterpret_cast<uint8_t *>(output), assets::DIMENSIONS * 4,
             &bytes, reinterpret_cast<const uint8_t *>(value->valuestring),
             strlen(value->valuestring)) == 0 &&
         bytes == assets::DIMENSIONS * 4 &&
         normalize(output, assets::DIMENSIONS);
  }
  cJSON_Delete(root);
  free(response.data);
  return ok;
}

void queries(void *) {
  float query[assets::DIMENSIONS]{};
  int64_t next_request = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (!enabled.load() || !board::wifi_ready() ||
        esp_timer_get_time() < next_request)
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
    next_request = esp_timer_get_time() + (ok ? 8000000 : 30000000);
    if (!ok) {
      ++failures;
      ESP_LOGW(TAG, "Cue embedding unavailable; offline dreams continue");
      continue;
    }
    Ranking next;
    for (unsigned i = 0; i < assets::COUNT; ++i) {
      bool usable;
      portENTER_CRITICAL(&lock);
      usable = playable[i];
      portEXIT_CRITICAL(&lock);
      if (usable)
        next.add(i,
                 cosine(query, assets::VECTORS[i], assets::CLIPS[i].norm,
                        assets::DIMENSIONS),
                 assets::CLIPS[i].source);
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
    ESP_LOGI(TAG, "Cue %s candidates=%u latency_ms=%lld",
             accepted ? "matched" : "superseded", next.count,
             (esp_timer_get_time() - began) / 1000);
  }
}

int choose_next() {
  Ranking next;
  const auto now = esp_timer_get_time();
  portENTER_CRITICAL(&lock);
  if (have_ranking && now < ranking_expires) {
    for (unsigned i = 0; i < ranking.count; ++i) {
      const auto &c = ranking.best[i];
      if (playable[c.id])
        next.add(c.id, c.score, c.source);
    }
  }
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
  portEXIT_CRITICAL(&lock);
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
  bool card_failed[assets::COUNT]{};
  unsigned back = 0;
  for (;;) {
    if (!enabled.load()) {
      if (published && board::view_movie_frame(nullptr))
        published = false;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (published && !board::view_movie_visible()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    const int id = choose_next();
    if (id < 0) {
      if (published && board::view_movie_frame(nullptr))
        published = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    const auto &clip = assets::CLIPS[id];
    Film film;
    bool loaded = false;
    if (storage_available() && !card_failed[id]) {
      loaded = storage_read(id, compressed, clip.bytes) &&
               film.parse(compressed, clip.bytes);
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
    current = id;
    history.add(id, clip.source);
    ++selected_count;
    const int64_t begin = esp_timer_get_time();
    const auto epoch = scene_epoch.load();
    unsigned frame = 0, decoded = 0, late = 0;
    uint64_t decode_us = 0;
    while (frame < film.count && enabled.load()) {
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
void init(const char *api_key) {
  key = api_key;
  for (unsigned i = 0; i < assets::COUNT; ++i)
    playable[i] = storage_available() || assets::CLIPS[i].length != 0;
  if (!key || !*key ||
      xTaskCreatePinnedToCoreWithCaps(
          queries, "cinema_queries", 16384, nullptr, 1, &query_task, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return;
  enabled = true;
  if (xTaskCreatePinnedToCoreWithCaps(
          play, "cinema_player", 12288, nullptr, 2, nullptr, 0,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    enabled = false;
    vTaskDeleteWithCaps(query_task);
    query_task = nullptr;
    return;
  }
  initialized = true;
}
bool available() { return initialized.load() && !player_failed.load(); }
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
  portENTER_CRITICAL(&lock);
  for (bool p : playable)
    usable += p;
  portEXIT_CRITICAL(&lock);
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
#else
namespace cinema {
void before_display() {}
void init(const char *) {}
void tick() {}
bool available() { return false; }
esp_err_t cue(const char *, char *, size_t) { return ESP_ERR_NOT_SUPPORTED; }
bool command(const char *line) {
  if (strncmp(line, "cinema ", 7))
    return false;
  ESP_LOGI("Cinema",
           "Build with STACKCHAN_CINEMA_PACK to enable silent archival video");
  return true;
}
} // namespace cinema
#endif
