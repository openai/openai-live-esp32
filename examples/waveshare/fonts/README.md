# Caption font and fallback

The stock LVGL Montserrat font includes ASCII, degree/bullet symbols, and icons.
It does not include smart quotes or accented Latin letters. The Waveshare app
uses `live_caption_font_22` for captions and 22 px UI text, generated from the
same Montserrat Medium face at 22 px and 4 bits per pixel, without compression.

This subset includes ASCII, Latin-1 Supplement, Latin Extended-A, common
typographic punctuation, euro/trademark, arrows, and the mathematical minus.
Narrow no-break space uses the thin-space bitmap; nonbreaking hyphen uses the
hyphen bitmap. Both retain their original Unicode codepoints. The generated
28 px line height includes accent/descender bounds; additional line spacing
remains zero and turn spacing remains 4 px.

Before pagination, a display-only transcript copy goes through `CaptionText.hpp`:

1. Keep printable ASCII and supported Unicode glyphs.
2. For missing glyphs, simplify known punctuation, spaces, and Latin letters
   to readable ASCII. Drop unsupported combining accents/joiners/variation
   selectors; a decomposed `cafe` plus acute accent displays as `cafe`.
3. Replace other unsupported scalars or malformed bytes with `?`.

The SDK's original UTF-8 transcript is unchanged. Replacements never expand
the byte buffer, so block limits and streaming pagination cursors stay bounded.
This is not full Unicode coverage: CJK, other scripts, and emoji need additional
fonts and, where applicable, shaping support. No TTF parser runs on the device.

## Regeneration

The source font is from LVGL commit
[`c016f72d4c125098287be5e83c0f1abed4706ee5`](https://github.com/lvgl/lvgl/tree/c016f72d4c125098287be5e83c0f1abed4706ee5/scripts/built_in_font),
the 9.4.0 component used by this example. Font SHA-256:
`421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53`.
The font and derived bitmaps retain the [SIL Open Font License](LICENSE.OFL).

Install the pinned converter in a separate local tools directory:

```sh
npm install --prefix "$HOME/.cache/live-font-tools" --ignore-scripts \
  --no-audit --no-fund --save-exact lv_font_conv@1.5.3
python tools/generate_caption_font.py \
  --lvgl-dir /path/to/lvgl-9.4.0 \
  --converter "$HOME/.cache/live-font-tools/node_modules/.bin/lv_font_conv"
```

The generator verifies the font hash and converter version and emits no local
paths. Its output is checked in; normal firmware builds need no converter.
See the [LVGL font documentation](https://lvgl.io/docs/open/9.4/details/main-modules/font.html)
and [converter](https://github.com/lvgl/lv_font_conv).

`live glyphs-test` shows three local sample pages for ten seconds each, including
the smart-quote reproduction, accents, punctuation, and unsupported-character
fallback. It checks the selected font's real glyph descriptors for missing
glyphs and clipping, and does not send sample text to the API.
