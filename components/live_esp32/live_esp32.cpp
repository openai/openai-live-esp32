#include "live_esp32.h"
#include "transcript.hpp"
#include "usage.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "live.h"

namespace {
constexpr char TAG[] = "LiveVoice";
constexpr int RATE = 24000, SAMPLES = RATE / 50, PACKET_BYTES = 1500;
struct Packet {
  uint32_t pts;
  int size;
  uint8_t data[PACKET_BYTES];
};
live_esp32_config_t config{};
live_esp32_status_t status{};
live_transcript_t transcripts{};
live_usage::Tracker usage;
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> test_audio{false};
std::atomic<bool> initialized{false}, starting{false}, stop{false},
    muted{false};
std::atomic<bool> audio_running{false}, capture_done{true}, playback_done{true};
std::atomic<bool> connected{false}, failed{false};
TaskHandle_t controller = nullptr;
QueueHandle_t tx = nullptr, rx = nullptr;
esp_peer_handle_t peer = nullptr;
live_session_t *session = nullptr;
uint16_t stream_id = 0;
bool channel_open = false;
bool first_audio_received = false;
int64_t requested_at_us = 0, startup_origin_us = 0;
constexpr const char *STARTUP_NAMES[] = {
    "controller",      "peer_created",   "offer_ready",
    "https_connected", "request_sent",   "response_headers",
    "response_body",   "audio_ready",    "ice_paired",
    "dtls_connected",  "sctp_connected", "data_channel_open",
    "session_started", "first_audio_rx", "first_playback"};
static_assert(sizeof(STARTUP_NAMES) / sizeof(STARTUP_NAMES[0]) ==
              LIVE_STARTUP_STAGE_COUNT);
std::string offer;
void *encoder = nullptr, *decoder = nullptr;

void mark_startup(live_startup_stage_t stage) {
  const uint32_t elapsed = (esp_timer_get_time() - startup_origin_us) / 1000;
  portENTER_CRITICAL(&status_lock);
  const bool first =
      status.startup_available && status.startup_ms[stage] == UINT32_MAX;
  if (first)
    status.startup_ms[stage] = elapsed;
  portEXIT_CRITICAL(&status_lock);
  if (first)
    ESP_LOGI(TAG, "startup stage=%s elapsed_ms=%lu", STARTUP_NAMES[stage],
             static_cast<unsigned long>(elapsed));
}

void message(const char *text) {
  portENTER_CRITICAL(&status_lock);
  snprintf(status.status, sizeof(status.status), "%s", text);
  portEXIT_CRITICAL(&status_lock);
  ESP_LOGI(TAG, "%s", text);
}
void error(const char *text) {
  failed = true;
  message(text);
}

int send_event(void *, const char *json, size_t length) {
  if (!peer || !channel_open)
    return -1;
  esp_peer_data_frame_t frame{};
  frame.type = ESP_PEER_DATA_CHANNEL_STRING;
  frame.stream_id = stream_id;
  frame.data = reinterpret_cast<uint8_t *>(const_cast<char *>(json));
  frame.size = length;
  return esp_peer_send_data(peer, &frame);
}

void publish_usage(const char *type, const cJSON *root) {
  usage.receive(type, root);
  portENTER_CRITICAL(&status_lock);
  status.usage = usage.snapshot;
  portEXIT_CRITICAL(&status_lock);
}

void event(void *, const char *type, const char *json, size_t length) {
  if (!strcmp(type, "session.started")) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    publish_usage(type, root);
    cJSON_Delete(root);
    mark_startup(LIVE_STARTUP_SESSION_STARTED);
    portENTER_CRITICAL(&status_lock);
    status.active = true;
    portEXIT_CRITICAL(&status_lock);
    message("Listening - speak naturally");
  } else if (!strcmp(type, "session.closed") ||
             !strcmp(type, "session.usage.updated")) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    publish_usage(type, root);
    cJSON_Delete(root);
    if (!strcmp(type, "session.closed"))
      message("Session ended");
  } else if (!strcmp(type, "error")) {
    // Server error envelopes contain no API key; keep diagnostics bounded.
    ESP_LOGE(TAG, "Live error: %.*s",
             static_cast<int>(std::min(length, size_t(1500))), json);
    error("Live API error - see USB log");
  } else if (!strcmp(type, "response.event")) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    publish_usage(type, root);
    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "event");
    auto text = [](const cJSON *object, const char *name) {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
      return cJSON_IsString(value) ? value->valuestring : "";
    };
    const char *nested_type = text(nested, "type");
    if (!strstr(nested_type, ".delta")) {
      const cJSON *response =
          cJSON_GetObjectItemCaseSensitive(nested, "response");
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(nested, "item");
      ESP_LOGI(TAG, "Responses event=%s model=%s tier=%s item=%s", nested_type,
               text(response, "model"), text(response, "service_tier"),
               text(item, "type"));
      if (!strcmp(nested_type, "response.failed") ||
          !strcmp(nested_type, "error"))
        ESP_LOGE(TAG, "Responses error: %.*s",
                 static_cast<int>(std::min(length, size_t(1500))), json);
    }
    cJSON_Delete(root);
  } else if (!strcmp(type, "session.input_transcript.delta") ||
             !strcmp(type, "session.output_transcript.delta")) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta)) {
      auto time_ms = [root](const char *name) -> uint32_t {
        const auto *value = cJSON_GetObjectItemCaseSensitive(root, name);
        return cJSON_IsNumber(value) && value->valuedouble >= 0 &&
                       value->valuedouble <= UINT32_MAX
                   ? static_cast<uint32_t>(value->valuedouble)
                   : 0;
      };
      const auto speaker = !strcmp(type, "session.input_transcript.delta")
                               ? LIVE_SPEAKER_USER
                               : LIVE_SPEAKER_ASSISTANT;
      live_transcript_t captions;
      portENTER_CRITICAL(&status_lock);
      captions = transcripts;
      portEXIT_CRITICAL(&status_lock);
      live_captions::append(captions, speaker, delta->valuestring,
                            time_ms("start_ms"), time_ms("end_ms"));
      portENTER_CRITICAL(&status_lock);
      transcripts = captions;
      status.transcript_revision = captions.revision;
      portEXIT_CRITICAL(&status_lock);
      ESP_LOGI(TAG, "%s: %s", type, delta->valuestring);
    }
    cJSON_Delete(root);
  } else {
    ESP_LOGI(TAG, "event: %s", type);
  }
}

