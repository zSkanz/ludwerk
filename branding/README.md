# Ludwerk branding - World Core

The current identity is an abstract spatial emblem and the Ludwerk wordmark,
created 2026-09-26 from an image-generated concept, rebuilt as four SVG polygons. Mint, graphite, deep teal and Inter connect it to the Orbit editor.

- [Identity board](../art/branding/orbit/brand-board.png)
- [Application icon review](../art/branding/orbit/icon-review.png)
- [Exact prompt and geometry](../art/branding/orbit/STYLE.md)
- [Brand metadata](brand.toml)

## Assets

| File | Purpose |
|---|---|
| `mark.svg`, `mark-512.png` | Transparent primary symbol |
| `mark-{light,dark,white,mono}.svg` | Surface-specific and single-ink vectors |
| `mark-{light,dark,white,mono}-512.png` | Matching transparent PNGs |
| `lockup-horizontal-{light,dark}.png` | 1400x360 name and symbol |
| `lockup-stacked-{light,dark}.png` | 800x800 stacked name and symbol |
| `lockup-horizontal.png`, `lockup-stacked.png` | Default light-surface lockups |
| `logo-512.png` | Wordmark on a transparent square |
| `app-icon-512.png` | Large graphite application tile |
| `icon/{16,24,32,48,64,128,256}.png` | Application icons at native sizes |
| `icon/icon.svg` | Editable application tile |
| `icon.ico`, `icon/icon.ico` | Identical seven-size PNG-only Windows icons |
| `social-card.png` | 1280x640 social artwork |

Use the tiled emblem for application icons and the transparent mark or full lockup
in editorial layouts. Keep the wordmark out of small application icons.

## Reproduction and transition

Run `python tools/repo/draw_branding.py`. Brand text comes from `brand.toml`;
Pillow and the vendored Inter font produce the exports deterministically.
The exact geometry and prompt are linked above.

Existing `engine-*` artwork paths and `icon/icon.ico` are compatibility copies
of the new assets. They keep the current resource, README and site consumers
working until the wider rename migrates their paths. `app.rc` still embeds
that compatibility ICO. Every embedded size is PNG, as required by the engine
resource decoder. Rebuild to update the executable; already installed packages
are not modified. This delivery is the artwork portion of ADR 0109, not the
full code, command, installation-folder or project-format migration.
