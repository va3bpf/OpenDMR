#!/usr/bin/env python3
"""
mkboot.py - Boot Splash Screen Converter for OpenDMR (240x320 4-bpp palette LCD)

Converts any image (PNG/JPG/BMP) into OpenDMR 4-bpp (16-color palette) format.
Outputs a C header file (boot_logo_default.h) or a raw binary file for CPS.

Usage:
  python3 tools/image/mkboot.py logo.png -o src/graphics/boot_logo_default.h
  python3 tools/image/mkboot.py logo.png --bin boot_image.bin
  python3 tools/image/mkboot.py --create-default -o src/graphics/boot_logo_default.h
"""

import os
import argparse
from PIL import Image, ImageDraw, ImageFont

WIDTH = 240
HEIGHT = 320
NUM_COLORS = 16
PIXEL_BYTES = (WIDTH * HEIGHT) // 2  # 38400 bytes


def rgb_to_native_bgr565(r, g, b):
    """
    Convert 8-bit RGB to the 16-bit native palette format used by panel_rgb888_to_native()
    in src/drivers/panel.c: BGR565, byte-swapped.
    """
    bgr565 = ((r & 0xF8) >> 3) | ((g & 0xFC) << 3) | ((b & 0xF8) << 8)
    return ((bgr565 & 0xFF) << 8) | ((bgr565 >> 8) & 0xFF)


