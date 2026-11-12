# StackChan memory cinema

> **Experimental example. Not officially supported by OpenAI.**

This is a separate firmware example from the [basic StackChan voice app](../stackchan/README.md).
It shares the SDK, camera, audio, display and motion drivers, and replaces captions
with silent archival film. The basic app does not build the cinema engine or index.

| Behavior | What drives the screen |
| --- | --- |
| Visual subconscious | Short conversation excerpts select related clips, with weighted randomness among close matches and a penalty for recent clips and source films. |
| Time-travel camera | The Responses backend takes a fresh `capture_photo`, describes its visible subject to `set_visual_scene`, and queues an archival counterpart. |
| Idle dreams | The device follows precomputed neighbors with occasional exploration, entirely offline. |

These are evocative associations, not factual evidence or a live camera feed.
Movies have **no audio track or audio playback path**. Voice sessions use the
existing Live SDK and start when you press Start. Tap a movie to reveal Start/Stop;
controls hide after a short interval. A camera photo still appears for two seconds.
Stopping a session shows usage briefly before dreaming resumes. `cinema off`
restores the caption interface.

## What runs where

| Location | Contents and responsibility |
| --- | --- |
| ESP32 flash | Application, compact word-vector model, clip vectors, neighbor graph, and a few fallback movies. Text embedding, ranking and randomized selection run here. |
| microSD | The full collection of silent `.SCF` movies and a pack identity file. |
| OpenAI API | Live voice, Responses reasoning, optional web search and camera interpretation. These still need Wi-Fi and an API key. |
| Preparation computer | Download/transcode media, optionally describe sample frames, generate the local index, build firmware and copy the pack. No laptop server is needed after setup. |

There are **no embedding API requests at runtime**, including for previously unseen
text cues. The local encoder uses a bounded vocabulary from Stanford GloVe 6B,
50-dimensional vectors, corpus weighting, normalization and int8 quantization.
A pooled embedding retrieves 64 candidates; word-level similarity reranks those,
and weighted selection chooses among the closest available matches. Recent clip
and source penalties add variety. Idle dreams use the neighbor graph.

This small model handles concrete English subjects and actions better than long
or abstract requests. It does not model word order or negation, and is not a modern
sentence encoder. Unsupported words are ignored; an all-unknown cue leaves dreaming
available. Both the model and clip index must be regenerated together when the pack
changes. The same encoder is used by the optional laptop inspection tool.

## Prepare a pack

Use Python 3.11+, `ffmpeg`/`ffprobe`, and the pinned pretrained archive. Keep generated
assets, keys, and firmware outside version control:

```sh
python3 -m venv "$HOME/.cache/cinema-tools"
"$HOME/.cache/cinema-tools/bin/pip" install -r tools/cinema-requirements.txt
export PATH="$HOME/.cache/cinema-tools/bin:$PATH"
curl -fL --retry 3 \
  https://huggingface.co/stanfordnlp/glove/resolve/main/glove.6B.zip \
  -o "$HOME/.cache/glove.6B.zip"
python tools/prepare_stackchan_cinema.py \
  --output "$HOME/.cache/stackchan-cinema" \
  --word-vectors "$HOME/.cache/glove.6B.zip" \
  --key-file "$HOME/.config/openai-live-esp32/api-key" \
  --count 250 --fallback-count 2
```

The tool verifies the archive SHA-256
`6471382cdd837544bf3ac72497a38715e845897d265b2b424b4761832009c837`.
It searches visual themes in Cova Lee's [Moving Image Archive](https://www.movingimagearchive.com/),
limits repeated source films, and makes short 320 × 240, 12 fps clips containing
baseline JPEG frames. Audio is removed. Each clip is bounded to 2 MiB and 144 frames.

By default, three sampled frames are described with `gpt-4.1-mini` (override with
`--caption-model`). **This preparation step makes paid API calls**. The model can
exclude unusable scenes; sampling is not a human review of every frame. Use
`--metadata-only` to avoid caption calls and derive coarse descriptions from archive
search context instead. This is less precise and is labeled in the catalog.
Embedding itself is always local. A later normal preparation run upgrades cached
coarse descriptions to visual captions.

For a larger library, increase `--count` and `--pages-per-theme` (up to 50), and
optionally `--workers` (1–8). Search and source-page requests are rate limited;
downloads, captions, and selected clips are cached. `--count` is an upper bound:
if the search has fewer eligible clips, the tool reports and exports the actual count.
Use `--existing` to regenerate a selected library's model/index without network calls.

