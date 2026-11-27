"""Generate the runtime and Android vector assets from the Sunshine logo."""

from pathlib import Path
from xml.etree import ElementTree


def android_paths(arcs):
    """Translate the SVG paths into Android VectorDrawable paths."""
    return "".join(
        f'        <path android:fillColor="{arc.attrib["fill"]}"\n'
        f'            android:pathData="{arc.attrib["d"]}" />\n'
        for arc in arcs
    )


def main():
    """Write the static logo, the four-tile arc atlas and Android artwork."""
    root = Path(__file__).resolve().parents[1]
    source = root / "sunshine-logo.svg"
    logo = ElementTree.parse(source).getroot()
    arcs = logo.findall("{*}path")
    size = int(logo.attrib["width"])
    assets = root / "xbe" / "assets"
    (assets / source.name).write_bytes(source.read_bytes())
    width = size * len(arcs)
    atlas = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{size}" '
        f'viewBox="0 0 {width} {size}">\n'
        + "".join(
            f'  <path fill="{arc.attrib["fill"]}" d="{arc.attrib["d"]}" '
            f'transform="translate({i * size} 0)"/>\n'
            for i, arc in enumerate(arcs)
        )
        + "</svg>\n"
    )
    (assets / "sunshine-logo-arcs.svg").write_text(atlas, encoding="utf-8", newline="\n")

    drawables = root / "packaging/android/app/src/main/res/drawable"
    namespace = 'xmlns:android="http://schemas.android.com/apk/res/android"'
    paths = android_paths(arcs)
    launcher = (
        f'<vector {namespace}\n'
        '    android:width="108dp" android:height="108dp"\n'
        '    android:viewportWidth="432" android:viewportHeight="432">\n'
        '    <group android:translateX="136" android:translateY="136" '
        'android:scaleX="0.625" android:scaleY="0.625">\n'
        f"{paths}    </group>\n</vector>\n"
    )
    banner = (
        f'<vector {namespace}\n'
        '    android:width="320dp" android:height="180dp"\n'
        '    android:viewportWidth="320" android:viewportHeight="180">\n'
        '    <path android:fillColor="@color/launcher_background" android:pathData="M0,0H320V180H0Z" />\n'
        '    <group android:translateX="96" android:translateY="26" '
        'android:scaleX="0.5" android:scaleY="0.5">\n'
        f"{paths}    </group>\n</vector>\n"
    )
    (drawables / "launcher_logo.xml").write_text(launcher, encoding="utf-8", newline="\n")
    (drawables / "tv_banner.xml").write_text(banner, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
