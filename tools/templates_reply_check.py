#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""СПИСОК МОДЕЛЕЙ У КЛІЄНТІ-ПРОГРАМІ: що робити з відповіддю на TEMPLATES.

  ЩО САМЕ ТУТ ЛОВИТЬСЯ. Скарга власника: «в exe при відновленні з еталона в
  Майстрі у випадному списку не показуються еталони». Причина виявилась не в
  Майстрі. Список моделей клієнт питає РІВНО ОДИН РАЗ — командою TEMPLATES у
  мить під'єднання, — і оброблялась відповідь так:

      models = r.get("models", []) if r.get("ok") else []
      for cb in (...): cb["values"] = models

  Тобто будь-яка невдала відповідь (таймаут, зайнята шина, обірваний кабель)
  МОВЧКИ затирала вже завантажений список порожнім. Другого шляху до
  TEMPLATES в інтерфейсі не існувало, тож вийти з цього стану можна було лише
  перепідключенням — про яке ніде не сказано. Людина бачила порожній список і
  напис «Оберіть модель для відновлення»: вимогу, яку неможливо виконати.

  Чому саме програма, а не веб-клієнти. Ті питають моделі ПІСЛЯ повного
  оновлення (await refresh(); await loadTemplates()) і показують «(немає
  шаблонів)», коли список порожній. Програма ж слала п'ять команд пачкою, і
  TEMPLATES стояв ПЕРШИМ — одразу після відкриття порту, коли ESP ще досипає
  стартовий звіт у той самий UART.

  ЧОМУ ПЕРЕВІРКА ТУТ, А НЕ У ВІКНІ. Вікна Tk на збірковій машині немає (у
  контейнері немає навіть tkinter), тож обробник, написаний прямо у віджетах,
  не покриває жоден тест — і саме тому дефект прожив непоміченим. Рішення
  винесене в окремий модуль БЕЗ ЖОДНОГО ВІДЖЕТА (usb_client/moto_models.py) і
  ганяється тут напряму. Той самий прийом, що й із charge.h проти
  web_server.h: логіка живе там, куди тест дістає.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "usb_client"))

import moto_models as G

fails = 0


def check(cond, msg):
    global fails
    print(("   ок    " if cond else "   ЗБІЙ  ") + msg)
    if not cond:
        fails += 1


GOOD = ["PMNN4488A", "PMNN4493A", "PMNN4409A"]
OK_REPLY = {"ok": True, "models": GOOD}
TIMEOUT = {"ok": False, "err": "таймаут"}

# ── ПОРТИ: ДВА BLUETOOTH-РЯДКИ МУСЯТЬ РОЗРІЗНЯТИСЬ ────────────────────────
#  Спарований прилад дає ДВА порти з однаковим описом. Доти вони виглядали в
#  списку однаково, і вибір між ними був підкиданням монетки: обравши вхідний,
#  людина діставала відмову без пояснення.
def check_ports():
    OUT = (r"BTHENUM\{00001101-0000-1000-8000-00805F9B34FB}_VID&00010057_PID&0001"
           r"\8&31d1a2b&0&A1B2C3D4E5F6_C00000000")
    INC = (r"BTHENUM\{00001101-0000-1000-8000-00805F9B34FB}_LOCALMFG&0000"
           r"\7&1d2c0d1b&0&000000000000_00000000")
    USB = "USB VID:PID=1A86:7523 SER=0001"

    check(G.port_is_bluetooth(OUT) and G.port_is_bluetooth(INC),
          "обидва порти пари впізнаються як Bluetooth")
    check(not G.port_is_bluetooth(USB), "USB-перехідник за Bluetooth не видається")

    # ⚑ НАЙВАЖЛИВІШЕ ТУТ. Перша редакція шукала «12 шістнадцяткових цифр» і
    #  знаходила хвіст GUID послідовного сервісу — однаковий в УСІХ портів.
    #  Обидва виглядали «вихідними», тобто перевірка не перевіряла нічого.
    check(G.port_bt_addr(OUT) == "A1B2C3D4E5F6",
          "адреса приладу читається саме з адреси, а не з GUID сервісу")
    check(G.port_bt_outgoing(OUT) is True,  "вихідний порт розпізнано")
    check(G.port_bt_outgoing(INC) is False, "вхідний порт розпізнано")
    check(G.port_bt_outgoing(USB) is None,
          "для не-Bluetooth відповідь «не знаю», а не «ні»")

    lo, li = G.port_label("COM5", "Standard Serial over Bluetooth link", OUT), \
             G.port_label("COM6", "Standard Serial over Bluetooth link", INC)
    check(lo != li, "два Bluetooth-порти дають РІЗНІ рядки списку")
    check("ВИХІДНИЙ" in lo, "…і вихідний названо вихідним")
    check("CH340" in G.port_label("COM3", "USB-SERIAL CH340", USB),
          "звичайний порт підписується як і раніше")

    # Підказка мусить називати причину, а не повторювати системну помилку.
    h = G.port_open_hint(INC, "PermissionError(13)")
    check("вихідним" in h, "відмова на вхідному порту пояснює, що робити")
    check(G.port_open_hint(USB, "boom") == "boom",
          "для USB підказка не вигадується — віддається системна помилка")


