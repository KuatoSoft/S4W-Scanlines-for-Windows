# Changelog

## v1.3

### ★ New feature

**Dynamic Bezel Reflection (MegaBezel style)**
An animated mirror reflection of the game image is projected into the bezel area around the
screen, like light reflecting on a real CRT. It layers on top of your own bezel PNG.

- For **16:9 games only** (4:3 support coming in a future version)
- Adjustable: **game size, opacity, blur, reflection size**
- **Roundable game corners + reflection corner** to match any CRT bezel

### ★ Improvements

- **Curvature geometry** — the screen curvature now bends equally on all four sides.
  Previously the top/bottom edges looked nearly flat while the left/right curved normally.
- **VHS & film grain clipped to the game area** — VHS effects and film grain no longer
  bleed into the black bars around the game image; they now apply only to actual game pixels.
- **Improved VHS visual effect.**

### ★ Compatibility

- **Direct3D 12 games** (e.g. *Mina the Hollower*) — added support for D3D12 titles.
- **MAME**
- **Gopher64** (N64, x86)
