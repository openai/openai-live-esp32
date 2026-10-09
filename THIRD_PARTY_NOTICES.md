# Third-party notices

The original SDK/example source is MIT-licensed. Vendor sources and binary
libraries downloaded for a device build retain their own licenses; they are
not relicensed by this repository.

| Dependency | Pinned revision | License / source |
| --- | --- | --- |
| Waveshare launcher | `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4` | [Apache-2.0](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/LICENSE) |
| Waveshare board components | `30d0ac3b8b6b27ebd402c5852819cbe0dab92749` | [Apache-2.0](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/30d0ac3b8b6b27ebd402c5852819cbe0dab92749/LICENSE) |
| Espressif peer connection | `ed834a3dc7c47e9a22a589dd0c950843918a548c` (`esp_peer` 1.5.5) | [Espressif Modified MIT](https://github.com/espressif/esp-webrtc-solution/blob/ed834a3dc7c47e9a22a589dd0c950843918a548c/components/esp_peer/LICENSE): use restricted to Espressif products |
| ESP-IDF | `v5.5.2` | [ESP-IDF licenses](https://github.com/espressif/esp-idf/tree/v5.5.2) |
| Espressif audio codecs | `2.4.1` | [Component registry](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.4.1); see the downloaded component's license |
| cJSON | Supplied by the selected ESP-IDF/launcher build | [MIT](https://github.com/DaveGamble/cJSON/blob/master/LICENSE) |
| Caption font (Montserrat bitmap subset) | LVGL `c016f72d4c125098287be5e83c0f1abed4706ee5` source font | [SIL Open Font License 1.1](examples/waveshare/fonts/LICENSE.OFL); [provenance](examples/waveshare/fonts/README.md) |

`tools/prepare_voice.py` applies an integration overlay to the pinned Waveshare
sources. Portions of the Wi-Fi/display integration are adapted from those
Apache-2.0 sources, including system-status code copyright 2026 Waveshare. The
upstream notices remain in the downloaded sources. The Apache-2.0 license is
included in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).

The preparation tool verifies the requested Git blob hashes before patching.
It downloads vendor assets and build dependencies into an external local cache.
The generated caption-font bitmap subset is shipped as C source with its license;
the original TTF and converter are not required for normal builds. Before distributing a product,
review all resolved dependency licenses and retain the required notices.
