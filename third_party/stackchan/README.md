# StackChan hardware protocol provenance

The StackChan example's `Motion.cpp` and `MotionPolicy.hpp` adapt the register
map, packet framing, big-endian servo words, base power control from
[M5Stack/StackChan-BSP at e2f3b02dbe7f28e26a02adc72bef3aa2c545e4a0](https://github.com/m5stack/StackChan-BSP/tree/e2f3b02dbe7f28e26a02adc72bef3aa2c545e4a0):

- `src/M5StackChan.cpp` and `src/drivers/PY32IOExpander/` — M5Stack, MIT;
  retained [license](M5Stack-LICENSE).
- `src/drivers/FTServo_Arduino/src/{SCS,SCSerial,SCSCL}.{cpp,h}` — FTServo,
  MIT; retained [license](FTServo-LICENSE).

Factory calibration keys, nominal zeros, angle conversion, and base supply
sequencing follow [`firmware/main/hal/hal_servo.cpp`](https://github.com/m5stack/StackChan/blob/28b9f0c8bd6acd81cb3c70f1a93c3e373b39d400/firmware/main/hal/hal_servo.cpp)
and [`firmware/main/hal/board/stackchan.cc`](https://github.com/m5stack/StackChan/blob/28b9f0c8bd6acd81cb3c70f1a93c3e373b39d400/firmware/main/hal/board/stackchan.cc)
in M5Stack's factory repository at `28b9f0c8bd6acd81cb3c70f1a93c3e373b39d400`
(MIT; the same retained M5Stack notice).

Our implementation uses ESP-IDF directly, fixed-size packets, checked servo
status/checksum/length/IDs, bounded transactions, strict tool arguments,
measured completion, cancellation, conservative travel/speed limits, and
fault-triggered motor power-off. It does not include the Arduino framework,
M5Unified, spring-animation library, NFC, infrared, or factory application.
It does not write servo EEPROM or factory calibration.
