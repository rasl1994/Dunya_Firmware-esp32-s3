#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path
from PIL import Image


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert(src: Path, dst: Path) -> None:
    im = Image.open(src).convert("RGBA")
    pixels = [(rgb565(r, g, b), a) for r, g, b, a in im.getdata()]
    runs = []
    i = 0
    while i < len(pixels):
        value = pixels[i]
        count = 1
        while i + count < len(pixels) and pixels[i + count] == value and count < 65535:
            count += 1
        runs.append((count, value[0], value[1]))
        i += count

    dst.parent.mkdir(parents=True, exist_ok=True)
    with dst.open("wb") as f:
        f.write(struct.pack("<4sHH", b"ARLE", im.width, im.height))
        for count, color, alpha in runs:
            f.write(struct.pack("<HHB", count, color, alpha))
    print(f"{src.name}: {im.width}x{im.height}, {len(runs)} runs -> {dst}")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("src", type=Path)
    p.add_argument("dst", type=Path)
    args = p.parse_args()
    convert(args.src, args.dst)


if __name__ == "__main__":
    main()
