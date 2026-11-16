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
The first-party left/right stick glyphs are also used by the stream wheels and on-screen keyboard.
The shell and stream controls share the same asset-path resolver and SVG rasterizer. Bundled paths
use Xbox backslashes throughout because the Xbox SDL file loader rejects forward slashes.

Keyboard helper keycaps are drawn with SDL primitives and the bundled UI font rather than SVG text.
Their measured widths adapt to labels such as Enter and Shift+PgUp, and crowded modal helpers wrap
onto additional rows. The active input device chooses between these keycaps and the controller glyphs.

Stream controls reuse the original colored controller button assets and SVG rasterizer. The stick and
Share symbols are local vector assets; Lucide supplies the other stream action symbols.
The Guide button uses the [Xbox symbol from Simple Icons 11.15.0](https://github.com/simple-icons/simple-icons/blob/11.15.0/icons/xbox.svg),
under [CC0](licenses/simple-icons.txt), with matching 24-unit dimensions and an explicit white fill for the older Xbox NanoSVG parser.
