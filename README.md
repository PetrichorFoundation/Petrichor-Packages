# Petrichor packages

Packages for [Petrichor OS](https://github.com/krspybacon/PetrichorOS). On a Petrichor machine:

    ppm update
    ppm install cowsay

`index` lists every package (name, version, size, SHA-256, dependencies); the `.ppk` files are the
packages. `ppm` verifies the size and SHA-256 before it installs anything. `packages/` holds the
sources. To rebuild everything, check out PetrichorOS next to this repository and run `make` here
(`make OS=/path/to/PetrichorOS` if it lives elsewhere); `make serve` serves the result on port 8080.

## The browser package

`packages/browser` is Petrichor's web browser (it is not part of the OS itself). Its own code renders pages,
runs JavaScript and handles the window; HTML is parsed by **hubbub**, NetSurf's HTML5 parser, with
**libparserutils** (both MIT-licensed, in `src/browser/netsurf/`, with a tiny C-library shim in `compat/`).
Styles come from NetSurf's CSS engine **libcss** (with **libwapcaplet**) working together with the browser's own
engine: `cssns.c` lets libcss compute each element's style and takes its answers property by property; what libcss
doesn't model (custom properties/var(), grid, gradients, rounded corners, gap) keeps the built-in engine's value,
and pages that use var() are styled by the built-in engine alone. The status bar says which ran.

Text is drawn with real TrueType fonts: the DejaVu Sans, Serif and Sans Mono families (regular, bold and italic,
in `files/share/browser/fonts/`, Bitstream Vera/DejaVu licence in `LICENSE-DejaVu`) rasterized by stb_truetype
(`src/browser/stb_truetype.h`, MIT/public domain) in `font.c`. CSS `font-family` picks sans, serif or monospace;
characters a face lacks come from DejaVu Sans. Not yet: Chinese/Japanese/Korean (DejaVu has no CJK glyphs),
joining and right-to-left ordering for Arabic and Hebrew, kerning, and web fonts (@font-face).
