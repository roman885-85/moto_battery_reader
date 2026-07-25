#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_dump.py — розбір і порівняння дампів IMPRES-чіпів Motorola.

Читає дамп DS2433 (512 Б) і, за наявності, DS2438 (64 Б), друкує анотовану
карту полів (заголовок + Σ, ASCII-маркери, модель, TLV-записи, ємність,
поля DS2438, дзеркало) і перевіряє інваріанти, якими користується прошивка:

    • заголовок DS2433 0x00..0x1F : сума ≡ 0x41 (байт 0x1F — контрольний);
    • TLV-запис                    : сума всіх байт запису ≡ 0x5A;
    • дзеркало DS2438[24:50] == DS2433[1:27];
    • ємність, %                   : запис 0x17, зсув +21;
    • ICA (паливомір)              : DS2438[0x0C];
    • ETM (лічильник напрацювання) : DS2438[0x08:0x0B], LE, секунди;
    • CCA / DCA                    : DS2438[0x3C:0x3E] / [0x3E:0x40], LE.

Використання:
    # Розбір дампа
    python decode_dump.py dump33.bin [dump38.bin]

    # Витягти вшитий еталон із templates.h у .bin (для звірки клонів)
    python decode_dump.py --template PMNN4409B --out ref
        -> ref_33.bin (512) + ref_38.bin (64)

    # Побайтова звірка «еталон vs клон» з класифікацією полів
    python decode_dump.py ref_33.bin ref_38.bin --diff clone33.bin clone38.bin
