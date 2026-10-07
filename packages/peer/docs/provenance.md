# Provenance and dependency policy

This portable package derives from the MIT-licensed `src` implementation and C
interface in [sepfy/libpeer](https://github.com/sepfy/libpeer), commit
`5b849de378545c31d34759a145413846953e1366`. Its original notice is retained in
[LICENSE.libpeer](../LICENSE.libpeer). Locally maintained changes use the
[MIT license](../LICENSE). The package is independently buildable and carries
its notices so it can be separated from this repository.

The source has been substantially changed: SDP/STUN/ICE parsing, DTLS identity
checks, crypto error handling, media dispatch, and SCTP/DCEP integration are
reviewable here. Unused HTTP/MQTT signaling, custom SCTP, mDNS, and video parsing
are excluded. This is not an assertion that unmodified upstream libpeer or any
Espressif release has the same fixes or security properties.

**No esp_peer source or binary is incorporated.** Espressif's
[Modified MIT license](https://github.com/espressif/esp-webrtc-solution/blob/main/components/esp_peer/LICENSE)
limits use to Espressif products, so it cannot be the source of a portable
implementation intended for other vendors. An ESP-IDF adapter, if added, must
remain separate from vendor-restricted implementation code.

CMake fetches complete release/source archives and verifies their SHA-256 hashes:

| Dependency | Pinned source | License |
| --- | --- | --- |
| Mbed TLS | [3.6.7 release archive](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7) | Apache-2.0 OR GPL-2.0-or-later; Apache-2.0 selected for this package |
| libSRTP | [2.8.1, `6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa`](https://github.com/cisco/libsrtp/tree/6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa) | BSD-3-Clause |
| usrsctp | [`c625e21950600460cb5c6075616eb1622df2b06a`](https://github.com/sctplab/usrsctp/tree/c625e21950600460cb5c6075616eb1622df2b06a) | BSD-3-Clause and notices in individual source files |

Copies of the top-level dependency licenses are in [licenses](../licenses).
Fetched source archives retain their additional per-file/subcomponent notices;
redistributors must preserve applicable notices for the actual linked build.
Mbed TLS includes third-party elliptic-curve implementations under their own
source-file notices. The package does not relicense those components.

Tests use aiortc 1.15.0 and its separately licensed Python dependencies, pinned in
`tests/interop/uv.lock`. Those are test tooling, not C runtime dependencies.
GitHub Actions are pinned to reviewed commit IDs. No binary transport library,
API key, Wi-Fi configuration, or device credentials are distributed here.

Dependency upgrades require checking upstream advisories and release notes,
updating archive hashes, and rerunning sanitizers, both DTLS roles, negative
fingerprint tests, malformed/replayed media tests, SCTP reassembly tests, and fuzz
smoke tests. An empty GitHub advisory list is not proof a dependency is secure.
