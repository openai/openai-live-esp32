# Portable peer connection (experimental)

An independently buildable C WebRTC library for Opus audio and data channels.
**Unsupported experiment: not an officially supported OpenAI SDK, and not a
production security certification.** The API and supported protocol subset may
change. The Waveshare app uses this transport through `components/live_peer`.

This package is derived from MIT-licensed
[libpeer](https://github.com/sepfy/libpeer/tree/5b849de378545c31d34759a145413846953e1366),
with extensive changes to parsing, authentication, state handling, and resource
limits. No esp_peer code or binaries are included: its Modified MIT license
restricts use to Espressif products. See [provenance and licenses](docs/provenance.md)
and the [security review](docs/security-review.md), including the
[comparison with published upstream issues](docs/upstream-issues.md).
The [correctness review](docs/correctness-review.md) records ordinary protocol
and lifecycle fixes, their regressions, and remaining timing limitations.

## Scope and architecture

```text
Application: authenticated signaling + audio codec/capture/playback
                            |
                    Public peer C API
                            |
                    Bounded SDP parser
                            |
                  ICE checks / UDP socket
                            |
            DTLS 1.2 + pinned SHA-256 certificate
                     /                 \
               libSRTP                usrsctp
            Opus RTP / RTCP         DCEP / messages
```

The application sends the generated offer over its authenticated signaling
channel and supplies the complete SDP answer. ICE selects a source address;
DTLS verifies the answer's certificate fingerprint before enabling SRTP or SCTP.
Audio callbacks receive encoded Opus; this library does not encode, decode, mix,
resample, or play audio. Signaling, Live API messages, and credentials remain in
the application. The standalone host build has no ESP-IDF dependency.

Implemented and tested:

- Local offerer, complete bundled SDP answer, one audio track and/or data transport.
- IPv4 UDP host candidates and optional STUN server-reflexive gathering.
- ICE controlling role, source/transaction/HMAC checks, periodic consent checks.
- Both DTLS roles; ECDHE-ECDSA with AES-128-GCM, P-256 local certificate;
  SRTP AES128-CM/HMAC-SHA1-80, replay protection for RTP and RTCP.
- Opus payload 111, 48 kHz RTP clock, one 20 ms encoded packet per send.
- Reliable ordered data channels, UTF-8 text or binary, bounded reassembly.
- POSIX host backend and a separate ESP-IDF/LwIP port for the Waveshare example.
- One authenticated local peer-reflexive mapping learned during initial ICE
  nomination, allowing a NAT to change the local host address/port.

Explicitly unsupported: answering offers, renegotiation/ICE restart, trickle ICE,
ICE role conflicts, remote peer-reflexive candidate discovery, TURN allocations, TCP,
IPv6, mDNS resolution, video, other audio codecs, unordered/partial-reliability channels,
channel reopening/reset, unidirectional/inactive audio, RTP retransmission, congestion control, jitter buffering,
and outbound RTCP reports. Authenticated incoming RTCP is validated and counted;
RTCP feedback does not yet adjust media behavior. This is a bounded framework,
not a complete WebRTC implementation. Valid unsupported TCP/IPv6/hostname
candidates in an answer are skipped without DNS lookups; at least one usable
numeric IPv4/UDP candidate is required.
Changes to the selected local mapping require a new connection.

STUN configuration accepts `stun:hostname[:port]` or numeric IPv4, with default
port 3478. Malformed and unsupported URI forms fail at peer creation. Each
configured server has a one-second gathering window with one retransmission
after 500 ms; this remains a bounded subset of STUN's full retry procedure.

Limits: SDP <8 KiB, lines <=1,023 bytes, 10 unique remote candidates, 1,536-byte
UDP datagrams, one authenticated remote audio SSRC, 5 channels with IDs <16,
31-byte labels/protocols, 64 KiB messages, 128 KiB SCTP send/receive buffers,
and two buffered early ClientHello datagrams (released only after ICE validation).
Fragmented DCEP control messages are rejected. Messages exceeding the limit are
discarded through their end marker so their tails cannot become new messages.

## Build and run

Requires CMake 3.24+, Ninja, Python 3, and Clang or GCC. CMake downloads SHA-256
verified source archives for **Mbed TLS 3.6.7, libSRTP 2.8.1, and usrsctp**. A
network connection is needed for the first build. No API key is needed for tests.

```sh
cmake -S packages/peer -B build-peer -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-peer -j 4
ctest --test-dir build-peer --output-on-failure
uv sync --locked --project packages/peer/tests/interop
uv run --locked --project packages/peer/tests/interop \
  packages/peer/tests/interop/run.py ./build-peer/peer_interop --attacks --rounds 3
uv run --locked --project packages/peer/tests/interop \
  packages/peer/tests/interop/run.py ./build-peer/peer_interop --server --reject
```

ASan/UBSan are enabled by default for the library **and all three dependencies**.
Set `LIVE_PEER_SANITIZE=OFF` for a normal build. To use a separately maintained
Mbed TLS 3.6.7+ (3.x) installation with DTLS-SRTP enabled, set
`LIVE_PEER_SYSTEM_MBEDTLS=ON` and `CMAKE_PREFIX_PATH`; sanitizer coverage then depends
on that installation. Mbed TLS 4 needs a separate adapter.

The local harness uses pinned aiortc as an independent peer, restricts traffic to
loopback, sends audio in both directions, opens channels from both ends, and
checks authentication failures without disrupting valid traffic.

## Use as a separate package

Copy this directory with its license files, or embed it as a CMake subdirectory.
It builds without the rest of the repository:

```cmake
set(LIVE_PEER_SANITIZE OFF CACHE BOOL "")
set(LIVE_PEER_BUILD_EXAMPLES OFF CACHE BOOL "")
add_subdirectory(path/to/peer peer-build)
target_link_libraries(my_application PRIVATE live_peer::peer)
```

Call `peer_init()`, create a `PeerConfiguration` with `CODEC_OPUS` and/or a data
channel type, then `peer_connection_create()`. Register callbacks, create an offer,
and supply a complete answer using `peer_connection_set_remote_description_n()`.
Poll `peer_connection_loop()` every 1–5 ms on the owner thread. Send media only
when connected and messages only when the desired channel has opened. Destroy
all peers before `peer_deinit()`; shutdown returns an error while peer objects
remain. See [the driver](examples/interop.c) for a complete local example.

All API calls and all peers share **one owner thread**. Send operations from
callbacks are supported. Queue close/destroy operations until the callback and
current API call have returned; do not recursively poll. Callback data is borrowed
and length-delimited, with no implied NUL terminator. Configuration strings are
copied/consumed at creation; application `user_data` must outlive the peer.

The host backend uses POSIX sockets, a monotonic clock, and Mbed TLS entropy.
UDP sends are nonblocking. Audio sends report encryption/transmission failures;
DTLS write failures, including backpressure, terminate the session rather than
reuse a pending SSL write with a different SCTP buffer. SCTP restart/reset is
unsupported and requires a fresh peer. Send paths enforce consent expiry even
between polls, but timely polling is still required for receiving revocations.
A non-POSIX port must implement those facilities and validate usrsctp's platform
configuration. Hardware entropy, thread constraints, memory/CPU limits, and
real-device audio timing need fresh validation on each device.

The ESP-IDF component uses LwIP, ESP-IDF network-interface accessors, and hardware
entropy while Wi-Fi is active. SCTP uses AF_CONN only: the owner drives input and
timers, with no raw-IP receive or timer threads. Upstream still creates one
internal iterator thread, protected by pthread locks and joined during shutdown.
The preparation tool adapts default rwlock attributes, socket layout, entropy,
and libSRTP length types for ESP-IDF. See the Waveshare build instructions and
the [device validation record](docs/validation.md).

## Fuzzing

Use Clang with libFuzzer installed (Apple's bundled Clang may lack that runtime).

```sh
cmake -S packages/peer -B build-peer-fuzz -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DLIVE_PEER_FUZZ=ON
cmake --build build-peer-fuzz -j 4
python3 packages/peer/tests/interop/seed_fuzz.py build-peer-fuzz/corpus
./build-peer-fuzz/peer_fuzz build-peer-fuzz/corpus -max_total_time=60 -max_len=8193
./build-peer-fuzz/peer_fuzz_sctp build-peer-fuzz/corpus-sctp -max_total_time=60 -max_len=8192
uv run --locked --project packages/peer/tests/interop \
  packages/peer/tests/interop/run.py ./build-peer-fuzz/peer_interop \
  --corpus build-peer-fuzz/dtls-corpus
./build-peer-fuzz/peer_fuzz_dtls build-peer-fuzz/dtls-corpus -max_total_time=60 -max_len=16384
```

The DTLS fuzzer disables cookies **only in its test context** to reach deeper
parser states. The SCTP fuzzer connects two in-memory associations, then repairs
the verification tag/checksum of mutated chunks to exercise parsing after DTLS
authentication. Neither harness opens external sockets. Production behavior is
unchanged by these test-only stubs. Short runs are regression smoke tests; sustained
fuzzing, coverage review, and independent security review remain necessary.
