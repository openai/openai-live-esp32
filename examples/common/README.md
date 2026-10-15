# Shared example helpers

These small, hardware-independent C++ helpers serve the device examples:

- `CaptionText.hpp`: bounded UTF-8 handling and readable font fallback.
- `TranscriptPage.hpp`: incremental, non-scrolling transcript pagination.
- `UsageSummary.hpp`: model, duration, and token formatting.
- `WifiSelection.hpp`: signal-strength selection and retry/DHCP timing policy.

They do not own hardware, threads, credentials, or networking. Board-specific
code supplies font measurements, UI objects, Wi-Fi events, and audio callbacks.
The root host tests cover these helpers under ASan/UBSan.

`StandaloneWifi.hpp` / `.cpp` provide the ESP-IDF network manager shared by
M5Stack and Sonatino: saved-profile selection, reconnects, DHCP grace periods,
SNTP, and SDK network-state notifications. This implementation owns a worker
and ESP-IDF Wi-Fi events; it is compiled by each standalone app with its private
credential header. Waveshare continues using its launcher-specific manager.