int on_state(esp_peer_state_t state, void *) {
  ESP_LOGI(TAG, "peer state %d", state);
  if (state == ESP_PEER_STATE_PAIRED)
    mark_startup(LIVE_STARTUP_ICE_PAIRED);
  if (state == ESP_PEER_STATE_CONNECTED) {
    mark_startup(LIVE_STARTUP_DTLS_CONNECTED);
    connected = true;
  }
  if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
    mark_startup(LIVE_STARTUP_SCTP_CONNECTED);
    // enable_data_channel reserves the SCTP m-line before the SDP offer.
    // Espressif opens the reliable DCEP channel after SCTP connects.
    esp_peer_data_channel_cfg_t channel{};
    channel.type = ESP_PEER_DATA_CHANNEL_RELIABLE;
    channel.ordered = true;
    channel.label = const_cast<char *>("oai-events");
    if (esp_peer_create_data_channel(peer, &channel))
      error("Could not open Live data channel");
  }
  if (state == ESP_PEER_STATE_CONNECT_FAILED ||
      state == ESP_PEER_STATE_DISCONNECTED) {
    connected = false;
    error("WebRTC disconnected");
  }
  return 0;
}
int on_message(esp_peer_msg_t *msg, void *) {
  if (msg->type == ESP_PEER_MSG_TYPE_SDP && offer.empty()) {
    if (msg->size <= 0 || msg->size > LIVE_MAX_SDP_BYTES) {
      error("SDP too large");
      return -1;
    }
    offer.assign(reinterpret_cast<char *>(msg->data),
                 strnlen(reinterpret_cast<char *>(msg->data), msg->size));
  }
  return 0;
}
int on_channel(esp_peer_data_channel_info_t *ch, void *) {
  stream_id = ch->stream_id;
  channel_open = true;
  mark_startup(LIVE_STARTUP_DATA_CHANNEL_OPEN);
  ESP_LOGI(TAG, "data channel open: %s (%u)", ch->label, stream_id);
  return 0;
}
int on_data(esp_peer_data_frame_t *frame, void *) {
  if (frame->size <= 0)
    return 0;
  live_result_t result = live_session_receive(
      session, reinterpret_cast<char *>(frame->data), frame->size);
  if (result != LIVE_OK) {
    ESP_LOGE(TAG, "Event rejected: %d, %d bytes", result, frame->size);
    error("Invalid Live event");
  }
  return 0;
}
int on_audio(esp_peer_audio_frame_t *frame, void *) {
  if (!audio_running || frame->size <= 0)
    return 0;
  if (frame->size > PACKET_BYTES) {
    error("Incoming Opus packet too large");
    return -1;
  }
  if (!first_audio_received) {
    first_audio_received = true;
    mark_startup(LIVE_STARTUP_FIRST_AUDIO_RX);
  }
  Packet p{};
  p.pts = frame->pts;
  p.size = frame->size;
  memcpy(p.data, frame->data, p.size);
  bool dropped = xQueueSend(rx, &p, 0) != pdTRUE;
  portENTER_CRITICAL(&status_lock);
  ++status.received_packets;
  if (dropped)
    ++status.dropped_packets;
  portEXIT_CRITICAL(&status_lock);
  return 0;
}
int on_audio_info(esp_peer_audio_stream_info_t *info, void *) {
  ESP_LOGI(TAG, "remote codec=%d clock=%lu channels=%u", info->codec,
           static_cast<unsigned long>(info->sample_rate), info->channel);
  if (info->codec != ESP_PEER_AUDIO_CODEC_OPUS)
    error("Remote did not select Opus");
  return 0;
}

