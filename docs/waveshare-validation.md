# Portable WebRTC device validation

The Waveshare example now builds `packages/peer` from source through the
`live_peer` ESP-IDF component. It does not link `esp_peer`. This is experimental:
the evidence below is a bounded engineering check, not a security certification
or a claim that live audio is production-ready.

## Checks performed

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 V2, ESP-IDF 5.5.2, 16 MB flash,
8 MB PSRAM. Local checks were run on 2026-10-07.

| Check | Result and limits |
| --- | --- |
| ESP-IDF source build and USB app flash | Passed; approximately 6.32 MB app in an 8 MB partition. Flash hash verified. |
| Local audio diagnostic | Passed 150 frames through Opus encode/decode and board audio I/O. The microphone's 500 Hz component rose substantially during the speaker tone. This does not measure conversational quality, echo cancellation, or network jitter. |
| Device peer lifecycle stress | After the wakeup fix, 100 consecutive init/offer/destroy/shutdown cycles passed while app wakeups were triggered every 100 ms. Every cleanup returned to 19 tasks. Free heap stayed between 5,527,080 and 5,532,836 bytes; internal free heap stayed between 63,851 and 68,779 bytes during Wi-Fi retries. Final heap exceeded the initial reading. These cycles do not establish a DTLS/SCTP association or run audio. |
| Core tests | Six native tests and 11 Python tests passed. |
| Portable peer security tests | Passed with AddressSanitizer and UndefinedBehaviorSanitizer, including NAT mapping authentication, mixed ICE candidates, and strict DCEP ACK compatibility cases. |
| Local peer interoperability | Repeated audio/data sessions passed in both DTLS roles, including malformed/replayed packet tests and padded DCEP ACKs. |
| Fuzz smoke tests | 961,364 parser inputs and 12,729 stateful SCTP inputs completed without a sanitizer failure. These are short smoke runs, not exhaustive fuzzing. |
| Live API from a laptop | Three successive sessions in one process passed, with global peer shutdown/reinit between calls. Each received `session.started` and authenticated audio; two graceful stops received `session.closed`, and the middle call was closed locally. Descriptor counts returned to baseline and sanitizers reported no failure. This does not validate the ESP32 scheduler or radio path. |
| Live voice on the device | Incomplete. The test network repeatedly lost beacons at roughly −78 to −83 dBm. Attempts failed in HTTPS or ICE before a usable voice session. |

During porting, an early build leaked a worker after shutdown because ESP-IDF
rejects non-NULL rwlock attributes with `ENOSYS`. The port now uses supported
default attributes with real locks. Longer lifecycle stress also reproduced an
intermittent initialization hang. The adapter used FreeRTOS notification slot
zero for app/network wakeups, while ESP-IDF's `pthread_join` uses that same slot
to wait for thread exit. An unrelated wakeup could prematurely satisfy the join.
App wakeups now use a dedicated binary semaphore. The debugger could not
reliably unwind the affected task, so the diagnosis rests on the conflicting
wait/wakeup code paths and subsequent stress testing, not a captured call stack.

The shared usrsctp preparation patch also clears worker termination flags before
reinitialization after a joined shutdown. The adapter retains runtime ownership
if shutdown fails, preventing subsequent attempts from accumulating init
references. Audio tasks stop before codec and peer resources are destroyed.

## Remaining device acceptance test

Use a stable nearby 2.4 GHz access point. Keep private logs outside the checkout.
Start one USB monitor as described in the [device guide](../examples/waveshare/README.md).

1. Disable idle warmup with `live preconnect-off`, then record `live diagnostics`.
2. Run `live peer-test` four times, waiting for each five-cycle batch to finish.
   Compare idle free/internal heap, largest internal block, and task counts.
   To stress wakeup isolation, append `live preconnect-off` every 100 ms while
   a batch is running; the command must not interrupt SCTP's thread join.
3. Start a call. Confirm `session_started`, `first_audio_rx`, and
   `first_playback` timing checkpoints, then speak and hear an actual response.
4. Hold a conversation for at least five minutes. Record two CPU snapshots,
   queue drops, send errors, codec durations, and packet-arrival statistics.
   Record an audible sample to assess gaps; counters alone cannot prove quality.
5. Stop gracefully and confirm final usage. Wait for task cleanup, then repeat
   at least five calls and compare idle memory after each one.
6. Interrupt Wi-Fi during a call, wait for cleanup and network recovery, then
   start another call. Confirm that the last reported usage survives the hard
   disconnect and no extra tasks accumulate.
7. Repeat a call with the USB screen mirror active and compare audio quality and
   queue/CPU measurements. Restore `live preconnect-on` afterward.

The portable stack currently has no jitter buffer, packet-loss concealment, or
acoustic echo cancellation. These remain possible causes of audible problems
even with stable memory and adequate CPU headroom.
