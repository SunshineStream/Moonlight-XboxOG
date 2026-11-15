SDL_image does not accept "text" parameters in SVG images, due to a limitation of nanosvg.
We must convert images so they do not contain any text parameters.

UI icons come from [Lucide Static](https://lucide.dev/guide/static). The Xbox build downloads a pinned,
SHA-256-verified package through [the Lucide dependency manifest](../../cmake/lucide.cmake).
[The staging script](../../cmake/stage-lucide.cmake) copies only the required SVGs into the ISO's
`assets/icons/` directory and replaces `currentColor` with explicit shell colors for NanoSVG.
The original downloaded SVGs stay in the build directory. The package's complete license, including
Feather attribution, is included at `assets/licenses/lucide.txt` on the ISO.

The Moonlight logo and controller button glyphs are separate bundled assets.
