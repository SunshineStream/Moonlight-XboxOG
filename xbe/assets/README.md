SDL_image does not accept "text" parameters in SVG images, due to a limitation of nanosvg.
We must convert images so they do not contain any text parameters.

UI icons come from [Lucide](https://lucide.dev/). CPM fetches the tagged release declared in
[the annotated package lock](../../package-lock.cmake), using the same convention as Sunshine and
LizardByte/build-deps so Renovate can update the dependency.
[The staging script](../../cmake/stage-lucide.cmake) copies only the required SVGs into the ISO's
`assets/icons/` directory and replaces `currentColor` with explicit shell colors for NanoSVG.
It also expands compact question-mark SVG arc flags for the older NanoSVG parser.
Downloaded sources remain in the build directory. The complete Lucide license, including Feather
attribution, is included at `assets/licenses/lucide.txt` on the ISO.

The grayscale host moons are first-party SVG artwork inspired by One's home page. Full, half and
crescent phases indicate online paired, online unpaired and offline PCs respectively. The orbit is
rendered separately so selection remains independent of reachability.

The Moonlight logo and controller button glyphs are separate bundled assets.
