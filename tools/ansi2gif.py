#!/usr/bin/env python3
"""Turn a spinner's ANSI output into an animated GIF for a README.

Every spinner can print frames without a terminal:

    ./wizard-hat/hat3d --frames 96 --size 100x45 --fps 16 \
        | tools/ansi2gif.py --size 100x45 --fps 16 -o wizard-hat/wizard-hat.gif

Each frame is HEIGHT lines of text with 24-bit colour codes
(ESC[38;2;R;G;Bm) or 256-colour codes (ESC[38;5;Nm). Frames are drawn as SVG on a fixed character grid and
assembled by ImageMagick, so it needs `magick` with SVG support (librsvg).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

TOKEN = re.compile(r"\x1b\[38;2;(\d+);(\d+);(\d+)m|\x1b\[38;5;(\d+)m|\x1b\[[0-9;]*[A-Za-z]|(.)", re.S)
CELL_W, CELL_H, FONT_PX = 7, 14, 12.5
BACKGROUND = "#0b0d13"
FONTS = "DejaVu Sans Mono, Adwaita Mono, Menlo, Consolas, monospace"


BASIC16 = ["000000", "800000", "008000", "808000", "000080", "800080", "008080", "c0c0c0",
           "808080", "ff0000", "00ff00", "ffff00", "0000ff", "ff00ff", "00ffff", "ffffff"]


def xterm256(n):
    """The standard xterm colour for 256-colour code n."""
    if n < 16:
        return "#" + BASIC16[n]
    if n < 232:
        n -= 16
        levels = [0, 95, 135, 175, 215, 255]
        return "#%02x%02x%02x" % (levels[n // 36], levels[n // 6 % 6], levels[n % 6])
    grey = 8 + 10 * (n - 232)
    return "#%02x%02x%02x" % (grey, grey, grey)


def parse_frames(text, width, height):
    """Yield frames as lists of rows; each row is a list of (char, colour)."""
    lines = text.split("\r\n") if "\r\n" in text else text.split("\n")
    colour = "#cccccc"
    for start in range(0, len(lines) - height + 1, height):
        frame = []
        for line in lines[start:start + height]:
            row = []
            for m in TOKEN.finditer(line):
                if m.group(1):
                    colour = "#%02x%02x%02x" % tuple(int(m.group(k)) for k in (1, 2, 3))
                elif m.group(4):
                    colour = xterm256(int(m.group(4)))
                elif m.group(5) is not None:
                    row.append((m.group(5), colour))
            row += [(" ", None)] * (width - len(row))
            frame.append(row[:width])
        yield frame


def escape(ch):
    return {"&": "&amp;", "<": "&lt;", ">": "&gt;"}.get(ch, ch)


def frame_to_svg(frame, width, height):
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width * CELL_W}" '
        f'height="{height * CELL_H}">',
        f'<rect width="100%" height="100%" fill="{BACKGROUND}"/>',
        f'<g font-family="{FONTS}" font-size="{FONT_PX}" font-weight="bold" xml:space="preserve">',
    ]
    for y, row in enumerate(frame):
        x = 0
        while x < width:
            ch, colour = row[x]
            if ch == " ":
                x += 1
                continue
            run_start, chars = x, []
            while x < width and row[x][0] != " " and row[x][1] == colour:
                chars.append(row[x][0])
                x += 1
            # one x position per glyph keeps the grid exact whatever the font
            xs = " ".join(str((run_start + i) * CELL_W) for i in range(len(chars)))
            baseline = y * CELL_H + CELL_H * 0.78
            out.append(f'<text x="{xs}" y="{baseline:.1f}" fill="{colour}">'
                       f'{"".join(escape(c) for c in chars)}</text>')
    out.append("</g></svg>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--size", required=True, help="WIDTHxHEIGHT the spinner rendered at")
    ap.add_argument("--fps", type=float, default=16)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    width, height = (int(v) for v in args.size.lower().split("x"))

    text = sys.stdin.buffer.read().decode("utf-8", "replace")
    frames = list(parse_frames(text, width, height))
    if not frames:
        sys.exit("no frames on stdin")

    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for i, frame in enumerate(frames):
            path = os.path.join(tmp, f"frame{i:04d}.svg")
            with open(path, "w") as f:
                f.write(frame_to_svg(frame, width, height))
            paths.append(path)
        delay = max(2, round(100 / args.fps))       # GIF delays are in 1/100 s
        subprocess.run(["magick", "-delay", str(delay), "-loop", "0", *paths,
                        "-layers", "Optimize", args.output], check=True)
    print(f"{args.output}: {len(frames)} frames", file=sys.stderr)


if __name__ == "__main__":
    main()
