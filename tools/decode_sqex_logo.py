#!/usr/bin/env python3
"""Decode and join DQ8's two 3DS Square Enix logo textures.

This intentionally supports the supplied v7.2.0 RGB565 / orientation-4 files
only. It is independent of the project's experimental general BFLIM decoder.
Requires Pillow. See ../sqex-logo-decoding.md for the format and address formula.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from PIL import Image


def morton8(x: int, y: int) -> int:
    """Interleave the three low bits: x0,y0,x1,y1,x2,y2."""
    return sum(
        (((x >> bit) & 1) << (2 * bit))
        | (((y >> bit) & 1) << (2 * bit + 1))
        for bit in range(3)
    )


def decode_logo(path: Path) -> tuple[Image.Image, dict]:
    data = path.read_bytes()
    footer = len(data) - 40
    if footer < 0 or data[footer:footer + 4] != b"FLIM":
        raise ValueError(f"{path}: missing 40-byte BFLIM footer")
    if data[footer + 4:footer + 6] != b"\xff\xfe":
        raise ValueError(f"{path}: expected little endian")
    if data[footer + 20:footer + 24] != b"imag":
        raise ValueError(f"{path}: missing imag section")
    version, file_size = struct.unpack_from("<II", data, footer + 8)
    width, height, alignment, fmt, orientation, raw_size = struct.unpack_from(
        "<HHHBBI", data, footer + 28
    )
    if (version, fmt, orientation) != (0x07020000, 5, 4):
        raise ValueError(f"{path}: expected v7.2.0, RGB565, orientation 4")
    if not width or not height or width % 8 or height % 8:
        raise ValueError(f"{path}: expected positive dimensions divisible by 8")
    if file_size != len(data) or raw_size != footer or raw_size != width * height * 2:
        raise ValueError(f"{path}: inconsistent file or pixel-data length")

    # The v7.2.0 footer gives the displayed dimensions. The physical tiled
    # surface has the axes exchanged, before its 90-degree CCW rotation.
    stored_width, stored_height = height, width
    stored = Image.new("RGB", (stored_width, stored_height))
    # Repack the decoded color values independently below. Equality checks
    # both the coordinates used by the rotation and all 16 bits of each color.
    for sy in range(stored_height):
        for sx in range(stored_width):
            tile = (sy // 8) * (stored_width // 8) + sx // 8
            offset = 2 * (64 * tile + morton8(sx, sy))
            value = struct.unpack_from("<H", data, offset)[0]
            r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
            stored.putpixel((sx, sy), (
                (r << 3) | (r >> 2),
                (g << 2) | (g >> 4),
                (b << 3) | (b >> 2),
            ))
    image = stored.transpose(Image.Transpose.ROTATE_90)

    rebuilt = bytearray(raw_size)
    # Iterate tiles and consume their Morton positions in sequence, instead
    # of reusing the address calculation from the decoding loop.
    reverse_morton = sorted(
        ((x, y) for y in range(8) for x in range(8)),
        key=lambda xy: morton8(*xy),
    )
    cursor = 0
    for tile_y in range(stored_height // 8):
        for tile_x in range(stored_width // 8):
            for local_x, local_y in reverse_morton:
                sx, sy = tile_x * 8 + local_x, tile_y * 8 + local_y
                r, g, b = image.getpixel((sy, stored_width - 1 - sx))
                value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                struct.pack_into("<H", rebuilt, cursor, value)
                cursor += 2
    if rebuilt != data[:raw_size]:
        raise ValueError(f"{path}: decoding/repacking did not preserve pixel bytes")
    return image, {
        "file": path.name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "footer_offset": footer,
        "version": hex(version),
        "display_size": [width, height],
        "stored_size": [stored_width, stored_height],
        "alignment": alignment,
        "format": fmt,
        "orientation": orientation,
        "pixel_bytes": raw_size,
        "roundtrip_exact": True,
    }


def verify_layout(path: Path, images: list[Image.Image]) -> list[dict]:
    """Check the actual logo panes, their anchors and full-range UVs."""
    data = path.read_bytes()
    if data[:6] != b"FLYT\xff\xfe":
        raise ValueError(f"{path}: expected little-endian BFLYT")
    panes = {}
    offset = struct.unpack_from("<H", data, 6)[0]
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError(f"{path}: invalid section length")
        if tag == b"pic1":
            name = data[offset + 12:offset + 36].split(b"\0")[0].decode("ascii")
            if name in ("sqex_logo_000", "sqex_logo_001"):
                origin = data[offset + 9]
                tx, ty, tz, rx, ry, rz, sx, sy, w, h = struct.unpack_from("<10f", data, offset + 44)
                uv_count = data[offset + 102]
                uv = struct.unpack_from("<8f", data, offset + 104)
                if (tz, rx, ry, rz, sx, sy) != (0, 0, 0, 0, 1, 1):
                    raise ValueError("logo pane has an unexpected transform")
                if uv_count != 1 or uv != (0, 0, 1, 0, 0, 1, 1, 1):
                    raise ValueError("logo pane has unexpected UV coordinates")
                panes[name] = {"name": name, "origin": origin, "x": tx, "y": ty, "width": w, "height": h}
        offset += size
    ordered = [panes[f"sqex_logo_{n:03d}"] for n in range(2)]
    left, right = ordered
    if left["origin"] != 2 or right["origin"] != 1:
        raise ValueError("expected a right-anchored left pane and left-anchored right pane")
    if (left["x"], left["y"]) != (right["x"], right["y"]):
        raise ValueError("logo panes do not share an anchor")
    for pane, image in zip(ordered, images):
        if (pane["width"], pane["height"]) != image.size:
            raise ValueError("logo pane and texture dimensions differ")
    return ordered


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path, help="extracted title archive with timg/ and blyt/")
    parser.add_argument("output", type=Path, help="output directory for PNGs and verification.json")
    args = parser.parse_args()
    decoded = [decode_logo(args.assets / "timg" / f"sqex_logo_{n:03d}.bflim") for n in range(2)]
    images = [item[0] for item in decoded]
    panes = verify_layout(args.assets / "blyt" / "title000_u.bflyt", images)
    if images[0].height != images[1].height:
        raise ValueError("logo parts have different heights")
    joined = Image.new("RGB", (sum(im.width for im in images), images[0].height))
    x = 0
    args.output.mkdir(parents=True, exist_ok=True)
    for n, im in enumerate(images):
        im.save(args.output / f"sqex_logo_{n:03d}.png")
        joined.paste(im, (x, 0))
        x += im.width
    joined.save(args.output / "sqex_logo.png")
    joined.resize((joined.width * 4, joined.height * 4), Image.Resampling.NEAREST).save(
        args.output / "sqex_logo_4x.png"
    )
    report = {"textures": [item[1] for item in decoded], "layout_panes": panes,
              "assembled_size": list(joined.size), "join_x": images[0].width}
    (args.output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {joined.width}x{joined.height} {args.output / 'sqex_logo.png'}")
    print("Both pixel payloads survived exact decoding/repacking; BFLYT confirms an edge-to-edge join.")


if __name__ == "__main__":
    main()