print("СПИСОК МОДЕЛЕЙ: що робити з відповіддю на TEMPLATES\n")

print("1) вдала відповідь — просто беремо моделі")
m, note = G.templates_from_reply(OK_REPLY, [])
check(m == GOOD, "усі моделі з відповіді на місці, у тому ж порядку")
check(note is None, "…і жодної скарги в статус")
m, note = G.templates_from_reply(OK_REPLY, ["ЩОСЬ_СТАРЕ"])
check(m == GOOD, "свіжа відповідь ЗАМІНЮЄ попередній список, а не додається до нього")

print("\n2) ⚑ ГОЛОВНЕ: невдала відповідь НЕ затирає добрий список")
m, note = G.templates_from_reply(TIMEOUT, GOOD)
check(m == GOOD, "після таймауту список лишився тим самим")
check(note and "таймаут" in note, "…але причина названа, а не проковтнута")
check(note and "лишили" in note, "…і сказано, що показане — попереднє, не свіже")
# Найтонше: підряд кілька невдач не мусять «з'їдати» список поступово.
cur = GOOD
for i in range(5):
    cur, _ = G.templates_from_reply(TIMEOUT, cur)
check(cur == GOOD, "п'ять невдач поспіль — список і далі цілий")

print("\n3) порожньо теж буває по-різному, і поради різні")
m, note = G.templates_from_reply(TIMEOUT, [])
check(m == [G.TPL_NOT_LOADED], "нічого не було й не приїхало -> «не завантажено»")
check(note and "не завантажився" in note, "…з причиною в статусі")
m, note = G.templates_from_reply({"ok": True, "models": []}, [])
check(m == [G.TPL_NONE_FW], "прошивка ЧЕСНО відповіла «шаблонів немає» -> інший напис")
check(m != [G.TPL_NOT_LOADED], "…і його не сплутати з «не завантажено»")
check(note and "шаблон" in note.lower(), "…причина теж інша")
# Порожній список без жодного напису — це рівно та порожнеча, з якої почалась
# скарга: людина бачить порожній випадний список і не знає, чому.
for reply, cur in ((TIMEOUT, []), ({"ok": True, "models": []}, []),
                   ({"ok": False}, []), ({}, []), ("не словник", [])):
    m, _ = G.templates_from_reply(reply if reply != "не словник" else {}, cur)
    check(len(m) == 1 and m[0].startswith("("),
          "порожнеча ніколи не мовчить: %r -> %r" % (reply, m))

