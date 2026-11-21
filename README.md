# TextureLoader.asi (GTA Vice City PC v1.0)

**Swap textures at runtime using PNGs and a simple INI—no TXD editors, no rebuilding `.img` or `.txd` archives, and no permanent edits to your base game files.**

`TextureLoader.asi` is a lightweight 32-bit x86 plugin for **Grand Theft Auto: Vice City (PC v1.0)**. Instead of opening TXD tools or overwriting original game archives on disk, you drop in `.png` images, map them in `TextureLoader.ini`, and let the plugin apply replacements dynamically in memory while the game runs—across both loose `.txd` files and streamed `.img` / `modloader\` textures.

## Why Use This Instead of Editing `.img` / `.txd` Files?

- **Non-destructive runtime loading:** Your original `.img` and `.txd` files stay untouched on disk.
- **No manual TXD packing needed:** Point INI keys to `.png` files; the plugin handles power-of-two sizing, transparent edge cleanup, mipmaps, and native RenderWare formats (`DXT1`, `DXT3`, `DXT5`, `BGRA8888`, `ARGB1555`, `RGB565`).
- **Covers loose and streamed textures:** Redirects loose `.txd` loads (`%TEMP%` temp TXD) and swaps `RwRaster*` pointers live during `RwTexDictionaryAddTexture` for streamed/archive textures.

## Included Example: White Dot Crosshair

Ships with `example/textures/hud/siteM16.png` and `example/TextureLoader.ini` preconfigured to replace `siteM16` inside `hud.txd` with a clean white dot crosshair.

## Setup

1. Ensure a 32-bit x86 ASI loader is installed in your GTA Vice City PC v1.0 folder.
2. Put `TextureLoader.asi` and `TextureLoader.ini` in the same folder (game root, `scripts\`, or a `modloader\` subfolder) and place `textures\hud\siteM16.png` next to them so `Folder = textures\hud\` resolves cleanly.
3. Launch `gta-vc.exe` and check `TextureLoader.log` next to the `.asi` if you need to verify hook or replacement status.

## Configuration (`TextureLoader.ini`)

```ini
[Settings]
Enable = 1

[hud]
Enable = 1
Folder = textures\hud\
siteM16 = siteM16.png
```

## Building & Releases

### Local Build (`i686-w64-mingw32-g++`)

```bash
mkdir -p build
i686-w64-mingw32-g++ -std=c++17 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -Isrc -Ithird_party src/TextureLoader.cpp \
  -shared -static-libgcc -static-libstdc++ -Wl,--kill-at \
  -o build/TextureLoader.asi
```

### GitHub Actions Release

Pushing a `v*` tag (such as `v1.0.0`) runs `.github/workflows/build-asi.yml` to cross-compile the 32-bit `.asi` and publish a GitHub Release with `TextureLoader-VC.zip` (containing `TextureLoader.asi`, `ReadMe.txt`, `TextureLoader.ini`, and `textures/hud/siteM16.png`).

## Notes & Contributions

This project was vibecoded and may have issues or edge cases, though everything described above has been tested and works on my end. Bug reports, fixes, improvements, and all contributions are welcome!
