# Device screenshots

These PNGs are direct framebuffer captures from Live Voice running on a physical
Waveshare ESP32-S3-Touch-AMOLED-1.8 V2. They show the app area (368 × 404 pixels),
excluding the launcher status bar. They are not camera photographs and do not
measure physical display brightness.

- `ready.png`: ready to start a session.
- `conversation.png`: assistant captions during an active Live session.
- `usage.png`: model names, voice duration, and delegated backend token usage
  after a completed session.

The ready screen and the conversation/usage pair were captured during device
tests. Displayed usage is illustrative and is not a pricing estimate. They were decoded from the device's RGB565 USB
output with [`decode_screen.py`](../../tools/decode_screen.py). The decoder
expands RGB565 to RGB888 without changing the layout or text and writes no log
text or PNG metadata. No UI elements were composited or retouched.

See the [USB capture instructions](../../examples/waveshare/README.md#usb-diagnostics-and-screenshots)
to capture your own device. Review the pixels for private conversation content
before sharing; keep the original USB logs local.