void capture(void *) {
  int16_t pcm[SAMPLES];
  Packet packet{};
  uint32_t pts = 0;
  while (audio_running) {
    if (!connected) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (config.read(config.context, pcm, SAMPLES) != ESP_OK) {
      error("Microphone read failed");
      break;
    }
    int peak = 0;
    for (int16_t s : pcm)
      peak = std::max(peak, abs(static_cast<int>(s)));
    portENTER_CRITICAL(&status_lock);
    status.mic_peak = peak;
    portEXIT_CRITICAL(&status_lock);
    if (muted)
      memset(pcm, 0, sizeof(pcm));
    esp_audio_enc_in_frame_t in{};
    in.buffer = reinterpret_cast<uint8_t *>(pcm);
    in.len = sizeof(pcm);
    esp_audio_enc_out_frame_t out{};
    out.buffer = packet.data;
    out.len = sizeof(packet.data);
    if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK ||
        out.encoded_bytes <= 0) {
      error("Opus encode failed");
      break;
    }
    packet.size = out.encoded_bytes;
    packet.pts = pts;
    pts += 20;
    if (xQueueSend(tx, &packet, 0) != pdTRUE) {
      portENTER_CRITICAL(&status_lock);
      ++status.dropped_packets;
      portEXIT_CRITICAL(&status_lock);
    }
  }
  ESP_LOGI(TAG, "capture stack minimum free=%u",
           uxTaskGetStackHighWaterMark(nullptr));
  capture_done = true;
  vTaskDeleteWithCaps(nullptr);
}
void playback(void *) {
  // Opus permits up to 120 ms per packet. Decode directly at the board's 24
  // kHz.
  int16_t pcm[RATE * 120 / 1000];
  Packet packet{};
  bool first = true;
  while (audio_running) {
    if (xQueueReceive(rx, &packet, pdMS_TO_TICKS(50)) != pdTRUE)
      continue;
    esp_audio_dec_in_raw_t in{};
    in.buffer = packet.data;
    in.len = packet.size;
    in.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t out{};
    out.buffer = reinterpret_cast<uint8_t *>(pcm);
    out.len = sizeof(pcm);
    esp_audio_dec_info_t info{};
    if (esp_opus_dec_decode(decoder, &in, &out, &info) != ESP_AUDIO_ERR_OK ||
        out.decoded_size <= 0 || out.decoded_size > sizeof(pcm)) {
      error("Opus decode failed");
      break;
    }
    size_t samples = out.decoded_size / sizeof(int16_t);
    if (config.write(config.context, pcm, samples) != ESP_OK) {
      error("Speaker write failed");
      break;
    }
    if (first) {
      mark_startup(LIVE_STARTUP_FIRST_PLAYBACK);
      first = false;
    }
    portENTER_CRITICAL(&status_lock);
    status.played_samples += samples;
    portEXIT_CRITICAL(&status_lock);
  }
  ESP_LOGI(TAG, "playback stack minimum free=%u",
           uxTaskGetStackHighWaterMark(nullptr));
  playback_done = true;
  vTaskDeleteWithCaps(nullptr);
}

