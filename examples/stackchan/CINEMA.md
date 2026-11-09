# Memory cinema

> **Experimental example. Not officially supported by OpenAI.**

An optional StackChan mode replaces captions with silent archival film. Three
behaviors share one library:

| Mode | What drives the screen |
| --- | --- |
| Visual subconscious | Short conversation excerpts select related clips, with weighted randomness among close matches and a recent-clip/source penalty. |
| Time-travel camera | The Responses backend takes a fresh `capture_photo`, describes its visible subject to `set_visual_scene`, and queues an archival counterpart. |
| Idle dreams | Without API calls, the device follows precomputed neighbors with occasional exploration. Works offline. |

These are evocative associations, not factual evidence or a live camera feed.
The video player contains no audio path. Voice sessions still use the existing
Live SDK and start only when you press Start.

Tap a film to reveal Start/Stop. Controls hide after a short interval. The camera
photo still appears for two seconds. Stopping a session shows usage for ten
seconds before dreaming resumes; tap again to reveal usage and Start. USB
`cinema off` restores the normal caption interface.

## Prepare a pack

Use Python 3.10+, `ffmpeg`, and an OpenAI API key with access to the configured
caption and embedding models. Preparation makes paid API calls. Keep the key,
download cache, and generated pack outside the checkout.

```sh
python tools/prepare_stackchan_cinema.py \
  --output "$HOME/.cache/stackchan-cinema" \
  --key-file "$HOME/.config/openai-live-esp32/api-key" \
  --count 250
```

