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
| Requested clip | The backend searches/inspects the local catalog and queues a specific clip. It plays once before automatic selection resumes. |

These are evocative associations, not factual evidence or a live camera feed.
Movies have **no audio track or audio playback path**. Voice sessions use the
existing Live SDK and start when you press Start. Tap a movie to reveal Start/Stop;
controls hide after a short interval. A camera photo still appears for two seconds.
Stopping a session shows usage briefly before dreaming resumes. `cinema off`
restores the caption interface.

The cinema personality is relaxed, warm and slightly dreamy: unhurried speech,
gentle pauses and occasional sensory imagery, with clear, direct answers for
practical questions. It keeps the `marin` voice. Each successful connection queues
one short welcome: "Hey. I'm here. Where shall we wander today?" The model may
paraphrase the wording, and speech remains interruptible. This is sent as frontend
spoken context, without a camera action or Responses call. Edit
[`CinemaPersonality.hpp`](main/CinemaPersonality.hpp) to adjust the frontend and
backend prompts or greeting. Other device examples keep their existing behavior.

## Talk about the video library

Try “Find a video of a boat and play it,” “What is in this clip?” or “What kinds
of films do you have?” The Responses backend calls tools on the ESP32:

| Tool | Behavior |
| --- | --- |
| `search_videos` | Local semantic/keyword search, returning the best eight matches in pages of three. An empty query browses the catalog. Returns clip IDs, source titles, short descriptions and description provenance. |
| `inspect_video` | Read a clip's sampled-frame description (`section: "clip"`) or its source-film synopsis and archive context (`section: "film"`). Use `clip_id: null` for a snapshot of the screen. Start at `offset: 0`; follow `next_offset` with the returned numeric clip ID so a changing screen cannot mix pages from different films. |
| `play_video` | Queue a returned clip ID for silent playback. Its first published frame changes request status from `queued` to `playing`. Transcript cues cannot replace it until the clip finishes; another explicit request can supersede it. |

Search and inspection are read-only; ordinary transcript-driven associations
continue independently. Results distinguish the last frame published to the UI,
whether the movie layer is visible, and the pending/playing/finished/failed request.
The screen can change after an inspection. The voice acknowledges a successful
queue request without polling or narrating a stale “not playing yet” status.
IDs belong to the returned pack digest;
do not reuse them after replacing a pack. Candidate availability is checked again
when loading: a missing/corrupt SD file fails rather than playing a guessed ID.
Queued requests time out after 15 seconds if no frame is published. Disabling
cinema or ending the session cancels explicit requests.

She describes the **stored catalog**, not a fresh analysis of the entire video.
`sampled_frames` describes three frames sampled during preparation;
`archive_metadata` is weaker title/search context and may not describe the visible
scene. `archive_catalog` is source-film context, including the archive synopsis,
year, creator and source links when available; it may describe scenes outside the
short clip. Film context is limited to 4 KiB, with long synopses explicitly marked
as excerpts and linked to the full source. Clip descriptions are retained and
paginated. Catalog text is untrusted
data, never instructions. These tools upload no video frames and require no laptop
service. Tool execution and search run below audio priority with bounded output
and cancellation checks. Verify speaker quality on your board and card: priority
alone cannot guarantee smooth playback, and upstream audio gaps can still cause
buffer underruns.

## What runs where

| Location | Contents and responsibility |
| --- | --- |
| ESP32 flash | Application, compact word-vector model, clip vectors, readable descriptions/provenance, neighbor graph, and a few fallback movies. Text embedding, ranking and randomized selection run here. |
| microSD | The full collection of silent `.SCF` movies and a pack identity file. |
| OpenAI API | Live voice, Responses reasoning, optional web search and camera interpretation. These still need Wi-Fi and an API key. |
| Preparation computer | Download/transcode media, optionally describe sample frames, generate the local index, build firmware and copy the pack. No laptop server is needed after setup. |

There are **no embedding API requests at runtime**, including for previously unseen
text cues. The local encoder uses a bounded vocabulary from Stanford GloVe 6B,
50-dimensional vectors, corpus weighting, normalization and int8 quantization.
Full visual captions supply document embeddings and 24 salient keywords.
The current index adds a broad sports association for named games such as
basketball and tennis, and avoids treating the verb in “sports a jacket” as
athletics. These indexing hints do not change the literal descriptions returned
by tools. Earlier v2 indexes remain readable by the laptop inspection tool.
A pooled embedding retrieves 64 candidates, combined with up to 32 exact-keyword
candidates. Weighted word similarity and exact subject coverage rerank the union.
The first selection for a new cue uses the best match; later selections add
randomness and repeat penalties within a narrow similarity range. A good
conversation match stays available for three minutes after the latest matching
cue. Weak transcript fragments or unknown words retain that topic without
extending its deadline or interrupting a pending clip. A strong new match replaces
it promptly. With no recent good match, playback returns to dreaming through the
neighbor graph. Ending the session or disabling cinema clears the topic.

