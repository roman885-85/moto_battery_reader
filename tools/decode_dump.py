#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_dump.py — розбір і порівняння дампів IMPRES-чіпів Motorola.

Структура прошивки описана в ../impres_format.h і в docs/FIRMWARE_ANALYSIS.md;
цей скрипт — її еталонна реалізація на Python для роботи з файлами.

Коротко:
  • DS2433 (512 Б) — ланцюг записів [ДОВЖИНА][дані][СУМА], Σ запису ≡ 0x5A;
    ланцюг «упакований» до кінця чипа (останній запис завершується на 0x200).
  • 0x000..0x065 — блок МОДЕЛІ (побайтово однаковий у всіх екземплярів моделі);
    заголовок 0x000..0x01F має Σ ≡ 0x41.
  • 0x18A..0x1FF — НАВЧЕНИЙ КАЛІБРУВАЛЬНИЙ ХВІСТ. Саме він блокує АКБ після
    заміни елементів: рація/ЗП відкидають ЧУЖИЙ хвіст, але приймають ПОРОЖНІЙ.
  • DS2438 (64 Б) — монітор; ETM 4 Б LE @0x08, ICA @0x0C, CCA/DCA LE @0x3C/0x3E,
    дзеркало DS2433[1:27] @0x18.

Використання:
    python decode_dump.py dump33.bin [dump38.bin]      # розбір
    python decode_dump.py --template PMNN4409B --out ref
    python decode_dump.py a33.bin a38.bin --diff b33.bin b38.bin
    python decode_dump.py --survey dumps/                # огляд усіх дампів
