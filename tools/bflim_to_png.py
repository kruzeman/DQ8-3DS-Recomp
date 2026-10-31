#!/usr/bin/env python3
"""Decode the 3DS BFLIM formats used by DQ8's title UI into PNG."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image


MODIFIERS = (
    (2, 8, -2, -8),
    (5, 17, -5, -17),
    (9, 29, -9, -29),
    (13, 42, -13, -42),
    (18, 60, -18, -60),
    (24, 80, -24, -80),
    (33, 106, -33, -106),
    (47, 183, -47, -183),
)


def sign3(value: int) -> int:
    return value - 8 if value & 4 else value


def expand4(value: int) -> int:
    return (value << 4) | value


def expand5(value: int) -> int:
    return (value << 3) | (value >> 2)


def decode_etc1_block(block: bytes) -> list[tuple[int, int, int]]:
    # PICA stores ETC words little-endian even though the ETC bit diagram is
    # conventionally written most-significant bit first.
    bits = int.from_bytes(block, "little")
    high, low = bits >> 32, bits & 0xFFFFFFFF
    differential = (high >> 1) & 1
    flip = high & 1
    if differential:
        base = [
            expand5((high >> 27) & 31),
            expand5((high >> 19) & 31),
            expand5((high >> 11) & 31),
        ]
        delta = [
            sign3((high >> 24) & 7),
            sign3((high >> 16) & 7),
            sign3((high >> 8) & 7),
        ]
        colors = (base, [expand5((((high >> shift) & 31) + d) & 31) for shift, d in zip((27, 19, 11), delta)])
    else:
        colors = (
            [expand4((high >> shift) & 15) for shift in (28, 20, 12)],
            [expand4((high >> shift) & 15) for shift in (24, 16, 8)],
        )
    table_ids = ((high >> 5) & 7, (high >> 2) & 7)
    out = []
    for y in range(4):
        for x in range(4):
            sub = (y >= 2) if flip else (x >= 2)
            index = x * 4 + y
            selector = ((low >> (index + 16)) & 1) * 2 + ((low >> index) & 1)
            adjust = MODIFIERS[table_ids[int(sub)]][selector]
            out.append(tuple(max(0, min(255, c + adjust)) for c in colors[int(sub)]))
    return out


def decode_bflim(path: Path) -> Image.Image:
    data = path.read_bytes()
    footer = len(data) - 40
    if footer < 0 or data[footer : footer + 4] != b"FLIM" or data[footer + 20 : footer + 24] != b"imag":
        raise ValueError("not a supported BFLIM file")
    endian = "<" if data[footer + 4 : footer + 6] == b"\xff\xfe" else ">"
    width, height, alignment = struct.unpack_from(endian + "HHH", data, footer + 28)
    fmt, tile_mode = data[footer + 34], data[footer + 35]
    raw_size = struct.unpack_from(endian + "I", data, footer + 36)[0]
    pixels = data[:raw_size]
    if not width or not height or raw_size > len(data) - 40:
        raise ValueError("invalid BFLIM dimensions or image data size")

    rgba = bytearray(width * height * 4)
    if fmt in (10, 11):  # ETC1 / ETC1A4, in 2x2 4x4-block tiles.
        bpp = 8 if fmt == 10 else 16
        blocks_x, blocks_y = (width + 3) // 4, (height + 3) // 4
        tiles_x, tiles_y = (blocks_x + 1) // 2, (blocks_y + 1) // 2
        needed = tiles_x * tiles_y * 4 * bpp
        if needed > len(pixels):
            raise ValueError(f"ETC data is truncated: need {needed}, have {len(pixels)}")
        for ty in range(tiles_y):
            for tx in range(tiles_x):
                for by in range(2):
                    for bx in range(2):
                        block_x, block_y = tx * 2 + bx, ty * 2 + by
                        if block_x >= blocks_x or block_y >= blocks_y:
                            continue
                        block_index = ((ty * tiles_x + tx) * 4) + by * 2 + bx
                        start = block_index * bpp
                        if fmt == 11:
                            alpha = pixels[start : start + 8]
                            colors = decode_etc1_block(pixels[start + 8 : start + 16])
                        else:
                            alpha = b"\xff" * 8
                            colors = decode_etc1_block(pixels[start : start + 8])
                        for py in range(4):
                            for px in range(4):
                                x, y = block_x * 4 + px, block_y * 4 + py
                                if x >= width or y >= height:
                                    continue
                                src = py * 4 + px
                                alpha_index = px * 4 + py
                                a_nibble = (alpha[alpha_index // 2] >> (4 * (alpha_index % 2))) & 15
                                r, g, b = colors[src]
                                dst = (y * width + x) * 4
                                rgba[dst : dst + 4] = bytes((r, g, b, a_nibble * 17))
    elif fmt == 5:  # RGB565
        expected = width * height * 2
        if len(pixels) < expected:
            raise ValueError("RGB565 data is truncated")
        for i in range(width * height):
            value = int.from_bytes(pixels[i * 2 : i * 2 + 2], "little")
            rgba[i * 4 : i * 4 + 4] = bytes((expand5((value >> 11) & 31), ((value >> 5) & 63) * 255 // 63, expand5(value & 31), 255))
    elif fmt in (0, 1):  # L8 / A8
        if len(pixels) < width * height:
            raise ValueError("8-bit image data is truncated")
        for i, value in enumerate(pixels[: width * height]):
            rgba[i * 4 : i * 4 + 4] = bytes((255, 255, 255, value)) if fmt == 1 else bytes((value, value, value, 255))
    else:
        raise ValueError(f"unsupported BFLIM format {fmt} (alignment {alignment}, tile mode {tile_mode})")

    return Image.frombytes("RGBA", (width, height), bytes(rgba))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    image = decode_bflim(args.input)
    image.save(args.output)
    print(f"Wrote {image.width}x{image.height} {args.output}")


if __name__ == "__main__":
    main()