Explicit camera/tool and USB cues retain their 45-second lifetime and 20-second
priority over transcript updates. An unsuccessful explicit request clears the old
topic instead of presenting it as the new request's result.

This small model handles concrete English subjects and actions better than long
or abstract requests. It does not model word order or negation, and is not a modern
sentence encoder. Unsupported words are ignored; they cannot create a new match.
Both the model and clip index must be regenerated together when the pack
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
python tools/sample_cinema_catalog.py \
  --output "$HOME/.cache/cinema-sample" --count 250
python tools/prepare_stackchan_cinema.py \
  --output "$HOME/.cache/stackchan-cinema" \
  --sample "$HOME/.cache/cinema-sample/sample.json" \
  --word-vectors "$HOME/.cache/glove.6B.zip" \
  --key-file "$HOME/.config/openai-live-esp32/api-key" \
  --fallback-count 2
```

The tool verifies the archive SHA-256
`6471382cdd837544bf3ac72497a38715e845897d265b2b424b4761832009c837`.
The sampler enumerates the full film catalog in Cova's
[Moving Image Archive](https://www.movingimagearchive.com/), reconciles every film's
clip count with the reported total, and draws unique global clip positions with
equal probability. Films with more clips occupy proportionally more positions;
there is no topic, duration, aspect ratio, color, or per-film-cap preference in
this raw draw. The browse clip feed is not used because its offset is capped.

`population.json` saves the catalog snapshot; `sample.json` saves the random seed,
positions and resolved clip IDs. Only the sampled films' clip lists need fetching.
Reruns reuse the saved seed and cached pages; increasing `--count` preserves the
earlier draw prefix. Use a new output directory for a fresh population snapshot.
`--seed` allows explicit reproduction and `--plan-only` records positions without
resolving media metadata. Incomplete pages, duplicate IDs, inconsistent counts,
or changed source/count totals fail instead of producing a partial sample.
The archive has no immutable snapshot API: the final catalog recheck detects
count changes, but cannot rule out same-count replacements during collection.

Preparation makes up to eight-second, 320 × 240, 12 fps excerpts containing baseline
JPEG frames. Audio is removed. Each clip is bounded to 2 MiB and 144 frames.
The raw sample remains intact. Provenance, download/format failures and caption
usability can exclude clips from the playable pack; every exclusion is recorded,
and **excluded draws are not replaced**. Consequently, the raw draw is uniform
over the catalog; the playable subset is conditional on those checks. Random
sampling reflects the archive's actual subject mix and does not balance topics.

By default, three sampled frames are described with `gpt-4.1-mini` (override with
`--caption-model`). **This preparation step makes paid API calls**. The model can
exclude unusable scenes; sampling is not a human review of every frame. Use
`--metadata-only` to avoid caption calls and derive coarse descriptions from archive
source titles (and search context in curated mode) instead. This is less precise
and is labeled in the catalog.
Embedding itself is always local. A later normal preparation run upgrades cached
coarse descriptions to visual captions.

For a larger random library, increase the sampler's `--count` and prepare that
complete draw in a new output directory. Optionally set preparation `--workers`
(1–8). Source-page requests are rate limited; downloads, captions, and selections
are cached. The older curated mode remains available by omitting `--sample` and
setting preparation `--count` and `--pages-per-theme` (up to 50). It searches visual
themes and limits repeated source films, so it is **not a uniform random sample**.
Use `--existing` to regenerate a selected library's model/index without network calls.
This also generates `CinemaMetadata.hpp` for library tools. Older packs must be
regenerated before building this example. Adding the readable catalog alone does
not change movie bytes or clip IDs; the firmware checks that both generated
headers have matching pack identity and clip count.

To add archive synopses to an older pack, run this **after** preparation finishes:

```bash
python tools/enrich_cinema_catalog.py --output "$HOME/.cache/stackchan-cinema"
```

It fetches missing source-film pages once per film and regenerates readable
metadata. It makes no OpenAI API calls and changes neither movie bytes nor the
embedding/index identity. Rebuild and flash afterward; the SD movies need no
recopy for a metadata-only update. New preparations retain the archive synopsis
automatically. Film context is separate from clip embeddings to avoid matching
an eight-second clip to a subject visible only elsewhere in its source film.

The generated `pack/` includes:

| File | Purpose |
| --- | --- |
| `CINEMA3/*/*.SCF` | Movies for the card, grouped into directories of at most 256 clips. |
| `CINEMA3/INDEX.ID` | Pack identity checked against firmware. |
| `catalog.json` | Captions, moods, source links, time windows, hashes and provenance. |
| `encoder.json`, `embeddings.json` | Local encoder and clip vectors for inspection/reproduction. |
| `CinemaAssets.hpp` | Generated model, index and neighbor graph compiled into firmware. |
| `CinemaMetadata.hpp` | Readable UTF-8 catalog compiled into flash, bound to the same pack identity. No SD reads are needed for descriptions. |
| `fallback.bin` | A small diverse set of flash-resident movies. |
| `raw-sample.json`, `sampling-population.json`, `selection-report.json` | Random-mode draw, population snapshot and explicit exclusions. |

An expanded 1,916-clip collection spans 734 source films and uses about 964 MB for
movies; its application image is about 5 MiB of the 12 MiB partition. The initial
250-clip sample spans 177 source films and uses about 124 MB for movies.
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
idf.py -C examples/stackchan_cinema reconfigure build
idf.py -C examples/stackchan_cinema -p YOUR_PORT flash
```

Use a FAT32 microSD card. The app never formats it; exFAT is not enabled. Insert it
with the device powered off. Copy the whole `pack/CINEMA3` directory to the card's
root using a card reader, or leave the card in the StackChan and transfer over USB:

```sh
python tools/load_stackchan_cinema.py --port YOUR_PORT \
  --pack "$STACKCHAN_CINEMA_PACK"
```

The USB loader stops the session and cinema, verifies the firmware/pack identity,
then writes numbered movie files with per-file CRC readback verification. Completed
files are verified and skipped on a resumed run. Explicit completed transfer
failures are retried at most twice; malformed acknowledgements stop the copy.
If every payload block was acknowledged but the final receipt is missing, the
loader first confirms that the device is back at its command parser and has the
same pack identity, then rechecks that file's CRC (also at most two retries).
Missing payload acknowledgements are not retried automatically. The pack identity is committed
last, once all expected files are present. Restart the device after completion to
initialize the complete playable set. A card reader is faster for large collections.
The loader only writes inside `CINEMA3` (`CINEMA` for older format-2 packs);
keep unrelated files outside those directories.
Do not remove the card or power while a write is active.

An absent card or mismatched identity uses the small flash fallback. Missing,
corrupt or oversized movies are disabled for that boot. Changing a library requires
matching firmware and card files; run `reconfigure` when changing the pack path so
CMake does not retain the previous include directory. Copying arbitrary MP4 files
is not supported.
NVS and factory servo calibration retain their existing offsets. The basic app is
still built independently with `idf.py -C examples/stackchan build`.

CoreS3 shares GPIO35 between LCD D/C and SD MISO. The storage adapter locks the
display, drains pending transfers, changes pin direction for small SD reads, then
restores it before unlocking. A 2 KiB internal DMA buffer enables multi-sector
reads; copying directly into PSRAM makes ESP-IDF fall back to individual sector
commands. The copy into PSRAM happens after releasing the display lock, with a
yield between chunks. FAT/directory caches use internal RAM and 512-byte SD
sectors. The LCD completion wait sleeps on the driver's transaction queue instead
of LVGL's default busy loop, leaving CPU time for audio and decoding.
Playback uses two compressed-movie PSRAM buffers
and two RGB565 buffers, validates frame bounds/CRC and decoded dimensions, and
skips missed frames. A background worker prefetches the next movie while the
current one plays; a slow read loops moving frames until the spare is ready.
The extra buffer costs at most 2 MiB of PSRAM. Video work and pending SD reads pause
before camera capture and stays hidden during its preview; a transient camera
timeout gets one bounded retry. Usage temporarily hides playback too. Background search runs at low priority with
periodic yields, separate from audio processing.

The prefetch worker runs on the other CPU core below microphone/speaker priority.
Busy display/SD access retries in place, preserving partial reads and checking
cancellation between attempts. Exhausting its bounded wait stays retryable; it
does not disable a valid movie. Actual file/CRC/decode failures still fail closed.

Format 3 groups movies into small directories under `CINEMA3`, avoiding long FAT
directory scans while holding the display lock. After regenerating an older pack,
flash its matching firmware and copy the new pack. The loader retains legacy flat
`CINEMA` files and their identity for rollback; they do not participate in the new
directory lookups. Legacy headers and the laptop tools still support flat packs.

Cinema requests a 240 ms PCM priming reserve to tolerate arrival bursts; other
examples retain the SDK's 120 ms default. This adds initial/rebuffer latency and
cannot repair missing upstream audio. Touch controls appear on touch-down, with
a 16 ms input polling period while cinema is enabled. `cinema status` reports
per-window maximum touch polling gaps, SD lock/open/read holds, display flush
submission time, and CPU samples
to distinguish card/display contention from audio/network issues.

## Diagnostics and optional laptop inspection

```text
cinema status
cinema cue ocean waves
cinema off
cinema on
cinema sd info
cinema trace on
cinema trace dump
cinema trace off
cinema trace clear
live quiet on
live quiet off
```

Run SD commands after `live stop` and `cinema off`.

`cinema status` reports available clips, rendered frames, timing, decode errors,
local-query failures, transition gaps, looped frames, remaining topic hold time
and CPU use between samples.
Tracing is off by default. `cinema trace on` keeps the latest 64 cue/ranking and
actual-playback records in RAM (including transcript excerpts and whether a weak
cue retained the previous topic); `dump` explicitly
prints them over USB. Nothing is written to the card or sent to a server. Use
`off` to stop recording and `clear` to erase records; reboot also clears them.
`live quiet on` zeroes microphone
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