"""
import sys, os, re, argparse

DUMP33 = 512
DUMP38 = 64

# ---- відомі поля для анотації diff (зсув -> (довжина, підпис, «летке?») ) ----
# «летке» = очікувано змінюється (лічильники/живі виміри/контр.суми) — розбіжність
# у таких полях НЕ є проблемою клонування; «стале» — ідентичність/калібрування.
def field_map_33(d):
    fields = {}
    fields[0x1F] = (1, "контр.сума заголовка", True)
    # модель
    mi = find_model(d)
    if mi >= 0:
        fields[mi] = (11, "запис моделі 0x0B", False)
    # запис ємності 0x17 (+21 = %)
    ci = find_record(d, 0x17)
    if ci >= 0:
        fields[ci + 21] = (1, "ємність, % (0x17+21)", True)
    return fields

def field_map_38():
    return {
        0x00: (8, "page0: живі виміри (T/V/I)", True),
        0x08: (3, "ETM лічильник напрацювання", True),
        0x0C: (1, "ICA паливомір", True),
        0x18: (26, "дзеркало DS2433[1:27] / калібр.", False),
        0x3C: (2, "CCA лічильник заряду", True),
        0x3E: (2, "DCA лічильник розряду", True),
    }

# ---------------------------------------------------------------- утиліти
def hx(b):  return ' '.join('%02X' % x for x in b)
def asc(b): return ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)

def find_model(d):
    for i in range(0x30, len(d) - 11):
        if d[i] == 0x0B and 0x41 <= d[i + 1] <= 0x5A:
            return i
    return -1

def find_record(d, tag, start=0x100):
    for i in range(start, len(d) - 23):
        if d[i] == tag and d[i + 1] == 0x00:
            return i
    return -1

def header_sum(d):  return sum(d[0:0x20]) & 0xFF

def grab_template(name):
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, '..', 'templates.h'), encoding='utf-8', errors='ignore').read()
    out = {}
    for suf in ('33', '38'):
        m = re.search(r'TPL_%s_%s\[\d+\] *PROGMEM *= *\{(.*?)\};' % (name, suf), src, re.S)
        if not m:
            return None
        out[suf] = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
    return out

# ---------------------------------------------------------------- розбір
def decode(d33, d38):
    print("=" * 68)
    print("DS2433  (%d Б)" % len(d33))
    print("=" * 68)
    hs = header_sum(d33)
    print("Заголовок 0x00..0x1F   Σ=%02X   %s" %
          (hs, "OK (≡0x41)" if hs == 0x41 else "!! ХИБНА (треба 0x41)"))
    print("  hex :", hx(d33[0:0x20]))
    print("  asc :", asc(d33[0:0x20]))

    print("\nASCII-маркери:")
    for mk in (b'COPYRIGHT2014', b'COPYRIGHT2017', b'COPYRIGHT2021',
               b'MOTOROLASOLUTIONS', b'MOTOROLA', b'PMNN', b'PMMN', b'APLI', b'NNTN'):
        i = d33.find(mk)
        if i >= 0:
            print("  %-18s @ 0x%03X" % (mk.decode(), i))
    fmt = ("2021 (IMPRES 2 / R7)" if d33.find(b'COPYRIGHT2021') >= 0 else
           "2017 (MOTOROLA-auth)" if d33.find(b'MOTOROLA') >= 0 and d33.find(b'COPYRIGHT2014') < 0 else
           "2014 (IMPRES 1)"      if d33.find(b'COPYRIGHT2014') >= 0 else "невідомий")
    print("  => формат прошивки:", fmt)

    mi = find_model(d33)
    if mi >= 0:
        name = asc(d33[mi + 1:mi + 11]).rstrip('.')
        print("\nМодель: '%s'  @0x%03X  (запис 0x0B, Σ=%02X)" %
              (name, mi, sum(d33[mi:mi + 11]) & 0xFF))

    print("\nTLV-записи (тег @зсув  дані  Σ):")
    for tag, ln, lbl in ((0x0D, 13, "калібр. ємності CCA/DCA"),
                         (0x16, 22, "калібрувальна таблиця/крива"),
                         (0x17, 23, "історія ємності (+21 = %)")):
        i = find_record(d33, tag)
        if i >= 0:
            s = sum(d33[i:i + ln]) & 0xFF
            print("  0x%02X @0x%03X  %s   Σ=%02X %s" %
                  (tag, i, hx(d33[i:i + ln]), s, "OK" if s == 0x5A else "≠0x5A"))
    ci = find_record(d33, 0x17)
    if ci >= 0:
        cap = d33[ci + 21]
        print("  -> ЄМНІСТЬ (health) = %d%%   знос = %d%%" % (cap, 100 - cap) if cap <= 100
              else "  -> байт ємності = 0x%02X (поза 0..100)" % cap)

    # learned-calib записи 0x0A у калібр-зоні
    la = [i for i in range(0x30, len(d33) - 2) if d33[i] == 0x0A]
    if la:
        print("\nЗаписи 0x0A (learned-calib) на зсувах:",
              ', '.join('0x%03X' % i for i in la[:16]), ('…' if len(la) > 16 else ''))

    if d38:
        print("\n" + "=" * 68)
        print("DS2438  (%d Б)" % len(d38))
        print("=" * 68)
        for off in range(0, len(d38), 16):
            print("  0x%02X: %s | %s" % (off, hx(d38[off:off + 16]), asc(d38[off:off + 16])))
        etm = d38[8] | (d38[9] << 8) | (d38[10] << 16)
        cca = d38[60] | (d38[61] << 8)
        dca = d38[62] | (d38[63] << 8)
        print("\n  ETM  0x08..0x0A = %d с  (%.1f діб)" % (etm, etm / 86400.0))
        print("  ICA  0x0C       = %d  (0x%02X)  ≈ паливомір" % (d38[12], d38[12]))
        print("  CCA  0x3C..0x3D = %d%s" % (cca, "  !! перепов. (0xFFFF)" if cca == 0xFFFF else ""))
        print("  DCA  0x3E..0x3F = %d" % dca)
        mir_ok = d38[24:50] == d33[1:27]
        print("  Дзеркало DS2438[24:50] == DS2433[1:27]: %s" %
              ("СХОДИТЬСЯ" if mir_ok else "!! РОЗБІЖНІСТЬ"))
        if not mir_ok:
            print("     38:", hx(d38[24:50]))
            print("     33:", hx(d33[1:27]))

# ---------------------------------------------------------------- diff
def classify(off, fields):
    for base, (ln, lbl, vol) in fields.items():
        if base <= off < base + ln:
            return lbl, vol
    return "(невідоме поле)", None

def diff(ref33, ref38, cl33, cl38):
    print("=" * 68)
    print("DIFF  еталон -> клон   (● стале/важливе   ○ летке/лічильник)")
    print("=" * 68)
    def run(a, b, fields, tag):
        if not a or not b:
            return
        n = min(len(a), len(b))
        rows = 0
        for i in range(n):
            if a[i] != b[i]:
                lbl, vol = classify(i, fields)
                mark = '○' if vol else ('●' if vol is False else '?')
                print("  %s %s 0x%03X: %02X -> %02X   %s" % (tag, mark, i, a[i], b[i], lbl))
                rows += 1
        if len(a) != len(b):
            print("  %s !! різна довжина: %d vs %d" % (tag, len(a), len(b)))
        if rows == 0:
            print("  %s: ідентичні" % tag)
    run(ref33, cl33, field_map_33(ref33), "33")
    if ref38 and cl38:
        run(ref38, cl38, field_map_38(), "38")
    print("\nПідсумок: '●' — розбіжності в ідентичності/калібруванні (ймовірна\n"
          "причина відмови рації/ЗП). '○' — лічильники/живі виміри (норма).")

# ---------------------------------------------------------------- main
def load(path, size):
    b = open(path, 'rb').read()
    if len(b) != size:
        print("! %s: %d Б (очікувалось %d)" % (path, len(b), size), file=sys.stderr)
    return b

def main():
    ap = argparse.ArgumentParser(description="Розбір/порівняння дампів IMPRES.")
    ap.add_argument("dump33", nargs='?', help="DS2433 .bin (512 Б)")
    ap.add_argument("dump38", nargs='?', help="DS2438 .bin (64 Б)")
    ap.add_argument("--template", help="витягти вшитий еталон із templates.h (напр. PMNN4409B)")
    ap.add_argument("--out", default="ref", help="префікс для --template (out_33.bin/out_38.bin)")
    ap.add_argument("--diff", nargs='+', metavar="CLONE", help="клон: clone33.bin [clone38.bin]")
    a = ap.parse_args()

    if a.template:
        t = grab_template(a.template)
        if not t:
            print("Шаблон '%s' не знайдено у templates.h" % a.template); sys.exit(1)
        open(a.out + "_33.bin", "wb").write(t['33'])
        open(a.out + "_38.bin", "wb").write(t['38'])
        print("Записано %s_33.bin (%d) і %s_38.bin (%d)" %
              (a.out, len(t['33']), a.out, len(t['38'])))
        decode(t['33'], t['38'])
        return

    if not a.dump33:
        ap.print_help(); sys.exit(1)
    d33 = load(a.dump33, DUMP33)
    d38 = load(a.dump38, DUMP38) if a.dump38 else None
    decode(d33, d38)

    if a.diff:
        c33 = load(a.diff[0], DUMP33)
        c38 = load(a.diff[1], DUMP38) if len(a.diff) > 1 else None
        print()
        diff(d33, d38, c33, c38)

if __name__ == "__main__":
    main()
