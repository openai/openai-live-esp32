# Testing and emulation

Activate ESP-IDF 5.5.2 before running these commands from the repository root.

## Native tests

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
python -m unittest discover -s tests -p 'test_*.py'
```

Four CTest targets cover protocol handling, Wi-Fi selection, transcript buffering,
and usage accounting. Python tests cover credential configuration and framebuffer
decoding. Tests use
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
