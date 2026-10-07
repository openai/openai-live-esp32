# Testing and emulation

Activate ESP-IDF 5.5.2 before running these commands from the repository root.

## Native tests

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
python -m unittest discover -s tests -p 'test_*.py'
```

Six CTest targets cover protocol handling, Wi-Fi selection, transcript buffering,
usage accounting, preconnection state, and mirror encoding. Python tests cover
credential configuration, framebuffer decoding, and fragmented/corrupted mirror
streams, including recovery and bounded malformed input. Tests use
synthetic inputs and never call the Live service. AddressSanitizer and
UndefinedBehaviorSanitizer are enabled by default.

Host CMake reads cJSON from `$IDF_PATH/components/json/cJSON`. Set
`-DCJSON_DIR=/path/to/cJSON` to exercise the parser used by another firmware build.

## ESP32-S3 protocol firmware

```sh
idf.py -C examples/protocol_smoke set-target esp32s3 build merge-bin
python tools/run_emulator.py qemu
python tools/run_emulator.py esp-emu
```

Install [Espressif QEMU](https://github.com/espressif/qemu) through IDF:

```sh
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
. "$IDF_PATH/export.sh"
```

Install [esp-emu](https://github.com/espressif/esp-emulator/releases) separately
and put its executable on `PATH`. The tested versions are Espressif QEMU
`esp_develop_9.2.2_20250817` and esp-emu `0.45.0` on macOS arm64. Follow the
emulator's platform dependency instructions as needed.

Both runners require `LIVE_TESTS_PASS` after the protocol assertions, 100 repeated
suites, and a heap-integrity check. A timeout, failure marker, or premature exit
fails the runner. Logs stay in the ignored firmware build directory. QEMU uses
a padded 4 MiB flash image; esp-emu uses the merged image directly.

These runs establish CPU/runtime and protocol-test behavior. They do not verify
Wi-Fi, I2S audio, touch input, or the Waveshare display. Use the physical device
for complete Live sessions.

## Physical device checks

After following the [Waveshare guide](../examples/waveshare/README.md):

1. Confirm the launcher opens Live Voice and enables Start after DHCP.
2. Start a call and check HTTP 201, `session.started`, and advancing audio counters.
3. Speak and verify audible replies plus both transcript colors.
4. Ask for a web lookup and inspect nested Responses lifecycle events.
5. Stop and check `finalized=1`, final duration, backend usage, and the usage page.
6. Test network-loss behavior separately: retained usage should be labeled
   "Last reported" and a new call should require Start.

Startup timing includes network transit. HTTPS connection time combines
DNS/TCP/TLS; it does not isolate API processing. First received audio can be
silence and is not the assistant's first spoken word.

## Initial validation

The standalone checkout passed all four native CTest targets with ASan/UBSan,
all five Python tests, and both emulator runs with 100 protocol suites and stable
heap usage. A fresh Waveshare build was flashed to the physical V2 device. It
completed a Live session, received assistant captions, ran a delegated web
search with `gpt-6-luna`, and closed gracefully with final voice and backend
usage. The [screenshots](screenshots/README.md) include that conversation and
its final usage page.

## USB mirror validation

The mirror passed the six native CTest targets with ASan/UBSan and all eleven
Python tests. On the physical Waveshare V2, its binary feed reconstructed the
368 × 448 display without CRC/decoder errors. Browser pause/resume and fullscreen
worked, and a roughly seven-minute VP9 WebM recording decoded successfully.
Five capture start/stop cycles returned to 19 idle tasks and a stable internal
heap; the 345 KiB pixel buffers were released each time.

During one established WebRTC session with mostly silent input, consecutive
28-second mirror-off and 22-second mirror-on CPU samples showed:

| Measurement | Mirror off | Mirror on |
| --- | --- | --- |
| Mirror worker (% of one core) | 0.03% | 0.83% |
| Core 0 idle | 58.7% | 58.4% |
| Core 1 idle | 79.6% | 79.0% |
| Microphone / playback queue drops | 0 / 0 | 0 / 0 |
| Audio send errors | 0 | 0 |

Forced screen resends and an eight-second host-process stall exercised
backpressure. The lease expired, capture buffers were released, and the feed
recovered after the viewer resumed. The largest observed flush-copy wall time
was 2.18 ms, including task preemption. Local microphone/Opus/speaker diagnostics
passed with mirroring off and with mirroring plus forced screen resends on.

A final repeat on the flashed build completed an 86-second WebRTC session with
zero microphone/playback queue drops and zero send errors, including forced
screen resends. Equal 20-second intervals measured 0.03% of one core in the
mirror worker while off and 0.90% while on. A 12.6-second interval containing a
7.5-second resend burst measured 5.24%; both cores remained at least 56% idle.
Eighteen peer sends exceeding 20 ms occurred before the comparison; that count
did not increase during either comparison interval or the resend burst. The
session closed gracefully and returned to 19 idle tasks. Input was mostly
silent, so this still does not replace a spoken-conversation listening check.

**Voice-quality limitation:** later in the longer WebRTC run, microphone queue
drops developed and persisted with mirroring disabled and its buffers freed.
CPU headroom remained substantial. Subsequent connection attempts also hit
TLS/HTTP connection failures. These observations do not establish the cause of
the send stalls, and the low mirror CPU cost is not proof of glitch-free audible
speech. A listening check and a longer comparison on a reliable network remain
necessary. The new `send_max_us` / `send_slow` diagnostics help distinguish
blocked peer sends from display CPU overhead during that follow-up.

To repeat the comparison, run the mirror helper with a private log and command
file, start a conversation on the device, and collect two `live diagnostics`
snapshots around equal intervals with `live mirror-off` and `live mirror-on`.
Compare per-task runtime deltas, audio queue drops, send durations, and audible
speech. Check `live mirror-status` and the viewer's `/status` endpoint for USB
stalls and decode errors. Keep logs and recordings out of the repository.