print("\n4) заглушку не можна ВИБРАТИ — і не можна відправити в запис")
check(G.templates_pick_valid(GOOD) == GOOD, "справжні моделі вибирати можна")
check(G.templates_pick_valid([G.TPL_NOT_LOADED]) == [], "«не завантажено» — не вибір")
check(G.templates_pick_valid([G.TPL_NONE_FW]) == [], "«немає шаблонів» — теж не вибір")
check(G.templates_pick_valid([]) == [], "порожній список — теж")
check(G.templates_pick_valid(None) == [], "і None не валить перевірку")
# ⚑ ЗАГЛУШКА НЕ МУСИТЬ ПРОЛІЗТИ НАЗАД ЯК «ПОПЕРЕДНІЙ ДОБРИЙ СПИСОК».
#  Дивимось саме на ПРИЧИНУ, а не на сам список: якщо фільтр прибрати, список
#  лишиться той самий («(список не завантажено…)»), зате людині скажуть
#  «лишили попередній» — тобто зроблять вигляд, що показане колись приїхало.
#  Перша редакція цієї перевірки порівнювала лише списки й тому мовчала.
m, note = G.templates_from_reply(TIMEOUT, [G.TPL_NOT_LOADED])
check(G.templates_pick_valid(m) == [], "заглушка не стає «збереженою моделлю»")
check(note and "попередній" not in note,
      "…і про неї не кажуть «лишили попередній» — лишати не було чого")
m, note = G.templates_from_reply(TIMEOUT, [G.TPL_NONE_FW])
check(G.templates_pick_valid(m) == [], "…жодна з двох")
check(note and "попередній" not in note, "…і для другої так само")

print("\n5) поверхні справді користуються цим рішенням")
src = open(os.path.join(ROOT, "usb_client", "moto_gui.py"), encoding="utf-8").read()
check("templates_from_reply(r, list(self.cbWiz" in src,
      "обробник відповіді питає саме цю функцію, а не рахує сам")
# ⚑ ІМ'Я З ДУЖКАМИ, А НЕ ПРОСТО ІМ'Я: «def ensure_templates» лишається
#  підрядком і в «def ensure_templates_DISABLED», тож перевірка на голе ім'я
#  пережила б вимкнення функції. Саме на цьому вона й попалась при звірці
#  від протилежного.
check("def ensure_templates(self):" in src and "self.ensure_templates()" in src,
      "є ДРУГИЙ шлях до списку, окрім миті під'єднання")
check("self.wizTplBtn" in src and 'command=self.load_templates' in src,
      "у Майстрі є кнопка «оновити список»")
# Усі три поверхні мусять питати ОДНУ функцію готовності: три копії умови
# розійшлися б, і десь лишилось би «оберіть модель» над порожнім списком.
check(src.count("self._models_ready(") == 3,
      "усі три поверхні (Майстер, «Новий», «Відновити») питають одну перевірку")
check("if cb.get() not in models:" in src,
      "вибране значення не лишається поза власним списком")
# ⚑ І СПУСКОВИЙ ГАЧОК ПРИБРАНО, а не лише наслідок. TEMPLATES більше не
#  стоїть першим у пачці команд під'єднання — тобто не питається в ту саму
#  мить, коли ESP ще досипає стартовий звіт у той самий UART.
#  ⚑ find(), А НЕ index(): відсутній рядок мусить дати ЗБІЙ перевірки, а не
#   виняток. Перша редакція падала з ValueError — тобто на зламаному коді тест
#   не «показував помилку», а розсипався, і зрозуміти з нього було нічого.
i_ref = src.find("self.refresh(), self.sound_load()")
i_tpl = src.find("self.load_templates()))")
check(i_ref >= 0 and i_tpl >= 0 and i_ref < i_tpl,
      "при під'єднанні моделі питають ПІСЛЯ оновлення, а не першими")

print("\n6) список портів: два Bluetooth-рядки розрізняються")
check_ports()

print("\n%s (помилок: %d)" % ("Є ПОМИЛКИ" if fails else "усі перевірки пройдено", fails))
sys.exit(1 if fails else 0)

print("\n6) список портів: два Bluetooth-рядки розрізняються")
check_ports()

print("\n%s (помилок: %d)" % ("Є ПОМИЛКИ" if fails else "усі перевірки пройдено", fails))
sys.exit(1 if fails else 0)