The generated `pack/` includes:

| File | Purpose |
| --- | --- |
| `CINEMA/*.SCF` | Movies for the card. |
| `CINEMA/INDEX.ID` | Pack identity checked against firmware. |
| `catalog.json` | Captions, moods, source links, time windows, hashes and provenance. |
| `encoder.json`, `embeddings.json` | Local encoder and clip vectors for inspection/reproduction. |
| `CinemaAssets.hpp` | Generated model, index and neighbor graph compiled into firmware. |
| `fallback.bin` | A small diverse set of flash-resident movies. |

The initial 250-clip sample spans 177 source films and uses about 124 MB for movies.
At that average size, a 16 GB card has room for roughly 30,000 clips; actual capacity
depends on clip lengths, compression, filesystem overhead and other files. The
current index is limited to 30,000 clips and firmware must fit its 12 MiB app
partition. Preparation and transfer time are practical limits too. Generated media,
model weights, and indexes are not included in this source repository.

## Build, flash and load the card

First follow [StackChan preparation](../stackchan/README.md#build-and-flash) for
private key/Wi-Fi configuration and ESP-IDF. Use this separate example directory:

```sh
export STACKCHAN_CINEMA_PACK="$HOME/.cache/stackchan-cinema/pack"
idf.py -C examples/stackchan_cinema build
idf.py -C examples/stackchan_cinema -p YOUR_PORT flash
```

Use a FAT32 microSD card. The app never formats it; exFAT is not enabled. Insert it
with the device powered off. Copy the whole `pack/CINEMA` directory to the card's
root using a card reader, or leave the card in the StackChan and transfer over USB:

```sh
python tools/load_stackchan_cinema.py --port YOUR_PORT \
  --pack "$STACKCHAN_CINEMA_PACK"
```

The USB loader stops the session and cinema, verifies the firmware/pack identity,
then writes numbered movie files with per-file CRC readback verification. Completed
files are verified and skipped on a resumed run. The pack identity is committed
last, once all expected files are present. Restart the device after completion to
initialize the complete playable set. A card reader is faster for large collections.
The loader only writes inside `CINEMA`; keep unrelated files outside that directory.
Do not remove the card or power while a write is active.

An absent card or mismatched identity uses the small flash fallback. Missing,
corrupt or oversized movies are disabled for that boot. Changing a library requires
matching firmware and card files; copying arbitrary MP4 files is not supported.
NVS and factory servo calibration retain their existing offsets. The basic app is
still built independently with `idf.py -C examples/stackchan build`.

CoreS3 shares GPIO35 between LCD D/C and SD MISO. The storage adapter locks the
display, drains pending transfers, changes pin direction for small SD reads, then
restores it before unlocking. Playback uses fixed PSRAM storage and two RGB565
buffers, validates frame bounds/CRC and decoded dimensions, and skips missed frames.
Loading a new movie can cause a short pause at clip boundaries. Video work pauses
before camera capture and stays hidden during its preview; a transient camera
timeout gets one bounded retry. Usage temporarily hides playback too. Background search runs at low priority with
periodic yields, separate from audio processing.

## Diagnostics and optional laptop inspection

```text
cinema status
cinema cue ocean waves
cinema off
cinema on
cinema sd info
live quiet on
live quiet off
```

Run SD commands after `live stop` and `cinema off`.

`cinema status` reports available clips, rendered frames, timing, decode errors,
local-query failures and CPU use between samples. `live quiet on` zeroes microphone
and speaker PCM for unattended tests while retaining the audio pipeline. It is
off by default and resets on reboot. Existing `live` diagnostics remain available.

The optional viewer is an inspection aid, not a runtime dependency:

```sh
python tools/stackchan_cinema.py "$STACKCHAN_CINEMA_PACK" --query "ocean waves"
python tools/stackchan_cinema.py "$STACKCHAN_CINEMA_PACK"
```

The latter binds to <http://127.0.0.1:8878/> and provides silent playback, local
search and source attribution. Neither command needs an API key.

## Provenance

[Third-party notices](../../THIRD_PARTY_NOTICES.md#optional-stackchan-cinema-assets)
identify the GloVe PDDL data license, archive sources and host-tool dependencies.
The preparer requires each film source to explicitly report `public_domain` and
retains its provenance. Inspect that record and original source before distributing
a pack or firmware. The repository's Apache-2.0 license covers project code, not an
independent guarantee about every downloaded movie or model asset.