The preparer searches 25 visual themes in Cova Lee's
[Moving Image Archive](https://www.movingimagearchive.com/), limits repeated
source films, and makes 3–8 second, 320 × 240, 12 fps clips. It removes audio and
uses baseline JPEG frames in a small indexed `.SCF` container. Three actual
frames from each clip are described with `gpt-4.1-mini` (override with
`--caption-model`). Captions marked as mostly title cards, unreadable, or
unsuitable are skipped. This automated filter is not a human review of every
frame.

Descriptions and moods are embedded with `text-embedding-3-small`, at 256
dimensions. Per-vector int8 quantization keeps the device's vector table to
64,000 bytes for 250 clips. Norms preserve cosine similarity; neighbors are
computed from the original float vectors. The preparer caches downloads,
captions and embeddings, bounds response/file sizes, and slows archive requests
to respect rate limits. Re-running resumes from the cache.

The output `pack/` contains:

| File | Purpose |
| --- | --- |
| `CINEMA/*.SCF` | Silent movies for the SD card. |
| `CINEMA/INDEX.ID` | Pack identity, covering clip IDs, media hashes and the quantized index. Must match the firmware. |
| `catalog.json` | Captions, moods, source links, time windows, media hashes and provenance. |
| `embeddings.json` | Original float vectors and embedding model/dimensions. |
| `CinemaAssets.hpp` | Generated device index and neighbor graph. |
| `fallback.bin` | Six diverse flash-resident clips for trying the app without SD. |

The initial 250-clip sample spans **177 source films**, uses **124,116,423 bytes**
of movie files, and has a **3,619,060-byte** flash preview. The caption embeddings
used 21,739 input tokens; captioning and query embeddings are separate usage.
These measurements describe one prepared sample, not fixed outputs of future
archive searches. No films, generated vectors, API keys, or private firmware
images are checked into this repository.

## Preview and search on a laptop

```sh
python tools/stackchan_cinema.py "$HOME/.cache/stackchan-cinema/pack" \
  --key-file "$HOME/.config/openai-live-esp32/api-key"
```

Open <http://127.0.0.1:8878/>. The viewer plays the same `.SCF` files silently,
supports semantic search, shows source attribution, and can wander through
neighbors. It binds only to loopback. Without a key, playback and Dream still
work; search needs the embedding API. For a single query:

```sh
python tools/stackchan_cinema.py "$HOME/.cache/stackchan-cinema/pack" \
  --key-file "$HOME/.config/openai-live-esp32/api-key" \
  --query "waves crashing against rocks"
```

## Build and use on StackChan

First complete the [normal StackChan preparation](README.md#build-and-flash),
including the private key/Wi-Fi configuration and ESP-IDF environment. Then:

```sh
export STACKCHAN_CINEMA_PACK="$HOME/.cache/stackchan-cinema/pack"
idf.py -C examples/stackchan reconfigure build
idf.py -C examples/stackchan -p YOUR_PORT flash
```

To build the ordinary caption app again, unset `STACKCHAN_CINEMA_PACK` and run
`idf.py -C examples/stackchan reconfigure build`.

For the full library, copy the **whole `CINEMA` directory** to the root of a
FAT32 microSD card, insert it with power off, and restart. Start with a known
compatible 16 GB card; the pinned build does not enable exFAT. There is no need
for a large card for this sample. This app never formats or writes the card.
An absent card or mismatched `INDEX.ID` falls back to the six flash clips.
Individual missing/corrupt files are disabled for that boot. Rebuild the device
index and replace the card directory together when changing the pack.

CoreS3 shares GPIO35 between LCD D/C and SD MISO. The storage adapter locks the
display, drains pending LCD transfers, changes pin direction for small SD reads,
then restores it before unlocking. It initializes the shared bus with enough
transfer capacity for the display. **The SD path still needs on-device
validation with an installed card**, including read latency and concurrent
voice; flash-preview testing does not establish SD compatibility.

USB commands:

```text
cinema status
cinema cue clouds drifting across the sky
cinema off
cinema on
```

`cinema status` reports available clips, frames submitted/rendered, timing,
decode errors, embedding request failures, and CPU use between status calls.
All existing `live` commands remain available.

## Runtime behavior and limits

- The device embeds a bounded text cue using the same model and dimensions as
  its compiled index. It ranks locally and selects only available movies.
  Short transcript excerpts therefore make **additional OpenAI embedding API
  requests**, outside the Live/Responses token totals shown on the usage page.
  The archive is contacted only during offline preparation, never by firmware.
- Automatic cues are throttled and debounced. Newer requests supersede stale
  results; Stop/off invalidates queued work. Camera/tool cues get a brief hold
  against transcript updates. `set_visual_scene` only queues work, so the tool
  worker never waits on an embedding request. The player can cut to a new match
  after a minimum dwell rather than waiting for the entire old clip.
- Network failure leaves offline dreaming available. Embedding requests run in
  a low-priority worker with certificate and hostname verification enabled,
  bounded responses, and no redirects. The API key is never sent to the archive.
- Playback uses fixed PSRAM storage and two RGB565 frame buffers. The container
  checks resolution, frame count, offsets, JPEG sizes and CRC; the decoder's
  output dimensions and byte count are checked before decoding. Missed frames
  are skipped rather than queued. Decoding pauses behind camera previews and
  usage. The JPEG codec is the same Espressif component already used by the
  CoreS3 camera example.
- Idle TLS preconnection runs below the GUI's priority. Active Live sessions
  restore the original control priority; microphone and speaker priorities are
  unchanged. The other device examples retain their normal caption behavior.
- Flash previews and the laptop viewer have been exercised silently. SD playback,
  a complete spoken camera-to-archive exchange, and simultaneous voice/video
  audio quality remain hardware acceptance tests. Short pauses can occur at
  clip boundaries while loading the next movie.

## Provenance

The archive describes its collection as public-domain footage; see its
[about page](https://www.movingimagearchive.com/about). The preparer requires
each selected source's metadata to explicitly report `public_domain`, and
retains the original collection, creator, source URL and archive page in
`catalog.json`. Inspect that record and the original source before distributing
a pack. The repository's Apache-2.0 license covers project code, not an
independent guarantee about every downloaded film. The generated media is
kept outside the source release.
