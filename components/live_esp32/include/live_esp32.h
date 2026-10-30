#ifndef LIVE_ESP32_H
#define LIVE_ESP32_H
#include "esp_err.h"
#include "live_transcript.h"
#include "live_usage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  LIVE_ESP32_PTT_DISABLED = 0,
  LIVE_ESP32_PTT_ENABLED,
  /* Keep the ordinary Start behavior until talk(true) is first called. Once
   * detected, PTT remains enabled until reboot, including after unplugging. */
  LIVE_ESP32_PTT_ON_FIRST_PRESS,
} live_esp32_ptt_mode_t;
/* Application-owned configuration must outlive this process-lifetime
 * controller. The controller owns peer lifecycle, events, and core calls;
 * the controller also sends queued microphone packets on that same thread.
 * Audio callbacks run on separate tasks and must return within one second,
 * including during shutdown. read/write exchange 24 kHz, signed 16-bit mono
 * PCM. */
typedef struct {
  const char *api_key;
  const char *model;
  const char *responses_model;
  void *context;
  esp_err_t (*audio_open)(void *context);
  void (*audio_close)(void *context);
  esp_err_t (*read)(void *context, int16_t *pcm, size_t samples);
  esp_err_t (*write)(void *context, const int16_t *pcm, size_t samples);
  const char *responses_service_tier;
  const char *responses_instructions;
  bool responses_web_search;
  /* Optional, nonblocking monotonic hardware-output starvation counter. The
   * adapter must keep it valid until audio_close; uint32_t wrap is supported.
   */
  uint32_t (*output_starvations)(void *context);
  /* Opt-in hold-to-talk with up to 10 seconds of startup audio in PSRAM.
   * Default DISABLED preserves continuous microphone behavior on other boards. */
  live_esp32_ptt_mode_t ptt_mode;
  /* Required for PTT. Discard queued/in-flight microphone samples on each
   * press so a hardware DMA backlog cannot expose audio from before the press.
   * Called only by the capture task, while audio is open; leave output intact. */
  esp_err_t (*input_flush)(void *context);
} live_esp32_config_t;
typedef enum {
  LIVE_STARTUP_CONTROL,
  LIVE_STARTUP_PEER_CREATED,
  LIVE_STARTUP_OFFER_READY,
  LIVE_STARTUP_HTTPS_CONNECTED,
  LIVE_STARTUP_REQUEST_SENT,
  LIVE_STARTUP_RESPONSE_HEADERS,
  LIVE_STARTUP_RESPONSE_BODY,
  LIVE_STARTUP_AUDIO_READY,
  LIVE_STARTUP_ICE_PAIRED,
  LIVE_STARTUP_DTLS_CONNECTED,
  LIVE_STARTUP_SCTP_CONNECTED,
  LIVE_STARTUP_DATA_CHANNEL_OPEN,
  LIVE_STARTUP_SESSION_STARTED,
  LIVE_STARTUP_FIRST_AUDIO_RX,
  LIVE_STARTUP_FIRST_PLAYBACK,
  LIVE_STARTUP_STAGE_COUNT
} live_startup_stage_t;
typedef struct {
  bool busy, active, muted, finalized;
  bool preconnect_ready, preconnect_pending, https_preconnected;
  uint32_t sent_packets, received_packets, played_samples, dropped_packets;
  uint32_t tx_queue_drops, rx_queue_drops, send_errors;
  uint32_t peer_poll_max_us, peer_poll_over_20ms;
  uint32_t control_cycle_max_us, control_cycle_over_20ms;
  uint32_t peer_send_max_us, peer_send_over_20ms;
  uint32_t encode_max_us, decode_max_us, playback_wait_timeouts;
  uint32_t rx_queue_age_max_us, speaker_write_max_us;
  uint32_t speaker_dma_empty_events, playout_rebuffers, playout_target_ms;
  uint32_t decoded_peak, decoded_near_full_scale_samples;
  uint32_t mic_peak;
  char status[128];
  char session_id[128];
  uint32_t transcript_revision;
  live_usage_t usage;
  bool startup_available;
  /* Milliseconds since Start was accepted; UINT32_MAX means not reached.
   * Retained after Stop, until the next session or audio diagnostic. */
  uint32_t startup_ms[LIVE_STARTUP_STAGE_COUNT];
  /* True only while attempting/running a network session. Local diagnostics
   * also report busy, but must remain usable without Wi-Fi. */
  bool network_required;
  bool push_to_talk, talk_held;
  uint32_t talk_buffered_ms, talk_captured_samples, talk_buffer_overflows;
} live_esp32_status_t;
/* Initialize once. start/stop/mute are nonblocking and safe on the GUI task. */
esp_err_t live_esp32_init(const live_esp32_config_t *config);
esp_err_t live_esp32_start(void);
/* Three-second local microphone/speaker diagnostic; creates no API session. */
esp_err_t live_esp32_audio_test(void);
/* Local Opus/playback test with deterministic receive jitter; no API session.
 */
esp_err_t live_esp32_jitter_test(void);
/* Apply to the next session/diagnostic. Enabled by default (120 ms maximum
 * priming wait). Disabling is useful for controlled latency/underrun tests. */
void live_esp32_playout_buffer(bool enabled);
/* Five local peer init/offer/destroy/shutdown cycles; no signaling or audio. */
esp_err_t live_esp32_peer_test(void);
void live_esp32_stop(void);
void live_esp32_mute(bool muted);
/* For enabled/on-first-press PTT configurations. Press starts an idle session; release
 * gates new microphone audio immediately but keeps the session connected and
 * finishes sending already captured speech. Call release on lost input too.
 * A full startup buffer fails the session rather than silently losing speech.
 * Only held frames are retained; unheld frames are replaced with silence. */
esp_err_t live_esp32_talk(bool held);
/* Opt-in idle TLS warmup. Enable while the voice UI is open; disable when it
 * closes. Creates no API session and sends no HTTP request or credentials.
 * Call network_changed on each IP acquisition/change and loss, even if a rapid
 * reconnect returns to the same IP. Both calls are nonblocking and may precede
 * init. Start remains usable while warmup is pending or unavailable. */
void live_esp32_preconnect(bool enabled);
void live_esp32_network_changed(bool available);
void live_esp32_status(live_esp32_status_t *status);
void live_esp32_transcripts(live_transcript_t *transcripts);
void live_esp32_log_startup(void);
/* USB diagnostics only: task sampling briefly suspends the scheduler. */
void live_esp32_log_diagnostics(void);
#ifdef __cplusplus
}
#endif
#endif
