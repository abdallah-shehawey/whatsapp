#!/usr/bin/env python3
"""Generate wa-lite's icon set from the two masters in data/icons.

The launcher and the tray deliberately do not share artwork: the launcher gets
the plated icon that suits an app grid, the tray gets the bare mark that still
reads at 16px against a panel.

The PNGs are committed so packaging needs no rendering step -- run this only when
a master changes, and never hand-edit the output.

Panels ask for small sizes, so the attention badge is sized as a fraction of the
icon rather than a fixed pixel count; at 16px a fixed badge would swallow the
whole glyph.
"""
import pathlib
import sys

import gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
import cairo

SIZES = (16, 22, 24, 32, 48, 64, 128, 256)
APP_ID = "io.github.shehawey.walite"

ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS = ROOT / "data" / "icons"
APP_MASTER = ICONS / "app-master.svg"
TRAY_MASTER = ICONS / "tray-master.png"


def render(master: pathlib.Path, size: int) -> GdkPixbuf.Pixbuf:
    """Rasterise at the target size rather than scaling a fixed bitmap, so an
    SVG master stays crisp at 16px."""
    return GdkPixbuf.Pixbuf.new_from_file_at_size(str(master), size, size)


def with_badge(pixbuf: GdkPixbuf.Pixbuf, size: int) -> GdkPixbuf.Pixbuf:
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    ctx = cairo.Context(surface)

    from gi.repository import Gdk
    Gdk.cairo_set_source_pixbuf(ctx, pixbuf, 0, 0)
    ctx.paint()

    radius = max(2.0, size * 0.22)
    cx = cy = size - radius - max(0.0, size * 0.02)

    if size >= 24:
        ctx.set_source_rgba(1, 1, 1, 1)
        ctx.arc(cx, cy, radius, 0, 6.28318)
        ctx.fill()
        radius *= 0.78

    ctx.set_source_rgba(1.0, 0.23, 0.19, 1.0)
    ctx.arc(cx, cy, radius, 0, 6.28318)
    ctx.fill()
    surface.flush()

    return Gdk.pixbuf_get_from_surface(surface, 0, 0, size, size)


def write(pixbuf: GdkPixbuf.Pixbuf, size: int, context: str, name: str) -> None:
    out = ICONS / str(size) / context / name
    out.parent.mkdir(parents=True, exist_ok=True)
    pixbuf.savev(str(out), "png", [], [])


def main() -> int:
    for master in (APP_MASTER, TRAY_MASTER):
        if not master.exists():
            print(f"missing {master}", file=sys.stderr)
            return 1

    for size in SIZES:
        write(render(APP_MASTER, size), size, "apps", f"{APP_ID}.png")

        tray = render(TRAY_MASTER, size)
        # Both contexts: SNI hosts disagree on which one they search.
        write(tray, size, "apps", f"{APP_ID}-tray.png")
        write(tray, size, "status", f"{APP_ID}-tray.png")
        write(with_badge(tray, size), size, "status", f"{APP_ID}-tray-attention.png")

    total = sum(1 for _ in ICONS.rglob("*.png")) - 1   # the tray master is one
    print(f"generated {total} icons across {len(SIZES)} sizes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