bool signal() {
  message("Creating Live session");
  std::vector<char> body(LIVE_MAX_JSON_BYTES + 1),
      answer(LIVE_MAX_JSON_BYTES + 1), sdp(LIVE_MAX_SDP_BYTES + 1);
  if (live_session_offer(session, offer.c_str(), body.data(), body.size()) !=
      LIVE_OK) {
    error("Could not serialize SDP");
    return false;
  }
  esp_http_client_config_t http{};
  http.url = "https://api.openai.com/v1/live/sessions";
  http.crt_bundle_attach = esp_crt_bundle_attach;
  http.timeout_ms = 30000;
  http.buffer_size = 4096;
  http.buffer_size_tx = 4096;
  http.event_handler = [](esp_http_client_event_t *event) -> esp_err_t {
    if (event->event_id == HTTP_EVENT_ON_CONNECTED)
      mark_startup(LIVE_STARTUP_HTTPS_CONNECTED);
    return ESP_OK;
  };
  // Never forward a project key to a redirect target.
  http.disable_auto_redirect = true;
  esp_http_client_handle_t client = esp_http_client_init(&http);
  if (!client) {
    error("HTTP allocation failed");
    return false;
  }
  std::string bearer = "Bearer ";
  bearer += config.api_key;
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Authorization", bearer.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  int body_length = strlen(body.data());
  esp_err_t result = esp_http_client_open(client, body_length);
  int sent = 0;
  while (result == ESP_OK && sent < body_length) {
    int n =
        esp_http_client_write(client, body.data() + sent, body_length - sent);
    if (n <= 0)
      result = ESP_FAIL;
    else
      sent += n;
  }
  if (result == ESP_OK)
    mark_startup(LIVE_STARTUP_REQUEST_SENT);
  int total = 0, code = 0;
  if (result == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
    mark_startup(LIVE_STARTUP_RESPONSE_HEADERS);
    code = esp_http_client_get_status_code(client);
    while (total < LIVE_MAX_JSON_BYTES) {
      int n = esp_http_client_read(client, answer.data() + total,
                                   LIVE_MAX_JSON_BYTES - total);
      if (n < 0) {
        result = ESP_FAIL;
        break;
      }
      if (!n)
        break;
      total += n;
    }
    if (!esp_http_client_is_complete_data_received(client))
      result = ESP_FAIL;
    if (result == ESP_OK)
      mark_startup(LIVE_STARTUP_RESPONSE_BODY);
  } else
    result = ESP_FAIL;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  std::fill(bearer.begin(), bearer.end(), '\0');
  ESP_LOGI(TAG, "Live HTTP %d, response bytes=%d", code, total);
  if (result != ESP_OK || code != 201) {
    ESP_LOGE(TAG, "Creation failed: %.*s", std::min(total, 1800),
             answer.data());
    char message_text[96];
    snprintf(message_text, sizeof(message_text),
             "Live request failed (HTTP %d)", code);
    error(message_text);
    return false;
  }
  if (live_session_answer(session, code, answer.data(), total, sdp.data(),
                          sdp.size()) != LIVE_OK) {
    error("Invalid Live session answer");
    return false;
  }
  portENTER_CRITICAL(&status_lock);
  snprintf(status.session_id, sizeof(status.session_id), "%s",
           live_session_id(session));
  portEXIT_CRITICAL(&status_lock);
  ESP_LOGI(TAG, "session_id=%s model=%s", live_session_id(session),
           config.model);
  esp_peer_msg_t msg{};
  msg.type = ESP_PEER_MSG_TYPE_SDP;
  msg.data = reinterpret_cast<uint8_t *>(sdp.data());
  msg.size = strlen(sdp.data());
  if (esp_peer_send_msg(peer, &msg)) {
    error("SDP answer rejected");
    return false;
  }
  message("Connecting WebRTC");
  return true;
}

