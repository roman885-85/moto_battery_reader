#!/usr/bin/env python3
"""Вшити веб-сторінку в прошивку.

ЧОМУ ЦЕ ПОТРІБНО. Сторінка жила в SPIFFS, а туди її треба заливати окремим
кроком (Tools -> ESP32 Sketch Data Upload). У Arduino IDE 2.x цього пункту
взагалі немає, а після зміни схеми розділів («Huge APP») старий образ SPIFFS
не монтується — і SPIFFS.begin(true) мовчки форматує розділ. Власник побачив
рівно це: /api/fs повернув {"total":836081,"used":0,"files":[]}, тобто
файлової системи з даними в пристрої не було зовсім, а браузер показував
порожній екран.

Тому сторінка тепер лежить у самій прошивці — стиснута gzip, як і в SPIFFS.
89 КБ на розділі 3 МБ, зайнятому на 40 %, — прийнятна ціна за те, що пристрій
працює одразу після прошивки, без другого кроку й без файлової системи.

Запуск (із теки скетча):
    python3 tools/mk_page_header.py

Пише page_index.h. Охоронець 27 звіряє його з index.html за CRC32 і довжиною,
тож забути перегенерувати не вийде.
"""
import gzip
import zlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "index.html")
OUT = os.path.join(ROOT, "page_index.h")


def main() -> int:
    if not os.path.exists(SRC):
        print("немає %s" % SRC)
        return 1
    raw = open(SRC, "rb").read()
    # mtime=0 — щоб файл був однаковий при кожному запуску: інакше git бачив би
    # зміну там, де вміст той самий.
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    # CRC32 самого СТИСНУТОГО масиву: пристрій рахує його в себе й порівнює,
    # тож «чи цілий блок у флеші» перевіряється без комп'ютера.
    gzcrc = zlib.crc32(gz) & 0xFFFFFFFF

    lines = [
        "#pragma once",
        "// ЗГЕНЕРОВАНО: python3 tools/mk_page_header.py — руками не правити.",
        "//",
        "//  Веб-сторінка, вшита в прошивку й стиснута gzip. Пристрій віддає її з",
        "//  заголовком Content-Encoding: gzip, тож браузер розпаковує сам.",
        "//",
        "//  Навіщо в прошивці, а не у файловій системі: SPIFFS треба заливати",
        "//  окремим кроком, у Arduino IDE 2.x такого пункту немає, а зміна схеми",
        "//  розділів робить старий образ немонтовним — і він мовчки форматується.",
        "//  Вшита сторінка працює одразу після прошивки й не залежить ні від чого.",
        "",
        "#define PAGE_INDEX_RAW_LEN  %uu   // розмір ВИХІДНОГО index.html" % len(raw),
        "#define PAGE_INDEX_RAW_CRC  0x%08Xu // і його CRC32 — звіряє охоронець" % crc,
        "#define PAGE_INDEX_GZ_CRC   0x%08Xu // CRC32 масиву нижче — звіряє сам пристрій" % gzcrc,
        "",
        "static const uint8_t PAGE_INDEX_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(gz), 16):
        chunk = gz[i:i + 16]
        lines.append("  " + ",".join("0x%02X" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const size_t PAGE_INDEX_GZ_LEN = sizeof(PAGE_INDEX_GZ);")
    lines.append("")

    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("%s: %d Б -> %d Б у gzip (%.1f×), CRC32 %08X"
          % (os.path.basename(OUT), len(raw), len(gz), len(raw) / len(gz), crc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