def create_default_logo_image():
    """
    Generates a default OpenDMR 240x320 portrait splash logo image.
    Uses a solid dark navy background so all 16 palette slots preserve crisp colors and text anti-aliasing.
    """
    bg_dark = (7, 19, 31)       # Matches OpenDMR Deep Sea theme background (#07131F)
    card_bg = (20, 38, 56)      # Dark slate blue
    cyan_bright = (56, 189, 248)
    cyan_mid = (14, 165, 233)
    white = (255, 255, 255)
    slate_light = (148, 163, 184)

    img = Image.new("RGB", (WIDTH, HEIGHT), color=bg_dark)
    draw = ImageDraw.Draw(img)

    # Top and bottom accent bars
    draw.rectangle([0, 0, WIDTH, 4], fill=cyan_mid)
    draw.rectangle([0, HEIGHT - 4, WIDTH, HEIGHT], fill=cyan_mid)

    # Center circle / emblem
    cx, cy = WIDTH // 2, 115
    r_outer = 56
    r_inner = 46
    draw.ellipse([cx - r_outer, cy - r_outer, cx + r_outer, cy + r_outer],
                 fill=card_bg, outline=cyan_bright, width=3)
    draw.ellipse([cx - r_inner, cy - r_inner, cx + r_inner, cy + r_inner],
                 fill=bg_dark, outline=cyan_mid, width=2)

    # Radio wave arc lines inside emblem
    for radius in [16, 26, 36]:
        draw.arc([cx - radius, cy - radius + 6, cx + radius, cy + radius + 6],
                 start=210, end=330, fill=cyan_bright, width=2)

    # Small center dot (antenna tip)
    draw.ellipse([cx - 5, cy + 16 - 5, cx + 5, cy + 16 + 5], fill=white)

    # Load fonts
    try:
        font_title = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 28)
        font_sub = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 14)
        font_ver = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 13)
        font_calls = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 11)
    except IOError:
        try:
            font_title = ImageFont.truetype("DejaVuSans-Bold.ttf", 28)
            font_sub = ImageFont.truetype("DejaVuSans.ttf", 14)
            font_ver = ImageFont.truetype("DejaVuSans-Bold.ttf", 13)
            font_calls = ImageFont.truetype("DejaVuSans.ttf", 11)
        except IOError:
            font_title = font_sub = font_ver = font_calls = ImageFont.load_default()

    # Draw Title Text "OpenDMR"
    title_text = "OpenDMR"
    bbox = draw.textbbox((0, 0), title_text, font=font_title)
    w_title = bbox[2] - bbox[0]
    draw.text(((WIDTH - w_title) // 2, 196), title_text, fill=white, font=font_title)

    # Draw Subtitle Text "Open Firmware"
    sub_text = "Open Firmware"
    bbox = draw.textbbox((0, 0), sub_text, font=font_sub)
    w_sub = bbox[2] - bbox[0]
    draw.text(((WIDTH - w_sub) // 2, 234), sub_text, fill=slate_light, font=font_sub)

    # Draw Radio model "DM-32UV"
    model_text = "DM-32UV"
    bbox = draw.textbbox((0, 0), model_text, font=font_ver)
    w_mod = bbox[2] - bbox[0]
    draw.text(((WIDTH - w_mod) // 2, 265), model_text, fill=cyan_bright, font=font_ver)

    # Draw Contributors Callsigns "VA3BPF - LU8HPA - HJ6MVV"
    callsigns_text = "VA3BPF - LU8HPA - HJ6MVV"
    bbox = draw.textbbox((0, 0), callsigns_text, font=font_calls)
    w_call = bbox[2] - bbox[0]
    draw.text(((WIDTH - w_call) // 2, 294), callsigns_text, fill=slate_light, font=font_calls)

    return img


def convert_image(img):
    """
    Quantizes PIL Image to 16-color palette and packs into 4bpp framebuffer format.
    Returns (palette_bgr565_list, pixel_bytes, quantized_rgb_image)
    """
    img = img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    img_quant = img.quantize(colors=NUM_COLORS, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)

    raw_palette = img_quant.getpalette() or []
    # Pad palette if fewer than 16 colors were used
    while len(raw_palette) < NUM_COLORS * 3:
        raw_palette.extend([0, 0, 0])

    palette_native = []
    for i in range(NUM_COLORS):
        r = raw_palette[i * 3]
        g = raw_palette[i * 3 + 1]
        b = raw_palette[i * 3 + 2]
        palette_native.append(rgb_to_native_bgr565(r, g, b))

    pixels = list(img_quant.get_flattened_data()) if hasattr(img_quant, 'get_flattened_data') else list(img_quant.getdata())
    packed_bytes = bytearray(PIXEL_BYTES)

    for i in range(PIXEL_BYTES):
        p0 = pixels[i * 2] & 0x0F
        p1 = pixels[i * 2 + 1] & 0x0F
        packed_bytes[i] = (p0 << 4) | p1

    return palette_native, packed_bytes, img_quant.convert("RGB")


def main():
    parser = argparse.ArgumentParser(description="OpenDMR Boot Splash Image Converter")
    parser.add_argument("input_image", nargs="?", help="Path to input image (PNG/JPG/BMP)")
    parser.add_argument("-o", "--output-header", help="Output C header file (e.g. boot_logo_default.h)")
    parser.add_argument("--bin", help="Output raw binary file for CPS")
    parser.add_argument("--preview", help="Save the actual 16-color quantized preview PNG")
    parser.add_argument("--create-default", action="store_true", help="Generate default OpenDMR logo")
    args = parser.parse_args()

    if args.create_default or not args.input_image:
        img = create_default_logo_image()
    else:
        img = Image.open(args.input_image).convert("RGB")

    palette, pixel_data, preview_img = convert_image(img)

    if args.preview:
        preview_img.save(args.preview)
        print(f"Saved 16-color quantized preview: {args.preview}")

    if args.bin:
        with open(args.bin, "wb") as f:
            for val in palette:
                f.write(val.to_bytes(2, byteorder="little"))
            f.write(pixel_data)
        print(f"Generated binary file: {args.bin} ({len(palette)*2 + len(pixel_data)} bytes)")

    if args.output_header:
        out_path = args.output_header
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w") as f:
            f.write("/* Auto-generated by tools/image/mkboot.py for OpenDMR */\n")
            f.write("#ifndef BOOT_LOGO_DEFAULT_H\n")
            f.write("#define BOOT_LOGO_DEFAULT_H\n\n")
            f.write("#include <stdint.h>\n\n")
            f.write("#define BOOT_LOGO_WIDTH  240\n")
            f.write("#define BOOT_LOGO_HEIGHT 320\n")
            f.write("#define BOOT_LOGO_BYTES  38400\n\n")

            f.write("static const uint16_t boot_logo_default_palette[16] = {\n    ")
            for i, p in enumerate(palette):
                f.write(f"0x{p:04X}, ")
                if (i + 1) % 8 == 0 and (i + 1) < len(palette):
                    f.write("\n    ")
            f.write("\n};\n\n")

            f.write("static const uint8_t boot_logo_default_pixels[BOOT_LOGO_BYTES] = {\n")
            for i in range(0, len(pixel_data), 16):
                chunk = pixel_data[i:i+16]
                hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
                f.write(f"    {hex_str},\n")
            f.write("};\n\n")

            f.write("#endif /* BOOT_LOGO_DEFAULT_H */\n")
        print(f"Generated C header: {out_path}")


if __name__ == "__main__":
    main()