"""
import sys, os, re, glob, argparse

DUMP33, DUMP38 = 512, 64

HDR_END, ID_END = 0x020, 0x066
COPYRIGHT, FACTORY_REC, MODEL_REC = 0x0E0, 0x129, 0x148
LEARNED_BEGIN = 0x18A
REC_SUM, HDR_SUM = 0x5A, 0x41


# ---------------------------------------------------------------- утиліти
def hx(b):  return ' '.join('%02X' % x for x in b)
def asc(b): return ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)


def header_sum(d): return sum(d[0:HDR_END]) & 0xFF


def record_ok(d, off):
    if not (0 <= off < len(d)):
        return False
    ln = d[off]
    if ln < 2 or off + ln > len(d):
        return False
    return (sum(d[off:off + ln]) & 0xFF) == REC_SUM


def walk(d, start):
    """Ланцюг записів від start -> [(зсув, довжина, ok)], кінцевий зсув."""
    out, i = [], start
    while i < len(d):
        if d[i] == 0xFF:
            i += 1
            continue
        ln = d[i]
        if ln < 2 or i + ln > len(d):
            return out, -1
        out.append((i, ln, record_ok(d, i)))
        i += ln
    return out, i


def find_model(d):
    """Валідний запис моделі: довжина 0x0B, 9 символів [A-Z0-9 ], Σ≡0x5A."""
    def ok_at(o):
        if not record_ok(d, o) or d[o] != 0x0B:
            return False
        digit = letter = False
        for k in range(9):
            c = d[o + 1 + k]
            if 0x30 <= c <= 0x39: digit = True
            elif 0x41 <= c <= 0x5A: letter = True
            elif c != 0x20: return False
        return digit and letter
    if ok_at(MODEL_REC):
        return MODEL_REC
    for i in range(0x20, len(d) - 11):
        if ok_at(i):
            return i
    return -1


def model_name(d):
    o = find_model(d)
    return bytes(d[o + 1:o + 10]).decode('ascii').strip() if o >= 0 else None


def tail_state(d):
    """'blank' | 'fresh' | 'learned' | 'broken' — стан хвоста 0x18A..0x1FF."""
    if all(x in (0xFF, 0x00) for x in d[LEARNED_BEGIN:]):
        return 'blank'
    recs, end = walk(d, LEARNED_BEGIN)
    bad = sum(1 for _, _, ok in recs if not ok)
    # рівно один запис у хвості ([0x03][прапорець][прапорець]) суми не має
    if not (end == len(d) and len(recs) >= 3 and bad <= 1):
        return 'broken'
    # «чистий»: скелет цілий, історія розряду (перший запис) обнулена
    if d[LEARNED_BEGIN] == 0x16 and all(x == 0 for x in d[LEARNED_BEGIN + 1:LEARNED_BEGIN + 0x15]):
        return 'fresh'
    return 'learned'


def fmt_year(d):
    for mk, y in ((b'COPYRIGHT2021', 2021), (b'COPYRIGHT2014', 2014)):
        if d.find(mk) >= 0:
            return y
    return 2017 if d.find(b'MOTOROLA') >= 0 else 0


def etm(e):  return int.from_bytes(e[8:12], 'little')
def cca(e):  return int.from_bytes(e[60:62], 'little')
def dca(e):  return int.from_bytes(e[62:64], 'little')


RATED = {'PMNN4409A': 2150, 'PMNN4409B': 2250, 'PT4409A': 2150,
         'PMNN4488A': 3000, 'PMNN4493A': 3000,
         'PMNN4809A': 2450, 'APLI4810C': 2450}


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
    print("=" * 72)
    print("DS2433  (%d Б)" % len(d33))
    print("=" * 72)
    hs = header_sum(d33)
    print("Заголовок 0x000..0x01F   Σ=%02X   %s" %
          (hs, "OK (≡0x41)" if hs == HDR_SUM else "!! ХИБНА (треба 0x41)"))
    print("  hex :", hx(d33[0:HDR_END]))

    name = model_name(d33)
    print("\nБлок МОДЕЛІ 0x000..0x065 — однаковий у всіх екземплярів моделі.")
    print("  Модель : %s  (запис @0x%03X)" % (name or "— не знайдено —", find_model(d33)))
    print("  Формат : %s" % (fmt_year(d33) or "невідомий"))
    print("  Паспортна ємність (таблиця проєкту): %s мА·год" % RATED.get(name, "?"))

    ci = d33.find(b'COPYRIGHT')
    if ci >= 0:
        print("  Copyright @0x%03X: %s" % (ci, asc(d33[ci:ci + 30])))

    if record_ok(d33, FACTORY_REC):
        ln = d33[FACTORY_REC]
        print("\nЗаводська таблиця @0x%03X (довжина %d) — КОНСТАНТА МОДЕЛІ, не здоров'я АКБ:" %
              (FACTORY_REC, ln))
        print("  " + hx(d33[FACTORY_REC:FACTORY_REC + ln]))

    print("\nЛанцюг записів від 0x120  (зсув  довж  Σ  дані):")
    recs, end = walk(d33, 0x120)
    for off, ln, ok in recs:
        mark = "OK " if ok else "≠5A"
        tag = ""
        if off == MODEL_REC:      tag = "  <- МОДЕЛЬ"
        elif off == FACTORY_REC:  tag = "  <- заводська таблиця"
        elif off >= LEARNED_BEGIN: tag = "  <- навчений хвіст"
        print("  0x%03X  %3d  %s  %s%s" % (off, ln, mark, hx(d33[off:off + ln])[:60], tag))
    print("  ланцюг завершився на 0x%X %s" %
          (end, "(рівно на кінці чипа — OK)" if end == len(d33) else "!! (очікувалось 0x200)"))

    ts = tail_state(d33)
    print("\n⚑ НАВЧЕНИЙ КАЛІБРУВАЛЬНИЙ ХВІСТ 0x18A..0x1FF: %s" % ts.upper())
    print({
        'blank':   "  ⛔ СТЕРТИЙ у 0xFF — скелет записів знищено. Рація АКБ приймає, але ЗП\n"
                   "  пише навчені значення за ФІКСОВАНИМИ адресами і структуру заново НЕ\n"
                   "  створює, тож калібрування щоразу завершується помилкою. Потрібен\n"
                   "  «чистий» хвіст (скелет + нулі), а не стирання.",
        'fresh':   "  ✅ ЧИСТИЙ: скелет записів цілий, навчені значення обнулені, суми вірні.\n"
                   "  Це цільовий стан після заміни елементів — ЗП може провести калібрування.",
        'learned': "  Навчені дані ПРИСУТНІ. Якщо це не рідні дані цього пакета (заміна\n"
                   "  елементів / записаний чужий еталон) — рація скаже «невідомий акумулятор».",
        'broken':  "  Пошкоджений ланцюг записів — потрібен ремонт після заміни елементів.",
    }[ts])

    if d38:
        print("\n" + "=" * 72)
        print("DS2438  (%d Б)" % len(d38))
        print("=" * 72)
        for off in range(0, len(d38), 16):
            print("  0x%02X: %s | %s" % (off, hx(d38[off:off + 16]), asc(d38[off:off + 16])))
        e = etm(d38)
        print("\n  статус/конфіг 0x00 = 0x%02X %s" % (d38[0], "" if d38[0] == 0x0F else "!! (у справних 0x0F)"))
        print("  поріг         0x07 = 0x%02X %s" % (d38[7], "" if d38[7] == 0x40 else "!! (у справних 0x40)"))
        print("  напруга             = %.2f В" % ((d38[3] | d38[4] << 8) * 0.01))
        print("  ETM  0x08..0x0B     = %d с (%.2f року)   [4 байти LE]" % (e, e / 31557600.0))
        print("       рація показує «перше користування» як (її час − ETM)")
        print("  ICA  0x0C           = %d" % d38[12])
        print("  CCA  0x3C..0x3D     = %d%s" % (cca(d38), "  !! перепов." if cca(d38) == 0xFFFF else ""))
        print("  DCA  0x3E..0x3F     = %d" % dca(d38))
        mir = d38[24:50] == d33[1:27]
        print("  Дзеркало DS2438[24:50] == DS2433[1:27]: %s" % ("СХОДИТЬСЯ" if mir else "!! РОЗБІЖНІСТЬ"))
        if tail_state(d33) == 'learned' and e < 86400:
            print("\n  ⚠️ Навчена калібровка присутня, а напрацювання < доби —")
            print("     дані майже напевно НЕ від цього пакета (записаний чужий еталон")
            print("     або замінені елементи). Це і є причина «невідомого акумулятора».")

    print("\nПримітка: «строк служби, %» і «дата першого користування» у прошивці НЕ")
    print("зберігаються — рація рахує їх сама (див. docs/FIRMWARE_ANALYSIS.md).")


# ---------------------------------------------------------------- survey
def survey(root):
    rows = []
    for f in sorted(glob.glob(os.path.join(root, '*', 'files', '*_2433.bin'))):
        d33 = open(f, 'rb').read()
        p38 = f.replace('_2433.bin', '_2438.bin')
        d38 = open(p38, 'rb').read() if os.path.exists(p38) else None
        rows.append((os.path.relpath(f, root), model_name(d33) or '—',
                     fmt_year(d33) or '?', 'OK' if header_sum(d33) == HDR_SUM else 'BAD',
                     tail_state(d33), etm(d38) if d38 else -1))
    print("%-46s %-10s %-5s %-4s %-8s %s" % ("файл", "модель", "фмт", "hdr", "хвіст", "ETM, с"))
    for r in rows:
        flag = ("  <== чужа навчена калібровка" if (r[4] == 'learned' and 0 <= r[5] < 86400)
                else "  <== СТЕРТО: ЗП не завершить калібрування" if r[4] == 'blank' else "")
        print("%-46s %-10s %-5s %-4s %-8s %-11d%s" % (r[0], r[1], r[2], r[3], r[4], r[5], flag))
    n = len(rows)
    print("\nусього %d; хвіст: blank=%d fresh=%d learned=%d broken=%d; без моделі=%d; заголовок BAD=%d" %
          (n, sum(1 for r in rows if r[4] == 'blank'),
           sum(1 for r in rows if r[4] == 'fresh'),
           sum(1 for r in rows if r[4] == 'learned'),
           sum(1 for r in rows if r[4] == 'broken'),
           sum(1 for r in rows if r[1] == '—'),
           sum(1 for r in rows if r[3] == 'BAD')))


# ---------------------------------------------------------------- diff
def classify(off):
    if off < HDR_END:        return "заголовок ідентичності", False
    if off < ID_END:         return "профіль моделі / крива", False
    if off < COPYRIGHT:      return "журнал використання", True
    if off < 0x100:          return "copyright", False
    if off < 0x120:          return "журнал використання", True
    if off < MODEL_REC:      return "заводська таблиця моделі", False
    if off < 0x153:          return "ЗАПИС МОДЕЛІ", False
    if off < LEARNED_BEGIN:  return "навчені записи ємності", True
    return "НАВЧЕНИЙ КАЛІБРУВАЛЬНИЙ ХВІСТ", True


def diff(a33, a38, b33, b38):
    print("=" * 72)
    print("DIFF   (● стале/ідентичність   ○ навчене/лічильники)")
    print("=" * 72)
    rows = 0
    for i in range(min(len(a33), len(b33))):
        if a33[i] != b33[i]:
            lbl, vol = classify(i)
            print("  33 %s 0x%03X: %02X -> %02X   %s" % ('○' if vol else '●', i, a33[i], b33[i], lbl))
            rows += 1
    if rows == 0:
        print("  33: ідентичні")
    if a38 and b38:
        names = {0: "конфіг", 7: "поріг", 12: "ICA"}
        for i in range(min(len(a38), len(b38))):
            if a38[i] != b38[i]:
                lbl = names.get(i, "ETM" if 8 <= i <= 11 else
                                   "дзеркало ідентичності" if 24 <= i < 50 else
                                   "CCA" if 60 <= i <= 61 else "DCA" if 62 <= i <= 63 else "живі виміри/user")
                vol = not (24 <= i < 50)
                print("  38 %s 0x%02X: %02X -> %02X   %s" % ('○' if vol else '●', i, a38[i], b38[i], lbl))
    print("\n'●' — розбіжність у сталій частині (ідентичність/модель): підозріло.")
    print("'○' — навчені дані та лічильники: змінюються під час експлуатації.")


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
    ap.add_argument("--out", default="ref", help="префікс для --template")
    ap.add_argument("--diff", nargs='+', metavar="OTHER", help="other33.bin [other38.bin]")
    ap.add_argument("--survey", metavar="DIR", help="огляд усіх дампів у теці dumps/")
    a = ap.parse_args()

    if a.survey:
        survey(a.survey); return

    if a.template:
        t = grab_template(a.template)
        if not t:
            print("Шаблон '%s' не знайдено у templates.h" % a.template); sys.exit(1)
        open(a.out + "_33.bin", "wb").write(t['33'])
        open(a.out + "_38.bin", "wb").write(t['38'])
        print("Записано %s_33.bin (%d) і %s_38.bin (%d)\n" %
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
