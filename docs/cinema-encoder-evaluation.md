# Cinema encoder evaluation

The deployed cinema encoder is a compact GloVe word-vector model. It is useful
for an inexpensive prototype, but exact vocabulary lookup and static word
averaging make natural-language retrieval brittle. The current retrieval path
also reranks candidates with word similarity and exact subject coverage.

A stronger encoder is worth evaluating before adding more topic-specific
aliases. Changing the numeric storage format alone does not address these
language limitations.

## Initial comparison

An offline comparison used all 1,940 descriptions in one prepared catalog and
23 short/conversational queries. These are diagnostic examples, not a labeled
accuracy benchmark or an ESP32 performance measurement. Only short-clip visual
descriptions were embedded; source-film titles and synopses were excluded.

| Encoder | Dimensions | Raw int8 weight bytes | Status |
| --- | ---: | ---: | --- |
| Current catalog's GloVe vocabulary | 50 | 1,078,850 | Deployed; semantic/keyword reranking |
| [POTION 2M](https://huggingface.co/minishlab/potion-base-2M/tree/389b9f64be5aa4ae7a6bc6fe95ef20ce485ae5da) | 64 | 1,889,792 | Offline candidate |
| [POTION 4M](https://huggingface.co/minishlab/potion-base-4M/tree/9b3cff412d30be9ae8603fe10224c224f3401869) | 128 | 3,779,584 | Most promising initial candidate |

Sizes exclude tokenizer strings/indexes, quantization scales, clip vectors and
application memory. POTION uses a 29,528-token WordPiece vocabulary and static
token-vector pooling. The models are distributed under MIT terms; no weights
or tokenizer data are committed here. See the [Model2Vec implementation](https://github.com/MinishLab/model2vec).

For this catalog, a sampled-frame description of one tabby kitten was the
relevant target. Its source film is titled *Three Little Kittens*; that title
was not used for embedding or as evidence of three visible animals.

| Query | Current retrieval | POTION 2M top result | POTION 4M top result |
| --- | --- | --- | --- |
| `cat` | Does not surface that kitten clip | Other footage | Kitten clip |
| `cats` | No confident match | Other footage | Kitten clip |
| `kitten` | Kitten clip | Kitten clip | Kitten clip |
| `kittens` | Unrecognized vocabulary | Kitten clip | Kitten clip |
| `can you find a cat video` | Other matches | Kitten clip | Kitten clip |

The 4M candidate also put an actual basketball game first for `sports`,
`basketball`, and `I was watching basketball last night`. However, its result
for `I really love cats` was less reliable, and `show me a cat, not a dog`
failed. Static pooling still does not understand word order or negation.

A second family, [Static Retrieval MRL](https://huggingface.co/sentence-transformers/static-retrieval-mrl-en-v1/tree/f60985c706f192d45d218078e49e5a8b6f15283a),
was tested at its supported 64/128-dimensional truncations. Its 128D version
found the kitten for `cat`, `kitten` and `kittens`, but ranked a wildlife montage
ahead for `cats` and missed the natural find request. The 64D version also
missed `cat`/`cats`. These initial cases favor testing POTION 4M first; they do
not establish an overall accuracy ranking. Static Retrieval MRL has 30,522
token rows, needs 3,906,816 raw int8 weight bytes at 128D, and is Apache-2.0
licensed. Neither it nor its tokenizer is included in the repository.

Each POTION candidate was evaluated with its original float32 weights, then with
per-row symmetric int8 weights (preserving a float scale) and int8 clip
vectors. Queries and clips used the same encoder and normalized mean pooling,
without special tokens or unknown-token vectors. Top-one results were unchanged
across all 23 queries for each candidate. This small test suggests int8 storage
is not the main source of the observed failures. Cosine scores from these models
are not directly comparable to the existing combined ranking score.

## Requirements before replacing the deployed encoder

- Evaluate a broader labeled set containing short words, plurals, ordinary
  conversational requests, unavailable subjects, misleading text cards and
  source-film title distractors. Include held-out queries and inspect actual
  frames, not only generated captions.
- Compare concise visual-subject descriptions with full captions. A film title,
  quoted intertitle, mood, or count of sampled frames must not become evidence
  that an object is visibly present.
- Calibrate confidence and randomness for the new score distribution. Reusing
  the current 0.72 threshold or selecting every nearest neighbor is incorrect.
- Match tokenizer/normalization and quantization on the computer and ESP32,
  with regression tests for unknown inputs, bounds and cancellation.
- Measure flash size, working memory and query latency during simultaneous
  voice/video playback. These laptop results do not establish audio safety.
- Rebuild the model and clip vectors together, preserving media IDs and checking
  pack identity. Existing movie files need not be transcoded solely because
  the text encoder changes.

The proposed runtime still performs query embedding and similarity search on
the device. Model preparation and evaluation happen on a computer; deployment
would not require a laptop embedding service.