bool open_codecs() {
  esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
  enc.sample_rate = RATE;
  enc.channel = ESP_AUDIO_MONO;
  enc.bits_per_sample = ESP_AUDIO_BIT16;
  enc.bitrate = 24000;
  enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  enc.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  enc.complexity = 1;
  enc.enable_fec = false;
  enc.enable_dtx = false;
  enc.enable_vbr = true;
  esp_opus_dec_cfg_t dec = ESP_OPUS_DEC_CONFIG_DEFAULT();
  dec.sample_rate = RATE;
  dec.channel = ESP_AUDIO_MONO;
  dec.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
  dec.self_delimited = false;
  int input_size = 0, output_size = 0;
  if (esp_opus_enc_open(&enc, sizeof(enc), &encoder) != ESP_AUDIO_ERR_OK ||
      esp_opus_enc_get_frame_size(encoder, &input_size, &output_size) !=
          ESP_AUDIO_ERR_OK ||
      input_size != SAMPLES * int(sizeof(int16_t)) ||
      output_size > PACKET_BYTES ||
      esp_opus_dec_open(&dec, sizeof(dec), &decoder) != ESP_AUDIO_ERR_OK) {
    error("Opus initialization failed");
    return false;
  }
  return true;
}
void close_codecs() {
  if (encoder) {
    esp_opus_enc_close(encoder);
    encoder = nullptr;
  }
  if (decoder) {
    esp_opus_dec_close(decoder);
    decoder = nullptr;
  }
}
bool open_audio() {
  if (config.audio_open(config.context) != ESP_OK) {
    error("Audio is busy or unavailable");
    return false;
  }
  if (!open_codecs())
    return false;
  audio_running = true;
  capture_done = false;
  if (xTaskCreatePinnedToCoreWithCaps(
          capture, "live_mic", 48 * 1024, nullptr, 7, nullptr, 0,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    capture_done = true;
    error("Microphone task allocation failed");
    return false;
  }
  playback_done = false;
  if (xTaskCreatePinnedToCoreWithCaps(
          playback, "live_speaker", 32 * 1024, nullptr, 7, nullptr, 0,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    playback_done = true;
    error("Speaker task allocation failed");
    return false;
  }
  return true;
}

void run_session() {
  failed = false;
  muted = false;
  connected = false;
  channel_open = false;
  first_audio_received = false;
  startup_origin_us = requested_at_us;
  stream_id = 0;
  offer.clear();
  usage.reset(config.model, config.responses_model);
  portENTER_CRITICAL(&status_lock);
  status = {};
  status.usage = usage.snapshot;
  live_captions::clear(transcripts);
  status.transcript_revision = transcripts.revision;
  status.busy = true;
  status.startup_available = true;
  std::fill_n(status.startup_ms, LIVE_STARTUP_STAGE_COUNT, UINT32_MAX);
  portEXIT_CRITICAL(&status_lock);
  mark_startup(LIVE_STARTUP_CONTROL);
  xQueueReset(tx);
  xQueueReset(rx);
  message("Preparing voice");
  bool audio_claimed = false;
  live_config_t cfg{};
  cfg.model = config.model;
  cfg.responses_model = config.responses_model;
  cfg.responses_service_tier = config.responses_service_tier;
  cfg.responses_instructions = config.responses_instructions;
  cfg.responses_web_search = config.responses_web_search;
  cfg.voice = "marin";
  cfg.instructions =
      config.responses_web_search
          ? "You are a friendly voice assistant on a small handheld device. "
            "Speak "
            "in English. Keep answers brief and conversational. Delegate "
            "requests "
            "for current information and internet lookups to your backend, "
            "which "
            "can search the web. Wait for its findings before stating results."
          : "You are a friendly voice assistant on a small handheld device. "
            "Speak "
            "in English. Keep answers brief and conversational. Delegate "
            "questions "
            "when needed.";
  cfg.send = send_event;
  cfg.on_event = event;
  if (live_session_create(&cfg, &session) != LIVE_OK) {
    error("Session allocation failed");
    goto cleanup;
  }
  {
    esp_peer_default_cfg_t defaults{};
    defaults.agent_recv_timeout = 10;
    defaults.ice_use_lite_mode = true;
    defaults.data_ch_cfg.recv_cache_size = 32768;
    defaults.data_ch_cfg.send_cache_size = 32768;
    defaults.rtp_cfg.audio_recv_jitter.cache_size = 32768;
    defaults.rtp_cfg.send_pool_size = 32768;
    defaults.rtp_cfg.send_queue_num = 64;
    esp_peer_cfg_t pc{};
    pc.role = ESP_PEER_ROLE_CONTROLLING;
    pc.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
    pc.audio_info.sample_rate = RATE;
    pc.audio_info.channel = 1;
    pc.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    pc.enable_data_channel = true;
    pc.manual_ch_create = true;
    pc.no_auto_reconnect = true;
    pc.extra_cfg = &defaults;
    pc.extra_size = sizeof(defaults);
    pc.on_state = on_state;
    pc.on_msg = on_message;
    pc.on_channel_open = on_channel;
    pc.on_data = on_data;
    pc.on_audio_data = on_audio;
    pc.on_audio_info = on_audio_info;
    if (esp_peer_open(&pc, esp_peer_get_default_impl(), &peer)) {
      error("Peer allocation failed");
      goto cleanup;
    }
    mark_startup(LIVE_STARTUP_PEER_CREATED);
    if (esp_peer_new_connection(peer)) {
      error("Could not create WebRTC offer");
      goto cleanup;
    }
    int64_t deadline = esp_timer_get_time() + 15000000;
    while (offer.empty() && !failed && !stop &&
           esp_timer_get_time() < deadline) {
      esp_peer_main_loop(peer);
      vTaskDelay(1);
    }
    if (offer.empty()) {
      error("ICE gathering timed out");
      goto cleanup;
    }
    mark_startup(LIVE_STARTUP_OFFER_READY);
    if (stop || !signal())
      goto cleanup;
    // audio_close tolerates a failed acquisition; the board adapter tracks
    // ownership.
    audio_claimed = true;
    if (!open_audio())
      goto cleanup;
    mark_startup(LIVE_STARTUP_AUDIO_READY);
    bool applied_mute = false;
    bool closing = false;
    int64_t close_deadline = 0;
    deadline = esp_timer_get_time() + 30000000;
    int64_t report_at = 0;
    Packet p{};
    while (!failed) {
      esp_peer_main_loop(peer);
      auto state = live_session_state(session);
      if (state == LIVE_CLOSED)
        break;
      if (state != LIVE_ACTIVE && !closing && esp_timer_get_time() > deadline) {
        error("Session startup timed out");
        break;
      }
      if (stop && !closing) {
        if (state == LIVE_ACTIVE && live_session_close(session) == LIVE_OK) {
          closing = true;
          // Live may drain work for 10 seconds, then reconcile usage for 5.
          // Keep receiving the final event with a little transport headroom.
          close_deadline = esp_timer_get_time() + 20000000;
          message("Ending session");
        } else
          break;
      }
      if (closing && esp_timer_get_time() > close_deadline) {
        message("Closed locally - final event timed out");
        break;
      }
      if (!closing && state == LIVE_ACTIVE && muted.load() != applied_mute) {
        if (live_session_mute(session, muted.load()) == LIVE_OK) {
          applied_mute = muted;
          portENTER_CRITICAL(&status_lock);
          status.muted = applied_mute;
          portEXIT_CRITICAL(&status_lock);
          message(applied_mute ? "Microphone muted"
                               : "Listening - speak naturally");
        }
      }
      for (int i = 0; i < 8 && connected && xQueueReceive(tx, &p, 0) == pdTRUE;
           ++i) {
        esp_peer_audio_frame_t frame{};
        frame.pts = p.pts;
        frame.data = p.data;
        frame.size = p.size;
        int r = esp_peer_send_audio(peer, &frame);
        portENTER_CRITICAL(&status_lock);
        if (!r)
          ++status.sent_packets;
        else
          ++status.dropped_packets;
        portEXIT_CRITICAL(&status_lock);
      }
      if (esp_timer_get_time() > report_at) {
        live_esp32_status_t s{};
        live_esp32_status(&s);
        ESP_LOGI(
            TAG,
            "audio tx=%lu rx=%lu played=%lu dropped=%lu mic_peak=%lu heap=%lu "
            "internal=%lu",
            (unsigned long)s.sent_packets, (unsigned long)s.received_packets,
            (unsigned long)s.played_samples, (unsigned long)s.dropped_packets,
            (unsigned long)s.mic_peak, (unsigned long)esp_get_free_heap_size(),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        report_at = esp_timer_get_time() + 5000000;
      }
      vTaskDelay(1);
    }
  }
cleanup:
  audio_running = false;
  // Never destroy codec or buffers while an audio task can still reference
  // them.
  while (!capture_done || !playback_done)
    vTaskDelay(pdMS_TO_TICKS(10));
  close_codecs();
  if (audio_claimed)
    config.audio_close(config.context);
  if (peer) {
    esp_peer_close(peer);
    peer = nullptr;
  }
  if (session) {
    bool finalized = live_session_finalized(session);
    ESP_LOGI(TAG, "session finished id=%s finalized=%d",
             live_session_id(session), finalized);
    portENTER_CRITICAL(&status_lock);
    status.finalized = finalized;
    portEXIT_CRITICAL(&status_lock);
    live_session_transport_closed(session);
    live_session_destroy(session);
    session = nullptr;
  }
  portENTER_CRITICAL(&status_lock);
  status.active = false;
  status.busy = false;
  portEXIT_CRITICAL(&status_lock);
  starting = false;
  const auto &u = usage.snapshot;
  ESP_LOGI(TAG,
           "usage source=%s seconds_available=%d seconds=%.1f "
           "tokens_available=%d partial=%d input=%llu output=%llu total=%llu "
           "cached=%llu responses=%lu",
           u.seconds_final ? "final" : "last_reported", u.seconds_available,
           u.seconds, u.tokens_available, u.tokens_partial,
           (unsigned long long)u.input_tokens, (unsigned long long)u.output_tokens,
           (unsigned long long)u.total_tokens, (unsigned long long)u.cached_tokens,
           (unsigned long)u.responses);
}
void audio_diagnostic() {
  portENTER_CRITICAL(&status_lock);
  status = {};
  live_captions::clear(transcripts);
  status.transcript_revision = transcripts.revision;
  status.busy = true;
  portEXIT_CRITICAL(&status_lock);
  message("Audio check - playing test tone");
  if (config.audio_open(config.context) == ESP_OK && open_codecs()) {
    int16_t input[SAMPLES], output[SAMPLES], decoded[RATE * 120 / 1000];
    uint8_t encoded[PACKET_BYTES];
    double quiet_energy = 0, tone_energy = 0;
    int quiet_frames = 0, tone_frames = 0;
    bool ok = true;
    for (int frame = 0; frame < 150 && !stop; ++frame) {
      bool tone = frame >= 25 && frame < 75;
      for (int i = 0; i < SAMPLES; ++i)
        output[i] = tone ? int16_t(3000 * sinf(2 * 3.14159265f * i / 48)) : 0;
      esp_audio_enc_in_frame_t raw{};
      raw.buffer = reinterpret_cast<uint8_t *>(output);
      raw.len = sizeof(output);
      esp_audio_enc_out_frame_t packet{};
      packet.buffer = encoded;
      packet.len = sizeof(encoded);
      esp_audio_dec_in_raw_t packet_in{};
      packet_in.buffer = encoded;
      esp_audio_dec_out_frame_t pcm{};
      pcm.buffer = reinterpret_cast<uint8_t *>(decoded);
      pcm.len = sizeof(decoded);
      esp_audio_dec_info_t info{};
      if (esp_opus_enc_process(encoder, &raw, &packet) != ESP_AUDIO_ERR_OK) {
        ok = false;
        message("Local Opus encode failed");
        break;
      }
      packet_in.len = packet.encoded_bytes;
      if (esp_opus_dec_decode(decoder, &packet_in, &pcm, &info) !=
              ESP_AUDIO_ERR_OK ||
          pcm.decoded_size != sizeof(output)) {
        ok = false;
        message("Local Opus decode failed");
        break;
      }
      if (config.write(config.context, decoded, SAMPLES) != ESP_OK ||
          config.read(config.context, input, SAMPLES) != ESP_OK) {
        ok = false;
        message("Local audio I/O failed");
        break;
      }
      double real = 0, imaginary = 0;
      int peak = 0;
      for (int i = 0; i < SAMPLES; ++i) {
        real += input[i] * cosf(2 * 3.14159265f * i / 48);
        imaginary += input[i] * sinf(2 * 3.14159265f * i / 48);
        peak = std::max(peak, abs(int(input[i])));
      }
      double energy =
          2 * (real * real + imaginary * imaginary) / (SAMPLES * SAMPLES);
      if (frame < 20) {
        quiet_energy += energy;
        ++quiet_frames;
      }
      if (frame >= 35 && frame < 70) {
        tone_energy += energy;
        ++tone_frames;
      }
      portENTER_CRITICAL(&status_lock);
      status.mic_peak = peak;
      status.played_samples += SAMPLES;
      portEXIT_CRITICAL(&status_lock);
    }
    if (ok) {
      char result[128];
      snprintf(result, sizeof(result),
               "Opus + audio passed: 500Hz mic RMS quiet=%.1f tone=%.1f",
               sqrt(quiet_energy / std::max(1, quiet_frames)),
               sqrt(tone_energy / std::max(1, tone_frames)));
      message(result);
    }
  } else
    message("Audio is busy or unavailable");
  ESP_LOGI(TAG, "diagnostic stack minimum free=%u",
           uxTaskGetStackHighWaterMark(nullptr));
  close_codecs();
  config.audio_close(config.context);
  portENTER_CRITICAL(&status_lock);
  status.busy = false;
  portEXIT_CRITICAL(&status_lock);
  starting = false;
}
void control(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (test_audio)
      audio_diagnostic();
    else
      run_session();
  }
}
} // namespace

esp_err_t live_esp32_init(const live_esp32_config_t *cfg) {
  if (!cfg || !cfg->api_key || !*cfg->api_key || !cfg->model ||
      !cfg->audio_open || !cfg->audio_close || !cfg->read || !cfg->write)
    return ESP_ERR_INVALID_ARG;
  if (initialized)
    return ESP_ERR_INVALID_STATE;
  config = *cfg;
  tx = xQueueCreateWithCaps(8, sizeof(Packet),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rx = xQueueCreateWithCaps(16, sizeof(Packet),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tx || !rx) {
    if (tx)
      vQueueDeleteWithCaps(tx);
    if (rx)
      vQueueDeleteWithCaps(rx);
    tx = rx = nullptr;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreatePinnedToCoreWithCaps(
          control, "live_control", 64 * 1024, nullptr, 6, &controller, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    vQueueDeleteWithCaps(tx);
    vQueueDeleteWithCaps(rx);
    tx = rx = nullptr;
    return ESP_ERR_NO_MEM;
  }
  initialized = true;
  message("Ready - tap Start");
  return ESP_OK;
}
esp_err_t live_esp32_start(void) {
  bool expected = false;
  if (!initialized || !starting.compare_exchange_strong(expected, true))
    return ESP_ERR_INVALID_STATE;
  stop = false;
  test_audio = false;
  requested_at_us = esp_timer_get_time();
  xTaskNotifyGive(controller);
  return ESP_OK;
}
esp_err_t live_esp32_audio_test(void) {
  bool expected = false;
  if (!initialized || !starting.compare_exchange_strong(expected, true))
    return ESP_ERR_INVALID_STATE;
  stop = false;
  test_audio = true;
  xTaskNotifyGive(controller);
  return ESP_OK;
}
void live_esp32_stop(void) { stop = true; }
void live_esp32_mute(bool value) { muted = value; }
void live_esp32_status(live_esp32_status_t *out) {
  if (!out)
    return;
  portENTER_CRITICAL(&status_lock);
  *out = status;
  portEXIT_CRITICAL(&status_lock);
  // Expose a queued start immediately, before the controller task runs.
  out->busy = out->busy || starting.load();
}
void live_esp32_log_startup(void) {
  live_esp32_status_t snapshot{};
  live_esp32_status(&snapshot);
  if (!snapshot.startup_available) {
    ESP_LOGI(TAG, "No session startup timing recorded since boot");
    return;
  }
  for (unsigned i = 0; i < LIVE_STARTUP_STAGE_COUNT; ++i)
    if (snapshot.startup_ms[i] != UINT32_MAX)
      ESP_LOGI(TAG, "startup stage=%s elapsed_ms=%lu", STARTUP_NAMES[i],
               static_cast<unsigned long>(snapshot.startup_ms[i]));
}
void live_esp32_transcripts(live_transcript_t *out) {
  if (!out)
    return;
  portENTER_CRITICAL(&status_lock);
  *out = transcripts;
  portEXIT_CRITICAL(&status_lock);
}
