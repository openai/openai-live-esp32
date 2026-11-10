# Provenance and dependency policy

This portable package derives from the MIT-licensed `src` implementation and C
interface in [sepfy/libpeer](https://github.com/sepfy/libpeer), commit
`5b849de378545c31d34759a145413846953e1366`. Its original notice is retained in
[LICENSE.libpeer](../LICENSE.libpeer). Locally maintained changes use the
[Apache License 2.0](../LICENSE), with attribution in [NOTICE](../NOTICE).
The package is independently buildable and carries its notices so it can be
separated from this repository. Upstream code retains its own licenses.

The source has been substantially changed: SDP/STUN/ICE parsing, DTLS identity
checks, crypto error handling, media dispatch, and SCTP/DCEP integration are
reviewable here. Unused HTTP/MQTT signaling, custom SCTP, mDNS, and video parsing
are excluded. This is not an assertion that unmodified upstream libpeer or any
Espressif release has the same fixes or security properties.

**No esp_peer source or binary is incorporated.** Espressif's
[Modified MIT license](https://github.com/espressif/esp-webrtc-solution/blob/main/components/esp_peer/LICENSE)
limits use to Espressif products, so it cannot be the source of a portable
implementation intended for other vendors. The separate ESP-IDF adapter builds
this portable source and does not link esp_peer.

CMake fetches complete release/source archives and verifies their SHA-256 hashes:

| Dependency | Pinned source | License |
| --- | --- | --- |
| Mbed TLS | [3.6.7 release archive](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7) | Apache-2.0 OR GPL-2.0-or-later; Apache-2.0 selected for this package |
| Mbed TLS / TF-PSA-Crypto (optional host path) | [4.1.1 release archive](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-4.1.1), SHA-256 `3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c` | Apache-2.0 selected; retain bundled subcomponent notices |
| libSRTP | [2.8.1, `6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa`](https://github.com/cisco/libsrtp/tree/6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa) | BSD-3-Clause |
| usrsctp | [`c625e21950600460cb5c6075616eb1622df2b06a`](https://github.com/sctplab/usrsctp/tree/c625e21950600460cb5c6075616eb1622df2b06a) | BSD-3-Clause and notices in individual source files |

The ESP-IDF preparation tool pins Espressif's Mbed TLS 3.6.7 fork at
[`2b96dd8e…`](https://github.com/espressif/mbedtls/tree/2b96dd8eebe880f304c69976b3c2fa0c5100cbb6)
and keeps the IDF 5.5.2 hardware/entropy and certificate-bundle integration in
an isolated component. The host build continues to use the upstream release.
ESP-IDF-only adaptations in `tools/prepare_peer.py` preserve dependency notices
and cover AF_CONN socket layout, default rwlock attributes, hardware randomness,
absent raw-IP headers, and Xtensa's distinct `uint32_t`/`unsigned int` types.
Both builds apply `patches/usrsctp-reinit.cmake` to clear the iterator's exit
flags before restarting it after a completed, joined global shutdown. Both also
apply `patches/mbedtls-empty-name.cmake`, guarding zero-length X.509 Name
comparisons before calling `memcmp`. The patch is idempotent, fails on source
drift, and adds a modification notice without removing upstream notices.
System-Mbed-TLS builds require an equivalent fix supplied by that installation.

The ESP32-S31 Mosaico example instead keeps IDF 6.2 preview's native Mbed TLS
4.1.1/TF-PSA-Crypto fork at
[`ce3f3485…`](https://github.com/espressif/mbedtls/tree/ce3f3485a121c100f58f36d700cb35b060f6e866).
It applies the same empty-name guard in the separate Mosaico IDF checkout.
The adapter uses PSA random generation, HMAC, and a short-lived P-256 signing
key whose policy follows Mbed TLS's deterministic-ECDSA configuration. The
key is destroyed at peer teardown. Both versions use the same negotiated
DTLS/SRTP restrictions and transport regression suite. Native IDF hardware,
entropy, certificate-bundle, and per-source licenses remain in place.

Both builds can select the pinned libSRTP tree's `aes_icm_mbedtls.c` and
`hmac_mbedtls.c` with `LIVE_PEER_MBEDTLS_SRTP=ON`. `src/srtp_mbedtls_aes.c` only provides the AES-192
type's forward declaration omitted by upstream headers when GCM is disabled;
the cipher implementation is included unchanged. GCM and extra negotiated
profiles stay disabled. The original software AES/HMAC sources remain available
as the default (`LIVE_PEER_MBEDTLS_SRTP=OFF`). Shared CMake configuration also selects
smaller usrsctp hash tables without changing protocol or message limits.

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
