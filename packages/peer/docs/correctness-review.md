# Correctness review — 2026-10-07

This pass reviewed ordinary connection setup, packet loss, RTP/SCTP delivery,
API return values, and the Waveshare controller/audio teardown paths. It is
separate from the [security review](security-review.md). Findings below are
correctness fixes, not new vulnerability claims.

## Findings fixed

| Finding | Observable effect | Fix and regression |
| --- | --- | --- |
| Failed peers returned success on later polls | An application checking `peer_connection_loop()` could observe failure once, then incorrectly treat the terminal peer as healthy. | Keep returning `-1` while the state is `FAILED`. The real negative-fingerprint interoperability tests poll three more times and require failure on each call. This test failed against the previous implementation. |
| Unsupported hostname candidates invalidated the entire answer | An answer containing an mDNS hostname plus a usable numeric IPv4 candidate was rejected, even though no hostname resolution was necessary. | Validate and skip unsupported hostname candidates, retaining numeric candidates. The mixed-candidate test failed before the fix and now passes; malformed names and answers with no usable candidates still fail. No DNS lookup is performed for remote SDP. |
| STUN URIs without an explicit port were silently ignored | Configuring `stun:host` produced no server-reflexive candidate instead of using the default port. Malformed configuration was also accepted at creation and ignored later. | Share URI validation between peer creation and gathering; default to port 3478 and reject malformed/unsupported forms early. Tests cover the default, an explicit port, case-insensitive scheme, and invalid forms. |
| STUN gathering never retransmitted | A single lost request or response prevented candidate discovery for that server. | Retransmit the same transaction once after 500 ms within the existing one-second window. A loopback server deliberately discards the first request, verifies the repeated transaction/source, and then returns a simulated NAT mapping. |
| Server-reflexive SDP omitted the related address and port | Generated candidates lacked required `raddr`/`rport` fields, risking rejection by stricter peers. | Retain the local base address during discovery and serialize it on non-host candidates. The discovery test distinguishes the simulated NAT mapping from the loopback base and checks the emitted fields. Candidate serialization also fails cleanly if its output buffer is too short. |

The URI behavior follows [RFC 7064 §3.2](https://www.rfc-editor.org/rfc/rfc7064#section-3.2).
Skipping unsupported FQDN candidates and including related addresses follow
[RFC 8839 §5.1](https://www.rfc-editor.org/rfc/rfc8839#section-5.1).
This is still an IPv4/UDP implementation, not added mDNS, IPv6, or TURN support.

## Validation

- Transport unit/regression tests passed with ASan/UBSan, including simulated
  NAT discovery with and without a lost initial STUN request.
- Six independent aiortc interoperability sessions passed: three in each DTLS
  role. Each deliberately dropped the first outgoing DTLS datagram, recovered,
  exchanged Opus and data-channel messages, and exercised the existing malformed
  traffic, fragmentation, and coalesced-record regressions.
- Both negative-fingerprint sessions passed, including repeated terminal-error
  polling. Descriptor checks and global teardown completed successfully.
- All six native core SDK tests passed.
- A 31-second parser fuzz smoke run completed 619,616 executions without a
  sanitizer finding. This is bounded regression evidence, not exhaustive testing.
- The Waveshare ESP-IDF firmware rebuilt successfully and fit its app partition.
  This pass did not flash the device or run another physical voice session.

## Limits and unresolved behavior

The new discovery retry remains deliberately short; prolonged loss or a slow
STUN response can still exhaust the one-second window. DNS resolution itself is
synchronous. More general asynchronous discovery and retry policy remain future
work.

The audio path still has no jitter buffer, packet-loss concealment, or acoustic
echo cancellation. Incoming RTP is delivered in arrival order; the Waveshare
adapter does not reorder late packets. Outbound timestamps advance by one 20 ms
frame per send, so microphone frames dropped before reaching the peer do not
preserve a capture-clock gap. These are remaining timing limitations, not claims
that the device's reported choppiness is explained or fixed.

The prior [device acceptance gaps](../../../docs/waveshare-validation.md) remain:
sustained audible calls, connected-session reconnects, active-call memory/CPU
measurements, and screen-mirror comparisons need a stable device network. Host
loopback recovery and a successful firmware build cannot substitute for them.
