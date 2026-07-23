#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Конвертер зображення -> КОЛЬОРОВА заставка для кольорових TFT ST7789
(display_color.h). Генерує файл custom_splash.h з масивом RGB565.

Бере БУДЬ-ЯКЕ ваше зображення (на яке ви маєте права), вписує у задані
розміри (за пропорціями, з фоном) і друкує C-масив `splash_rgb565[]`.
Покладіть згенерований custom_splash.h у папку скетчу і ввімкніть у
settings.h:  #define DISPLAY_SPLASH_CUSTOM

Потрібен Pillow:  pip install pillow
Приклади:
  python make_color_splash.py logo.png                 # 240x240 -> custom_splash.h
  python make_color_splash.py logo.png -W 240 -H 280   # для ST7789V3 240x280
  python make_color_splash.py logo.png -W 240 -H 320 --fit cover
  python make_color_splash.py logo.png --bg 000000     # колір фону (hex RRGGBB)
  python make_color_splash.py logo.png -o custom_splash.h
"""
import sys, argparse
try:
    from PIL import Image
except ImportError:
    print("Потрібен Pillow:  pip install pillow"); sys.exit(1)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("-W", "--width", type=int, default=240, help="ширина, px (за замовч. 240)")
    ap.add_argument("-H", "--height", type=int, default=240, help="висота, px (за замовч. 240)")
    ap.add_argument("--fit", choices=["contain", "cover"], default="contain",
                    help="contain — вписати повністю (з фоном); cover — заповнити з обрізанням")
    ap.add_argument("--bg", default="000000", help="колір фону HEX RRGGBB (за замовч. 000000)")
    ap.add_argument("--name", default="splash_rgb565", help="ім'я масиву")
    ap.add_argument("-o", "--out", default="custom_splash.h", help="вихідний файл (.h)")
    a = ap.parse_args()

    W, H = a.width, a.height
    if W < 1 or H < 1 or W > 320 or H > 320:
        print("Розмір має бути 1..320 (матриця ST7789 макс. 240x320).", file=sys.stderr)
    try:
        bg = tuple(int(a.bg[i:i + 2], 16) for i in (0, 2, 4))
    except Exception:
        print("Некоректний --bg, очікується HEX RRGGBB (напр. 001B3A)."); sys.exit(1)

    img = Image.open(a.image).convert("RGB")

    if a.fit == "cover":
        # Заповнити весь екран, обрізавши зайве (по центру).
        sc = max(W / img.width, H / img.height)
        nw, nh = max(1, round(img.width * sc)), max(1, round(img.height * sc))
        img = img.resize((nw, nh), Image.LANCZOS)
        left, top = (nw - W) // 2, (nh - H) // 2
        img = img.crop((left, top, left + W, top + H))
        canvas = img
    else:
        # Вписати повністю, лишок — фоном.
        im = img.copy()
        im.thumbnail((W, H), Image.LANCZOS)
        canvas = Image.new("RGB", (W, H), bg)
        canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2))

    px = canvas.load()
    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            vals.append(rgb565(r, g, b))

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("// Кольорова заставка (RGB565) для кольорових TFT ST7789.\n")
        f.write("// Згенеровано tools/make_color_splash.py. Увімкнути:\n")
        f.write("//   settings.h -> #define DISPLAY_SPLASH_CUSTOM\n")
        f.write("#ifndef CUSTOM_SPLASH_H\n#define CUSTOM_SPLASH_H\n")
        f.write("#include <stdint.h>\n")
        f.write(f"#define SPLASH_W {W}\n#define SPLASH_H {H}\n")
        f.write(f"static const uint16_t {a.name}[] = {{\n")
        for i in range(0, len(vals), 12):
            f.write("  " + ", ".join("0x%04X" % v for v in vals[i:i + 12]) + ",\n")
        f.write("};\n#endif // CUSTOM_SPLASH_H\n")

    kb = len(vals) * 2 / 1024.0
    print(f"OK: {a.out}  ({W}x{H}, {len(vals)} пікселів, ~{kb:.1f} КБ у флеші)")
    print("Покладіть custom_splash.h у папку скетчу і ввімкніть DISPLAY_SPLASH_CUSTOM у settings.h.",
          file=sys.stderr)


if __name__ == "__main__":
    main()
