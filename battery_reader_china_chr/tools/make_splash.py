#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Конвертер зображення -> заставка дисплея (XBM 64x64) для display.h.

Бере БУДЬ-ЯКЕ ваше зображення (на яке ви маєте права), вписує у 64x64,
переводить у 1 біт і друкує масив у форматі `ngu_xbm[]`. Скопіюйте вивід у
display.h замість блоку `static const unsigned char ngu_xbm[] = { ... };`.

Потрібен Pillow:  pip install pillow
Запуск:           python make_splash.py my.png            # темне -> світиться
                  python make_splash.py my.png --invert   # інвертувати
                  python make_splash.py my.png --dither    # дизеринг замість порогу
                  python make_splash.py my.png --name my_xbm
"""
import sys, argparse
try:
    from PIL import Image
except ImportError:
    print("Потрібен Pillow:  pip install pillow"); sys.exit(1)

W = H = 64

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--invert", action="store_true", help="інвертувати (світле<->темне)")
    ap.add_argument("--dither", action="store_true", help="дизеринг (напівтони) замість порогу")
    ap.add_argument("--thresh", type=int, default=128, help="поріг 0..255 (за замовч. 128)")
    ap.add_argument("--name", default="ngu_xbm", help="ім'я масиву (за замовч. ngu_xbm)")
    a = ap.parse_args()

    img = Image.open(a.image).convert("L")
    img.thumbnail((W, H))                       # вписати, зберігши пропорції
    canvas = Image.new("L", (W, H), 255)        # білий фон
    canvas.paste(img, ((W - img.width) // 2, (H - img.height) // 2))

    if a.dither:
        bw = canvas.convert("1")                # Флойд–Стейнберг
    else:
        bw = canvas.point(lambda p: 255 if p >= a.thresh else 0, mode="1")

    # На OLED drawXBM засвічує пікселі для біт=1. Темний суб'єкт хочемо світлим:
    # PIL '1': 0=чорний. Беремо як 1 біти ті, що ТЕМНІ (0), якщо не --invert.
    px = bw.load()
    def bit(x, y):
        v = 1 if px[x, y] == 0 else 0           # темний -> 1 (світиться)
        return v ^ (1 if a.invert else 0)

    out = []
    for y in range(H):
        for xb in range(0, W, 8):
            b = 0
            for i in range(8):                  # XBM: LSB зліва
                if bit(xb + i, y): b |= (1 << i)
            out.append(b)

    print(f"#define NGU_W {W}")
    print(f"#define NGU_H {H}")
    print(f"static const unsigned char {a.name}[] = {{")
    for i in range(0, len(out), 12):
        print("  " + ", ".join("0x%02X" % v for v in out[i:i+12]) + ",")
    print("};")
    print(f"\n// {len(out)} байт. Вставте у display.h замість ngu_xbm[] (розмір лишається 64x64).",
          file=sys.stderr)

if __name__ == "__main__":
    main()
