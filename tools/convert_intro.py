#!/usr/bin/env python3
"""Convert LOAD_INTRO_*.jpg/png frames to compact RGB565 RLE files.

Output format (little-endian):
  4 bytes  magic: b'IRLE'
  uint16   width
  uint16   height
  repeated records:
    uint16 run_length (1..65535)
    uint16 rgb565_color
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path
from PIL import Image

MAGIC = b"IRLE"


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def encode_frame(src: Path, dst: Path) -> tuple[int, int, int]:
    image = Image.open(src).convert("RGB")
    width, height = image.size
    pixels = [rgb565(r, g, b) for r, g, b in image.getdata()]

    payload = bytearray(MAGIC)
    payload += struct.pack("<HH", width, height)

    index = 0
    while index < len(pixels):
        color = pixels[index]
        run = 1
        while index + run < len(pixels) and pixels[index + run] == color and run < 65535:
            run += 1
        payload += struct.pack("<HH", run, color)
        index += run

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(payload)
    return width, height, len(payload)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--pattern", default="LOAD_INTRO_*")
    args = parser.parse_args()

    files = sorted(p for p in args.input_dir.glob(args.pattern) if p.suffix.lower() in {".jpg", ".jpeg", ".png"})
    if not files:
        raise SystemExit("No animation frames found")

    total = 0
    for number, src in enumerate(files):
        dst = args.output_dir / f"frame_{number:03d}.rle"
        width, height, size = encode_frame(src, dst)
        total += size
        print(f"{src.name} -> {dst.name}: {width}x{height}, {size} bytes")

    print(f"Converted {len(files)} frames, total {total} bytes")


if __name__ == "__main__":
    main()
