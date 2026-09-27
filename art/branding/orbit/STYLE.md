# Ludwerk - World Core

Active identity, 2026-09-26, following the owner's brand selection in
[ADR 0109](../../../docs/decisions/0109-the-engine-has-an-internal-name-that-never-changes-and-its-brand-is-ludwerk.md).
This replaces the letter-based Workshop L. Orbit remains the editor glyph
family. The generated concept and previous L review are retained locally.

## Concept and exact prompt

[Exact image-generation prompt and production translation](EMBLEM-PROMPT.md).
The built-in image_gen tool produced [this raster concept](world-core-concept.png).
The production symbol is a manually reconstructed vector: two rising side
pieces, a platform and a central diamond, suggesting a world under construction.
It uses four filled polygons rather than a single letter.

The generated image included glow despite the flat-art constraint. Production
removes it entirely: one solid color, transparent gaps, no gradients or effects.
SVG contains actual polygon geometry, never an embedded image. Pillow renders
those same coordinates for PNG and ICO. Inter is the existing OFL-licensed font.

## Production geometry and palette

Canvas: 64 by 64. Four separate polygons, in order:

1. `(30,6) (8,18) (8,43) (20,36) (20,27) (30,21)`
2. `(34,6) (56,18) (56,43) (44,36) (44,27) (34,21)`
3. `(22,41) (32,46) (42,41) (53,47) (32,59) (11,47)`
4. `(32,32) (40,36.5) (32,41) (24,36.5)`

Keep the open channels between pieces. Do not outline or independently color
them. The application tile is graphite `#101C24`, bounds `(1,1)-(63,63)`,
radius 14, with mint `#55E0C5` geometry and transparent outer corners.
Transparent variants: primary `#17BDA6`, light-surface teal `#07867C`,
dark-surface mint, white and ink `#14252D`. Wordmark: Ludwerk, Inter 650,
optical size 32, -2.5% tracking; dark-surface text `#F4F8F9`.

## Source and reproduction

```sh
python tools/repo/draw_branding.py
python tools/repo/draw_icons.py
```

Brand text comes from [brand.toml](../../../branding/brand.toml). The generator
uses neutral output paths in `branding/`: `mark.svg`, `lockup-*.png`,
`app-icon-512.png`, `social-card.png`, and `icon.ico`. Icon PNGs are in
`icon/{16,24,32,48,64,128,256}.png`, with the tiled vector at `icon/icon.svg`.
The editor header mirrors `mark_polygons()` in `drawBrandMark`; update both
when geometry changes. Editor glyph designs retain the Orbit style; their
preview headings read the brand file too.

Every ICO entry contains PNG data, including 16px. The existing embedded
resource decoder requires this. The generator also writes compatibility
copies under the former brand's filenames, so current resource, website and
packaging consumers receive the new artwork before the wider rename lands.
These filenames are aliases, not a second visual identity.

This artwork delivery does not complete the internal namespace/CLI/project
migration in the rename ledger. Installed binaries are unchanged until rebuilt
or reinstalled. The other stages remain tracked in that ledger.

## Review

- [Image-generated concept](world-core-concept.png)
- [Previous letter-based identity](workshop-l-board.png)
- [Identity board](brand-board.png)
- [Native application sizes](icon-review.png)
- [Gallery](index.html)
