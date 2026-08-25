#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ДВІ ТОПОЛОГІЇ ЗАРЯДУ — чи не лишилось у живому коді чужих імен.

  ЩО ЦЕ ВЗАГАЛІ ЗА ПЕРЕВІРКА. Прошивка вміє дві силові частини заряду:
  власний понижувач (CHARGE_TOP_BUCK) і готову плату DC/DC на TL494
  (CHARGE_TOP_TL494). Кожна приносить свої макроси й забирає чужі: у TL494
  немає ні CHARGE_PWM_PIN, ні шунта, ні подільника; у понижувача немає
  калібрувальної таблиці й enable. Помилитись тут дуже легко й дуже тихо.

  ЧОМУ НЕ ДОСИТЬ ПРОСТО ЗІБРАТИ. Прошивка на хості не збирається взагалі (за
  нею увесь Arduino-світ), тож помилку «звертання до неіснуючого макроса» ми
  побачили б лише в Arduino IDE — тобто не побачили б до релізу. А друга,
  гірша половина взагалі не є помилкою збірки: невідомий ідентифікатор у #if
  препроцесор МОВЧКИ вважає нулем, і перевірка, яка на нього посилається,
  просто перестає щось перевіряти, лишаючись на вигляд справною. Цей проєкт
  на такому вже горів (порівняння з неіснуючим DISCHARGE_RAMP_LO_MV).

  ЯК ПЕРЕВІРЯЄМО. Препроцесувати прошивку можна навіть тоді, коли зібрати не
  можна. Беремо САМ СКЕТЧ (він тягне settings.h, charge.h, web_server.h, обидва
  драйвери екрана, меню й serial_api — тобто накриває все, а не одну ділянку),
  женемо через cpp за кожної топології — і дивимось, які імена
  ВИЖИЛИ в результаті. Виживає рівно те, що не було визначене: усе визначене
  препроцесор розкрив би. Тож будь-яке ім'я з settings.h, знайдене у виводі
  cpp, — це звертання до макроса, якого за цієї топології не існує.

  Рядкові сталі не рахуємо: «закоментуйте CHARGE_PSU_PIN у settings.h» — це
  текст для людини, а не звертання до макроса.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TOPOLOGIES = ((0, "власний понижувач"), (1, "готова плата TL494"))

# Імена, які визначає сам скетч чи бібліотека, а не settings.h, — їх у виводі
# cpp повно, і до топології вони не мають стосунку.
IGNORE = re.compile(r"^(?:CHARGE_H|SETTINGS_H|WEB_SERVER_H)$")


def defined_names():
    """Усі імена, які settings.h узагалі коли-небудь визначає — за будь-якої
    топології. Беремо з тексту, а не з препроцесора: нас цікавить саме повний
    список, включно з тими, що в цій конфігурації вимкнені."""
    txt = open(os.path.join(ROOT, "settings.h"), encoding="utf-8").read()
    return {m.group(1) for m in re.finditer(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)", txt, re.M)}


def preprocess(topology):
    cmd = ["g++", "-E", "-std=c++17",
           "-Itools/fake/pp", "-I.", "-Itools/fake",
           "-DCHARGE_TOPOLOGY=%d" % topology,
           "-x", "c++", "tools/ws_preprocess.cpp"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        tail = (r.stderr or r.stdout).strip().splitlines()
        return None, "\n        ".join(tail[-4:])
    return r.stdout, None


def strip_strings(src):
    """Прибрати рядкові й символьні сталі: імена макросів усередині них — це
    текст повідомлення, а не звертання."""
    src = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)
    src = re.sub(r"'(?:[^'\\\n]|\\.)*'", "' '", src)
    return src


def main():
    known = defined_names()
    fails = 0
    print("Дві топології заряду: чи не лишилось у живому коді чужих імен")
    for topo, name in TOPOLOGIES:
        out, err = preprocess(topo)
        if out is None:
            print("   ЗБІЙ  топологія %d (%s): препроцесор не пройшов\n        %s"
                  % (topo, name, err))
            fails += 1
            continue
        code = strip_strings(out)
        seen = {m.group(0) for m in re.finditer(r"\b[A-Z][A-Z0-9_]{2,}\b", code)}
        stray = sorted(n for n in (seen & known) if not IGNORE.match(n))
        if stray:
            print("   ЗБІЙ  топологія %d (%s): звертання до невизначених макросів — %s"
                  % (topo, name, ", ".join(stray)))
            fails += 1
        else:
            print("   ок    топологія %d (%s)" % (topo, name))

    print("\n%s (помилок: %d)" % ("Є ПОМИЛКИ" if fails else "усі перевірки пройдено", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
