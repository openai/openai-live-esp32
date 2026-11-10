# ESP-Mosaico Live Voice

> **Experimental project. Not officially supported by OpenAI.**

A standalone Live voice app for Espressif's **ESP-Mosaico**, including its
camera and interaction expansion modules. Mosaico uses an **ESP32-S31**,
not an ESP32-S3: dual 320 MHz RISC-V cores, PSRAM, a 480 × 480 square
AMOLED, ES8311 audio, IMU, two magnetometers, a battery gauge, and a vibration
motor. This example uses the shared SDK and portable WebRTC transport.

See Espressif's [board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html),
[camera guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_camera.html),
and [interaction-module guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_interact.html).
Connect the camera to the **left slot** and interaction module to the **right
slot**, following those guides. The BSP discovers the board revision and camera
model; this app does not hard-code an S3 pinout. Attach the modules before
boot; the interaction module is initialized during startup.

## Build and flash

Mosaico needs the preview S31 toolchain. Keep this installation separate from
the **ESP-IDF 5.5.2** installation used by the S3 examples. The tested source pin
is ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` (6.2 preview):

```sh
git clone --filter=blob:none https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-mosaico"
git -C "$HOME/esp/esp-idf-mosaico" checkout 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe
git -C "$HOME/esp/esp-idf-mosaico" submodule update --init --recursive
export IDF_TOOLS_PATH="$HOME/.espressif-mosaico"
"$HOME/esp/esp-idf-mosaico/install.sh" esp32s31
. "$HOME/esp/esp-idf-mosaico/export.sh"
python -m pip install idf-component-manager==3.1.2
```

From this repository's root, with your API key and Wi-Fi profiles stored outside
the repository:

```sh
python tools/prepare_mosaico.py \
  --key-file "$HOME/.config/openai-live-esp32/api-key" \
  --wifi-file "$HOME/.config/openai-live-esp32/wifi.json"
export LIVE_VOICE_PRIVATE_DIR="$HOME/.cache/openai-live-mosaico/private"
export LIVE_PEER_DEPS_DIR="$HOME/.cache/openai-live-mosaico/vendor/peer"
idf.py --preview -C examples/mosaico build
```

Preparation checks source hashes, downloads the BSP and USB console into the
ignored `vendor/` directory, and writes credentials into the external private
cache. Keep the default vendor location for portable dependency-lock paths.
`LIVE_MODEL` and `LIVE_RESPONSES_MODEL` before preparation choose models;
prompts, priority-tier delegation, web search, and tools are configured in
[`main/main.cpp`](main/main.cpp).

Use a data-capable Type-C cable. **Back up the factory NOR flash before
replacing its application and partition layout**:

```sh
umask 077
python -m esptool --chip esp32s31 --port YOUR_PORT \
  read-flash 0 ALL "$LIVE_VOICE_PRIVATE_DIR/factory-backup.bin"
idf.py --preview -C examples/mosaico -p YOUR_PORT flash monitor
```

The application preserves a high-speed USB CDC console and esptool's software
download-mode reset. Entering ROM download mode can change the serial-port name;
if it does, select the newly enumerated port. Do not substitute the S3 native
USB Serial/JTAG settings: those pins are shared with this board's camera.
The app does not mount, format, or write the separate NAND storage.

Firmware, generated headers, factory backups, and device logs can contain
credentials or conversation content. Keep them private and outside the repo.

## Controls and tools

Tap **Start**, press the board's **AI** button, or press the interaction module's
**left button** to start/stop. The right module button toggles microphone mute
during a session. Cyan/gold captions paginate without scrolling. Stop shows
usage. A captured photo appears for two seconds before captions return.

The six interaction LEDs show amber while connecting, cyan while connected,
red when muted, and turn off when idle. Requested light effects temporarily
override this indication. Sensor events do not automatically start recording.

| Tool | Inputs and behavior |
| --- | --- |
| `capture_photo` | Fresh 320 × 240 JPEG; camera runs only for capture; quality is reduced if needed to fit the SDK's 10 KiB image limit |
| `read_device_sensors` | Acceleration in g, angular velocity in degrees/second, two magnetic vectors, battery readings, button state, PIR motion, and relative light (0–100, not lux); missing sensors are reported unavailable |
| `set_lights` | LED index 0–5 or −1 for all; RGB bytes; duration 100–10,000 ms; brightness limited by the module driver; resets after expiry or Stop |
| `vibrate` | Strength 1–100 and duration 10–500 ms; cancellation stops the motor |
| `send_infrared` | One NEC 38 kHz frame with 8-bit address and command; only for explicitly requested, known codes |

Try “What am I showing you?”, “Read the motion and light sensors,” or
“Turn the lights purple for two seconds and give a short vibration.” The
backend prompt distinguishes a snapshot from continuous video, and PIR motion
from identity or certain occupancy. It treats text in images as untrusted data.

USB diagnostics include `live status`, `live start`, `live stop`, `live mute`,
`live unmute`, `live audio-test`, `live jitter-test`, `live peer-test`,
`live photo-test`, `live memory`, and `live timing`. For a local sensor check:

```text
live tool read_device_sensors {}
```

Camera and hardware tools execute away from the audio/controller tasks. Camera
pixels and temporary JPEG buffers use PSRAM; I²S uses internal DMA memory.
The app retains the shared 120 ms playback reserve and 32-datagram UDP receive
mailbox. It does not implement wake-word activation, echo cancellation,
continuous video, infrared learning, or a NAND media library.

## Dependencies and verification

Board sources and the USB console are pinned and hash-checked by
[`prepare_mosaico.py`](../../tools/prepare_mosaico.py). Registry versions and
hashes are recorded in [`dependencies.lock`](dependencies.lock); see the
[third-party inventory](../../THIRD_PARTY_NOTICES.md#esp-mosaico-integration).

This example uses IDF's Mbed TLS 4.1.1/PSA integration and the existing X.509
empty-name fix. S3 examples keep Mbed TLS 3.6.7. Both paths retain fingerprint
verification, signed DTLS handshakes, SRTP authentication, and replay checks.
The SDK selects peer-certificate retention, which IDF 6 disables by default;
a compile-time guard prevents building without the certificate needed for
fingerprint verification.
Host tests exercise image bounds, pixel conversion, strict tool arguments,
and the same transport/security regressions with both crypto versions.
