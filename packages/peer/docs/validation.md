# Validation record — 2026-10-07

Initial local campaign on macOS arm64 with Clang, ASan/UBSan, Mbed TLS 3.6.7,
libSRTP 2.8.1, the pinned usrsctp revision, and aiortc 1.15.0. These are measured
results for the bounded implementation, not a security guarantee.

| Check | Result |
| --- | --- |
| Security regressions | 625 assertions pass, including local UDP and STUN-server fixtures |
| Independent interoperability | Two-way decoded Opus; channels opened by either peer; both DTLS roles pass |
| Message behavior | Empty, UTF-8, and 64 KiB messages round-trip; oversized/invalid messages rejected and subsequent messages work |
| Live packet rejection | Each adversarial session rejects at least 2 invalid/replayed RTP and 3 invalid/replayed/malformed RTCP packets; valid media continues |
| Fingerprint mismatch | Both roles fail with zero delivered audio/messages |
| Media subsets | Audio-only and data-only sessions pass |
| Lifecycle | Repeated sessions within one process, stable open-descriptor counts, successful global SCTP shutdown; repeated create/close/destroy/init/deinit tests |
| CMake embedding | Separate public-header-only release consumer builds, links, creates an offer, and shuts down |
| Clang static analyzer | No diagnostics in 14 local transport translation units |
| Parser fuzzing | 4,028,973 total inputs over four bounded runs; no sanitizer failure |
| DTLS fuzzing | 17,357 inputs over two bounded runs; no sanitizer failure |
| Stateful SCTP fuzzing | 44,753 inputs over two bounded runs; no sanitizer failure |
| Repository checks | C/Python formatting, Python lint, diff whitespace, and package private-detail scan pass |

Each fuzz run was approximately 61 seconds. Runs were made as the hardening
changes evolved, with affected tests and fuzz targets rerun after changes. The
last parser run, after discovery hardening, processed 699,555 inputs. The last
DTLS and SCTP runs processed 10,591 and 22,223 inputs respectively. Longer campaigns
can expose states these short runs did not reach.

Combined source coverage from unit, integration, and fuzz runs is **85.75% of
local-library lines and 69.45% of branches**. This excludes dependencies and is
not a coverage claim for the entire cryptographic/SCTP stack. Selected local
source line coverage:

| Source | Lines covered |
| --- | ---: |
| SDP | 96.76% |
| STUN | 95.77% |
| ICE agent | 87.50% |
| DTLS/SRTP integration | 89.31% |
| SCTP/DCEP integration | 90.72% |
| RTP framing | 93.18% |
| RTCP framing | 100.00% |

Uncovered branches include allocation/system-call failures, unsupported platform
paths, and compatibility helpers. 100% RTCP line coverage does not mean all
possible report structures are supported or verified; unimplemented report types
have no behavior beyond framing checks/counting.

The Linux GitHub Actions workflow reproduces sanitizer regressions, embedding,
interoperability, both fingerprint failures, media subsets, and three fuzz smoke
tests. Local macOS validation alone does not establish Linux/MCU support. The PR's
completed CI result is the source of truth for Linux execution.

To collect local-source coverage, configure with `LIVE_PEER_COVERAGE=ON` using
Clang, set `LLVM_PROFILE_FILE` to a per-process path (`%p.profraw`) while running
tests, merge with `llvm-profdata merge -sparse`, and use `llvm-cov report` with the
unit, interop, and fuzz executables as objects. Keep raw generated corpora, logs,
profiles, and binaries outside the committed package.
