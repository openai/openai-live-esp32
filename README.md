# OpenAI Live SDK for ESP32

> **Experimental project. Not officially supported by OpenAI.** This is a proof
> of concept, with no stability guarantees or support commitment.

Build a voice assistant with the [OpenAI Live API](https://developers.openai.com/api/docs/guides/live)
on an ESP32-S3. The project has two parts:

1. **Core SDK:** a portable C protocol library and an ESP-IDF WebRTC/Opus adapter.
2. **Device examples:** a native Live Voice app for the **Waveshare
   ESP32-S3-Touch-AMOLED-1.8 V2**, integrated into its Brookesia launcher, a
   standalone app for **M5Stack CoreS3 LITE**, and a screenless **Sonatino**
   headset example.

A separate [experimental portable WebRTC package](packages/peer) provides a
libpeer-derived C transport, host interoperability tests, and a security review.
It is independently buildable and supplies all three examples' WebRTC transport.

## Core SDK

The core SDK handles Live sessions, WebRTC transport, and audio streaming. Your
application supplies networking and microphone/speaker drivers; a display is
optional. The device apps below show how to connect these pieces to real hardware.

| Component | Purpose |
| --- | --- |
| [`components/live`](components/live) | Portable C API for session creation, JSON offer/answer, events, mute, and graceful close. Depends on cJSON; has no board or credential storage. |
| [`components/live_esp32`](components/live_esp32) | ESP-IDF adapter: HTTPS signaling, portable peer connection, Opus audio, transcript buffers, usage accounting, and startup timing. |
| [`components/live_peer`](components/live_peer) | ESP-IDF build and platform integration for the source-built WebRTC transport in `packages/peer`. |
| [`examples/protocol_smoke`](examples/protocol_smoke) | Hardware-independent ESP32-S3 protocol test firmware for QEMU and esp-emu. |

Start with the [core integration guide](docs/core.md) and
[`live_esp32.h`](components/live_esp32/include/live_esp32.h). Initialize the
adapter with your configuration and audio callbacks, then call
`live_esp32_start()` after Wi-Fi has an IP address. Audio callbacks exchange
24 kHz, signed 16-bit mono PCM. The portable C library in
[`live.h`](components/live/include/live.h) can also be used with another
transport implementation.

The SDK uses the public Live WebRTC JSON offer/answer protocol at
`POST https://api.openai.com/v1/live/sessions`. RTP carries audio; a reliable data
channel carries JSON events. This is the Live API, whose signaling and event
contract differs from the Realtime API.

### Customize the system prompt and Live configuration

The example apps initialize `live_esp32_config_t` in
[`LiveVoice.cpp`](examples/waveshare/LiveVoice.cpp) (Waveshare),
[`main.cpp`](examples/m5stack/main/main.cpp) (M5Stack), or
[`main.cpp`](examples/sonatino/main/main.cpp) (Sonatino). Set these fields before
calling `live_esp32_init()`:

| Setting | Configuration | Example configuration |
| --- | --- | --- |
| Frontend voice model | `model` | `LIVE_MODEL`, generated from the environment; defaults to `gpt-live-1` |
| Delegated Responses model | `responses_model` | `LIVE_RESPONSES_MODEL`, generated from the environment; defaults to `gpt-6-luna` |
| Backend instructions | `responses_instructions` | Instructions for the delegated backend, such as when to search and how to summarize findings |
| Backend service tier | `responses_service_tier` | `"priority"` in the device examples |
| Hosted web search | `responses_web_search` | `true` in the device examples; set `false` to omit the web-search tool |
| API key | `api_key` | `LIVE_API_KEY` from the private preparation step below |

The **frontend system prompt** and voice are currently set inside the shared
adapter's `run_session()` in
[`live_esp32.cpp`](components/live_esp32/live_esp32.cpp). Replace its
`cfg.instructions` and `cfg.voice` assignments before `live_session_create()`
to customize them, for example:

```cpp
cfg.instructions =
    "You are a friendly voice assistant. Keep answers brief and conversational. "
    "Delegate current-information questions to your backend and wait for its findings.";
cfg.voice = "marin";
```

These two settings are not yet fields on `live_esp32_config_t`; this shared
adapter edit applies to all device examples. When using the portable C library
directly, set `live_config_t.instructions` and `live_config_t.voice` in your own
session configuration. `responses_instructions` controls the delegated backend
and does not replace the frontend system prompt. Rebuild and flash after changing
the configuration; the SDK does not yet expose live session-update APIs.

### Set the API key and model names

For the device examples, save your API key as a single line in a private file
outside this checkout, such as `$HOME/.config/openai-live-esp32/api-key`, with
file permissions `600`. Pass it to your board's preparation script with
`--key-file`. For example, the Waveshare preparation command is:

```sh
LIVE_MODEL=gpt-live-1 LIVE_RESPONSES_MODEL=gpt-6-luna \
python tools/prepare_voice.py \
  --key-file "$HOME/.config/openai-live-esp32/api-key"
```

Use [`prepare_m5stack.py`](tools/prepare_m5stack.py) or
[`prepare_sonatino.py`](tools/prepare_sonatino.py) for those boards; both also
require `--wifi-file`, as shown in their device guides. All three scripts accept
`OPENAI_API_KEY` from the environment instead of `--key-file`. Change the model
environment variables above to use other models available to your account,
then rerun preparation, build, and flash using the chosen device guide.

Preparation generates `live_voice_secrets.h` in a private external cache. The
examples pass its `LIVE_API_KEY` to the HTTPS adapter. In your own application,
supply `live_esp32_config_t.api_key` through your provisioning mechanism and keep
the string alive for the controller's lifetime. The portable C library leaves
authentication to your transport.

The example firmware embeds the key and Wi-Fi credentials. **Do not publish
credential files, generated headers, build directories, firmware, or flash
backups.** No credentials or prebuilt firmware are included in this repository.
See [credential handling](SECURITY.md) for deployment considerations.

## Choose a device

This library is intended for ESP32 voice applications beyond the boards listed
here. These are **illustrative, working example apps**: use one on its named
device to get started, or adapt its board drivers and controls for your own
project. The same core SDK supports all three integrations.

The current ESP-IDF adapter targets dual-core **ESP32-S3 boards with PSRAM**.
Other boards need compatible networking and audio drivers, sufficient memory,
and validation of task scheduling and audio performance. A display is optional;
the Sonatino example shows a screenless integration.

All three examples use the same protocol, WebRTC transport, Opus audio, and
120 ms playback reserve. Each has been exercised in a real Live conversation.

| Device | Audio and display | Session controls | Setup |
| --- | --- | --- | --- |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 | Built-in microphone/speaker; 368 × 448 AMOLED | Touch Start/Stop; USB commands and screen mirror | [Waveshare guide](examples/waveshare/README.md) |
| M5Stack CoreS3 LITE | Built-in microphones/speaker; 320 × 240 LCD | Touch Start/Stop; USB commands | [M5Stack guide](examples/m5stack/README.md) |
| Sonatino 3.1.x | External CTIA headset with mic; no display | BOOT release starts while idle; RESET reboots; USB `live stop` closes gracefully | [Sonatino guide](examples/sonatino/README.md) |

Sonatino requires its external antenna. Its BOOT button shares the audio clock
pin: **do not press BOOT during a call or audio test**. Firmware and pin mappings
are specific to each named board; do not flash one example onto another device.

## Waveshare screenshots

These are unmodified framebuffer captures from the app running on a physical
Waveshare device, not mockups. The Dot rail layout hides the launcher's status
bar and bottom gesture indicator. The usage reference predates the indicator
change; see [capture dates and details](docs/screenshots/README.md).

| Ready | Caption layout | Session usage |
| --- | --- | --- |
| ![Ready screen with Start button](docs/screenshots/ready.png) | ![Synthetic user and assistant captions in 36-pixel Roboto](docs/screenshots/conversation.png) | ![Session usage and model names](docs/screenshots/usage.png) |

## Waveshare example app

[`examples/waveshare`](examples/waveshare) targets the **V2 CO5300 display** board
with 16 MB flash, 8 MB PSRAM, touch input, microphone, and ES8311 audio codec.
It provides:

- One Start/Stop button whose color reflects connection state.
- Compact cyan user captions and warm sand assistant captions with a dotted turn rail.
- A session-end page with model names, voice duration, and backend token usage.
- Saved Wi-Fi profiles and automatic fallback when a network is unavailable.
- Responses delegation with `gpt-6-luna`, priority tier, and hosted web search.
- USB controls for diagnostics, timing, and actual device screenshots.
- A [USB screen mirror](examples/waveshare/README.md#usb-screen-mirror-and-demo-recording)
  with a laptop viewer and screen-only video recording.

The example uses `gpt-live-1` for voice. Model availability and API usage charges
depend on your account. Model names can be configured at build time.
The launcher retains its other apps, including Settings, but excludes the vendor
Xiaozhi assistant and its separate font, emoji, prompt-audio, and speech-model
dependencies. Live Voice uses the shared Roboto fonts.

**[Build, configure, and flash the Waveshare app →](examples/waveshare/README.md)**

## M5Stack example app

[`examples/m5stack`](examples/m5stack) adapts the same SDK to the **CoreS3 LITE**:
a 320 × 240 touch UI, ES7210 microphones, AW88298 speaker, saved Wi-Fi profiles,
colored paginated captions, and session usage. Its board drivers and build are
separate from Waveshare; the protocol, WebRTC, Opus, and playback buffering are
shared. See the [build and flash guide](examples/m5stack/README.md).

## Sonatino example app

[`examples/sonatino`](examples/sonatino) runs the shared stack on the **Sonatino
revision 3.1.x** audio board with a CTIA headset. USB commands provide session
control, volume, transcript snapshots, and diagnostics. Once Wi-Fi connects,
press and release BOOT while idle to start; use RESET to end by rebooting, or
USB `live stop` for graceful close. It has no screen or launcher and does not
start a session automatically. See the
[hardware setup and build guide](examples/sonatino/README.md).

## Build and test the core

Install [ESP-IDF 5.5.2](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/index.html),
including its ESP32-S3 tools, and activate its `export.sh`. The documented command
line workflow targets macOS/Linux. From this repository:

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
python -m unittest discover -s tests -p 'test_*.py'
```

Native tests cover protocol parsing and lifecycle, Wi-Fi selection, transcript
buffering, PCM conversion/priming, usage snapshots, response deduplication,
TLS cache lifecycle, and Sonatino button debouncing. They use synthetic fixtures,
need no API key, and enable ASan/UBSan by default. Use
`-DLIVE_SANITIZE=OFF` if your compiler lacks those runtimes.

To run the portable protocol tests as ESP32-S3 firmware:

```sh
idf.py -C examples/protocol_smoke set-target esp32s3 build merge-bin
python tools/run_emulator.py qemu
python tools/run_emulator.py esp-emu
```

Install the desired emulator first; see [testing and emulation](docs/testing.md).
Emulator tests cover protocol/runtime behavior, not the board's microphone,
speaker, display, Wi-Fi reliability, or a complete voice call.

## Current limits

This is an early SDK. A bounded 120 ms playback reserve absorbs short arrival
delays; it is not an adaptive jitter buffer. There is no acoustic echo
cancellation or packet-loss concealment in the example. Audio quality and
interruptions need further tuning.
The named Waveshare V2, M5Stack CoreS3 LITE, and Sonatino 3.1.x boards have been
exercised end to end. These are short hardware checks, not long-duration soak
tests. Other boards need their own audio/display integration.

The portable core supports creation, mute, close, and raw event forwarding.
It does not yet provide typed APIs for arbitrary tools, session updates, or
client-delegation completion. Payload and buffer limits are documented in the
[core guide](docs/core.md). Reconnection creates a new session.

Live reports voice usage in seconds, not frontend token counts. Backend tokens
come from delegated Responses events. After an interrupted connection, the last
received snapshot may omit in-flight usage. See the public
[usage guide](https://developers.openai.com/api/docs/guides/voice-latency-cost#monitor-actual-usage).

## License and contributions

Original SDK/example code and locally maintained portable transport changes are
provided under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for
attribution.
The portable WebRTC package retains libpeer's MIT attribution and uses separately
licensed cryptographic and SCTP dependencies. The shared UI fonts use Apache-2.0 Roboto 2.138;
[font provenance](examples/common/fonts/README.md) includes the exact source and
regeneration steps. Downloaded launcher/LVGL fonts retain their separate terms. All three ESP-IDF examples use an audio codec with
Espressif-only terms; Waveshare and M5Stack also resolve other restricted
components. Those terms do not apply to the standalone portable transport.
See [third-party notices](THIRD_PARTY_NOTICES.md)
for the dependency inventory, license exceptions, and upstream sources.

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidelines and the
[Contributor License Agreement](CLA.md). Support and response times are not
guaranteed. Include your board revision, ESP-IDF version, reproduction steps,
and sanitized logs. Do not include API keys, Wi-Fi credentials, private
conversations, or firmware images.
