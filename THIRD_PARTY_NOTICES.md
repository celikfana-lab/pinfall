# Third-party notices

PinFall itself is MIT-licensed (see [LICENSE](LICENSE)). It bundles or links
against the following third-party components, each under its own license.

## Bundled in this repository

| Component | Location | License |
|---|---|---|
| [cJSON](https://github.com/DaveGamble/cJSON) | `vendor/cJSON.{c,h}` | MIT |
| [stb_image](https://github.com/nothings/stb) | `vendor/stb_image.h` | Public domain / MIT (dual) |
| [LZ4](https://github.com/lz4/lz4) | `vendor/lz4/` | BSD 2-Clause |
| [DejaVu Sans](https://dejavu-fonts.github.io/) | `android/app/src/main/assets/fonts/DejaVuSans*.ttf` | Bitstream Vera license + public domain additions — see [`LICENSES/DejaVu.txt`](LICENSES/DejaVu.txt) |
| [Noto Color Emoji](https://github.com/googlefonts/noto-emoji) | `android/app/src/main/assets/fonts/NotoColorEmoji.ttf` | SIL Open Font License 1.1 — see [`LICENSES/NotoColorEmoji-OFL-1.1.txt`](LICENSES/NotoColorEmoji-OFL-1.1.txt) |

The full text of each license is kept under [`LICENSES/`](LICENSES/), and the
license header is also present at the top of each vendored source file.

## Linked at build time (desktop)

Installed via the system package manager, not vendored:

- **SDL2**, **SDL2_ttf** — zlib license
- **libcurl** — curl license (MIT-like)
- **libjpeg** (optional) — IJG license
- **ffmpeg** (runtime, `PATH`) — LGPL-2.1+ / GPL-2.0+ depending on the build

## Linked at build time (Android)

Fetched and built by [`android/deps.sh`](android/deps.sh); not vendored:

- **zlib** — zlib license
- **mbedTLS** — Apache-2.0
- **FreeType** — FreeType License (FTL) / GPL-2.0
- **libcurl** — curl license
- **SDL2**, **SDL2_ttf** — zlib license

## Trademarks

Pinterest is a trademark of Pinterest, Inc. This project is an independent,
unofficial client and is not affiliated with, endorsed by, or sponsored by
Pinterest.
