# Shared example helpers

These small, hardware-independent C++ helpers are shared by the Waveshare and
M5Stack examples:

- `CaptionText.hpp`: bounded UTF-8 handling and readable font fallback.
- `TranscriptPage.hpp`: incremental, non-scrolling transcript pagination.
- `UsageSummary.hpp`: model, duration, and token formatting.
- `WifiSelection.hpp`: signal-strength selection and retry/DHCP timing policy.

They do not own hardware, threads, credentials, or networking. Board-specific
code supplies font measurements, UI objects, Wi-Fi events, and audio callbacks.
The root host tests cover these helpers under ASan/UBSan.
