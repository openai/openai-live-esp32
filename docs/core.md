# Core integration

The SDK separates protocol handling from the board and transport.

## Portable C library

Add `components/live` as an ESP-IDF component, or compile `live.c` with cJSON in
your own build. [`live.h`](../components/live/include/live.h) is usable from C
and C++. It does not initialize networking, capture audio, or retain an API key.

The application's transport performs this sequence:

1. Create a `live_session_t` with model settings and `send` / `on_event` callbacks.
2. Gather an SDP offer that includes audio and a reliable data channel.
3. Call `live_session_offer` to produce the session-creation JSON body.
4. POST it over authenticated HTTPS to `/v1/live/sessions`.
5. Pass the HTTP status/body to `live_session_answer` and apply the returned SDP
   answer to your peer connection.
6. Feed each complete data-channel JSON message to `live_session_receive`.
7. Wait for `session.started` before sending mute or close commands.
8. On shutdown, send `live_session_close` and continue processing events until
   `session.closed` or a bounded timeout. Then tear down the transport.

Do not send `session.start` on the WebRTC data channel. The HTTP creation call
already initializes the session. The public
[WebRTC guide](https://developers.openai.com/api/docs/guides/voice-webrtc?api=live)
describes the API exchange.

`send` must copy or consume its payload before returning; zero means accepted
locally, not acknowledged by the server. Configuration strings are copied.
Callback payloads are borrowed only during the callback. Copy data before
queueing it, and serialize all core calls on one task. Callbacks may use getters
but must queue mutations until they return.

Unknown events and nested Responses events are forwarded unchanged. The core
does not interpret backend text as spoken transcript text. Use the input/output
transcript events for captions.

## ESP-IDF adapter

Add `components/live`, `components/live_esp32`, and the pinned Espressif
`esp_peer` component to your ESP-IDF project. The adapter's component manifest
resolves its audio codec dependency. The Waveshare preparation script is a
complete example of wiring these components into an application.

Initialize [`live_esp32.h`](../components/live_esp32/include/live_esp32.h) once
with an application-owned configuration that remains alive for the process:

```c
live_esp32_config_t config = {
    .api_key = device_api_key,
    .model = "gpt-live-1",
    .responses_model = "gpt-6-luna",
    .audio_open = board_audio_open,
    .audio_close = board_audio_close,
    .read = board_read_microphone,
    .write = board_write_speaker,
    .responses_service_tier = "priority",
    .responses_web_search = true,
};
// Keep config strings and context alive; callbacks are application supplied.
ESP_ERROR_CHECK(live_esp32_init(&config));
// Connect Wi-Fi and obtain an IP address before starting.
ESP_ERROR_CHECK(live_esp32_start());
```

Audio callbacks exchange 24 kHz signed 16-bit mono PCM and must return within
one second, including during shutdown. Capture and playback run on separate
tasks. The controller owns peer/core calls. The adapter is a process-lifetime
singleton; `start`, `stop`, and `mute` are nonblocking GUI-safe operations.

For faster first-tap startup, opt into background TLS preconnection with
`live_esp32_preconnect(true)` while the voice interface is open. Notify the SDK
of each acquired/changed IP with `live_esp32_network_changed(true)` and each
disconnect/lost IP with `live_esp32_network_changed(false)`. These calls are
nonblocking and can precede initialization. Call `live_esp32_preconnect(false)`
when leaving the interface. The component enables ESP-IDF's custom HTTP
transport support through Kconfig.

Preconnection opens one verified TLS socket, without sending HTTP, credentials,
or microphone audio. Start consumes it or completes the pending handshake;
if unavailable, Start connects normally. A cached socket is invalidated on
network changes, peer closure, or after 45 seconds. Failed warmups back off for
five seconds. Cleanup runs on the controller task; DNS/TCP setup can take up to
the underlying network timeout before a queued cancellation is processed.
HTTP/TLS resources are released before audio starts. No session-creation POST
is retried after its body begins sending.

`preconnect_ready` and `preconnect_pending` describe idle preparation;
`https_preconnected` records whether the latest session used it. An immediately
tapped Start may still wait for TLS. All startup checkpoints include that wait.

The adapter includes TLS certificate verification and disables redirects while
using the API key. The example enables Responses delegation and web search;
see the [public delegation guide](https://developers.openai.com/api/docs/guides/live-delegation).

## Status, captions, and usage

`live_esp32_status` returns a synchronized status snapshot. Startup checkpoints
record elapsed milliseconds from Start through HTTPS, WebRTC setup, and first
audio. `live_esp32_transcripts` returns eight bounded transcript blocks with
separate user/assistant roles. Same-speaker fragments within 1.5 seconds extend
the same block, preserving UTF-8 boundaries. This grouping is a UI heuristic,
not a server-defined conversational turn.

Usage is retained until the next session or audio diagnostic; reboot clears it.
Cumulative `session.usage.updated.usage.seconds` replaces the prior snapshot.
A valid `session.closed.usage.seconds` is authoritative. Without it, retain the
last report and label it incomplete. Never sum duration snapshots or treat
context-window pressure as token billing.

Backend counters sum valid usage from distinct terminal Responses events,
deduplicated by response ID. They retain input, output, total, and cached-input
counts. Tracking is bounded to 128 response IDs; exceeding that limit or missing
terminal usage marks totals as partial. No reported usage means unavailable,
not zero. Model names start from configuration and update from server events.

## Resource and lifecycle limits

| Item | SDK limit |
| --- | --- |
| Incoming JSON | 16 KiB, 32 nesting levels |
| SDP | 8 KiB |
| Session ID and event type | 127 bytes |
| Instructions | 4,096 bytes |
| Transcript history | Eight blocks of 255 text bytes |
| Deduplicated backend responses | 128 per session |

These are SDK limits, not API limits. Larger valid API events can exceed them.
Raw or decoded NUL bytes and ambiguous lifecycle fields are rejected. JSON
parsing allocates on the control path; the audio path uses bounded buffers.

`live_session_finalized` is true only after `session.closed`. A transport close
without that event is not successful finalization. The adapter allows 20 seconds
for graceful close before local teardown. Destroy closed core handles and create
new ones for subsequent sessions.
