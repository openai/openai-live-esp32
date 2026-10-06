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
/* Application-owned configuration must outlive this process-lifetime
 * controller. The controller owns peer lifecycle, events, and core calls;
 * a dedicated sender submits audio concurrently with the peer receive loop.
 * Audio callbacks run on separate tasks and must return within one second,
 * including during shutdown. read/write exchange 24 kHz, signed 16-bit mono PCM. */
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
  uint32_t mic_peak;
  char status[128];
  char session_id[128];
  uint32_t transcript_revision;
  live_usage_t usage;
  bool startup_available;
  /* Milliseconds since Start was accepted; UINT32_MAX means not reached.
   * Retained after Stop, until the next session or audio diagnostic. */
  uint32_t startup_ms[LIVE_STARTUP_STAGE_COUNT];
} live_esp32_status_t;
/* Initialize once. start/stop/mute are nonblocking and safe on the GUI task. */
esp_err_t live_esp32_init(const live_esp32_config_t *config);
esp_err_t live_esp32_start(void);
/* Three-second local microphone/speaker diagnostic; creates no API session. */
esp_err_t live_esp32_audio_test(void);
void live_esp32_stop(void);
void live_esp32_mute(bool muted);
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
