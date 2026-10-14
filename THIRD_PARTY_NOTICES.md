# Third-party notices

Original SDK/example code and locally maintained portable transport changes are
MIT-licensed. Third-party code, fonts, vendor assets, and libraries retain their
own licenses; the repository's MIT license does not relicense them. This is a
source distribution, not a redistributable firmware bundle.

## Portable protocol and WebRTC libraries

| Dependency | Source / version | License and attribution |
| --- | --- | --- |
| libpeer-derived transport | `sepfy/libpeer` at `5b849de378545c31d34759a145413846953e1366` | [MIT, copyright 2021 sepfy](packages/peer/LICENSE.libpeer) |
| Mbed TLS | Host release 3.6.7; ESP-IDF fork `2b96dd8eebe880f304c69976b3c2fa0c5100cbb6` | [Apache-2.0 OR GPL-2.0-or-later](packages/peer/licenses/LICENSE.mbedtls); Apache-2.0 selected. Additional source-file notices apply. |
| libSRTP | 2.8.1, `6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa` | [BSD-3-Clause](packages/peer/licenses/LICENSE.libsrtp) |
| usrsctp | `c625e21950600460cb5c6075616eb1622df2b06a` | [BSD-3-Clause](packages/peer/licenses/LICENSE.usrsctp), plus individual source-file notices |
| cJSON | Supplied by ESP-IDF for host tests; registry component 1.7.19~2 for the device build | [MIT, copyright Dave Gamble and contributors](https://github.com/DaveGamble/cJSON/blob/v1.7.19/LICENSE) |

See [portable transport provenance](packages/peer/docs/provenance.md) for source
links, archive verification, adaptations, and test-tool dependencies. No
`esp_peer` source or binary is incorporated or linked. The portable package does
not depend on Espressif's restricted peer or audio libraries.

## Waveshare integration and font

| Dependency | Pinned source | License / attribution |
| --- | --- | --- |
| Waveshare launcher | `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4` | [Apache-2.0](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/LICENSE); nested components and assets retain their own notices |
| Nested Xiaozhi source | Within the pinned launcher above | [MIT](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/firmware/brookesia/components/XiaozhiApp/third_party/xiaozhi_esp32/LICENSE), copyright 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors; this code license does not establish rights to all associated assets |
| Waveshare board components | `30d0ac3b8b6b27ebd402c5852819cbe0dab92749` | [Apache-2.0](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/30d0ac3b8b6b27ebd402c5852819cbe0dab92749/LICENSE) |
| WAV file player | `chmorgan/esp-audio-player` 1.1.0, `3cfeb3459e6c9be8055b507f520b2f5e8800b742` | [Apache-2.0](https://github.com/chmorgan/esp-audio-player/blob/3cfeb3459e6c9be8055b507f520b2f5e8800b742/LICENSE); modified to remove Helix MP3 resolution/linking and disable MP3 playback |
| Caption font (Montserrat bitmap subset) | LVGL `c016f72d4c125098287be5e83c0f1abed4706ee5` source font | [SIL Open Font License 1.1](examples/waveshare/fonts/LICENSE.OFL), copyright 2011 The Montserrat Project Authors; [provenance](examples/waveshare/fonts/README.md) |
| ESP-IDF | `v5.5.2` | [Apache-2.0 and component-specific licenses](https://github.com/espressif/esp-idf/tree/v5.5.2) |

`tools/prepare_voice.py` applies an integration overlay to the pinned sources.
Wi-Fi/display/audio adaptations include system-status code copyright 2026
Waveshare and board/launcher code copyright Espressif Systems. Upstream copyright
and license headers remain intact. Modified source, configuration, and build
files carry an OpenAI modification notice. The Apache-2.0 license is included in
[`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).

The generated caption-font bitmap is shipped as C source under OFL-1.1; the TTF
and converter are not required for normal builds. This font's notice does not
cover the launcher's separately downloaded fonts.

## Device-only dependencies

The following components have terms limited to use with Espressif products.
They belong to the ESP-IDF adapter or inherited launcher, not the portable
transport. Do not redistribute them for use on non-Espressif hardware.

| Component | Reviewed version | License |
| --- | --- | --- |
| `espressif/esp_audio_codec` | 2.4.1 | [Espressif Modified MIT](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.4.1/license) |
| `espressif/esp_audio_effects` | 1.3.0~1 | [Espressif Modified MIT](https://components.espressif.com/components/espressif/esp_audio_effects/versions/1.3.0~1/license) |
| `espressif/gmf_fft` | 1.0.0 | [Espressif Modified MIT](https://components.espressif.com/components/espressif/gmf_fft/versions/1.0.0/license) |
| `espressif/esp-sr` | 2.4.7 | [Espressif MIT](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.7/license); includes separately downloaded speech assets |
| `espressif/esp_new_jpeg` | 1.0.2 | [Espressif MIT](https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2/license) |

The inherited launcher also resolves the following components. These versions
record the inspected build, not additional pins: transitive constraints can
resolve differently. Inspect the generated `dependencies.lock`, component
manifests, license files, and individual source notices for the actual build.

| Component(s) and reviewed versions | License evidence |
| --- | --- |
| `lvgl/lvgl` 9.4.0 | MIT (`LICENCE.txt`); bundled fonts/assets can have separate terms |
| `chmorgan/esp-file-iterator` 1.0.0 | Apache-2.0 notices in `file_iterator.c` and its header |
| `espressif/esp_codec_dev` 1.5.11; `esp-dsp` 1.8.0; `esp-lib-utils` 0.3.0 | Apache-2.0 license files |
| `espressif/esp-boost` 0.3.1 | Apache-2.0, with Boost-derived portions under Boost Software License 1.0; both texts are in `license.txt` |
| `espressif/esp_io_expander` 1.2.1; `esp_io_expander_tca9554` 2.0.3; `esp_lcd_co5300` 2.2.0; `esp_lcd_panel_io_additions` 1.0.1~1 | Apache-2.0 license files |
| `espressif/esp_lcd_touch` 1.2.1; `esp_lcd_touch_cst816s` 1.1.2; `esp_lcd_touch_ft5x06` 1.1.1; `esp_lvgl_port` 2.9.0 | Apache-2.0 license files |
| `espressif/avi_player` 2.0.0; `cmake_utilities` 0.5.3; `esp_mmap_assets` 1.3.2; `espressif2022/image_player` 1.1.1 | Apache-2.0 license files |
| `espressif/esp_websocket_client` 1.8.0; `esp_xiaozhi` 0.1.1; `mcp-c-sdk` 2.0.1; `mqtt` 1.1.0 | Apache-2.0 license files; nested code can carry separate notices |
| `waveshare/pcf85063a` 2.0.0; `waveshare/qmi8658` 2.0.1 | Apache-2.0 license files |
| `espressif/dl_fft` 0.8.0 | Manifest declares MIT, while inherited FFT source files carry Apache-2.0 notices; preserve the applicable per-file terms |
| `78/xiaozhi-fonts` 1.6.0 | Manifest declares MIT; this is not a verification of the rights to every bundled font. Check font-specific licenses before redistribution. |

The build downloads the full launcher, including its other apps, media,
speech-model assets, and fonts, into an external cache. They are not committed
here. A top-level code license is not evidence that every bundled asset has the
same license or is cleared for redistribution. In particular, independently
review these assets before distributing the launcher or its filesystem images.

Earlier builds used `esp_peer` and the launcher's Helix MP3 decoder. The current
overlay removes Helix from the player manifest and CMake dependencies, disables
MP3, and keeps WAV support. The Helix wrapper's Apache license did not relicense
the nested RealNetworks RPSL/RCSL code. Historical firmware retains the
dependencies it was built with; rebuild to obtain the WAV-only profile.

## Redistribution

Preparation verifies pinned Git blob or archive hashes before patching and keeps
upstream notices in the downloaded source. Preserve all applicable license,
copyright, modification, and NOTICE text when redistributing source or binary
components. Review the resolved dependency graph and nested assets rather than
treating this inventory as a blanket license grant. Never distribute builds
containing API keys, Wi-Fi credentials, or private data.
