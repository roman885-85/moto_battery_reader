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
import json
import io
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
    check("вихідний" in lo and "вхідний" not in lo.replace("вихідний", ""),
          "…і вихідний названо вихідним, а не сплутано з вхідним")
    check("CH340" in G.port_label("COM3", "USB-SERIAL CH340", USB),
          "звичайний порт підписується як і раніше")

    # ── ІМ'Я ПРИЛАДУ В РЯДКУ СПИСКУ ──────────────────────────────────────
    #  Windows підписує всі Bluetooth-порти однаково, тож без цього прилад у
    #  списку був без назви взагалі.
    import tempfile
    check("A1B2C3D4E5F6" in G.port_label("COM5", "x", OUT),
          "поки ім'я невідоме — у рядку адреса, а не порожнє місце")
    names = G.bt_names_merge({}, "A1B2C3D4E5F6", "MotoBattery-E5F6")
    check("MotoBattery-E5F6" in G.port_label("COM5", "x", OUT, names),
          "щойно прилад назвався — порт підписано ІМЕНЕМ")
    # ⚑ ІМ'Я НЕ ВИГАДУЄТЬСЯ Й НЕ ПРИЛИПАЄ ДО ЧУЖОЇ АДРЕСИ.
    check("MotoBattery" not in G.port_label("COM7", "x",
          OUT.replace("A1B2C3D4E5F6", "0102030405F0"), names),
          "чужий прилад не отримує чужого імені")
    check(G.bt_names_merge(names, "", "X") == names and
          G.bt_names_merge(names, "AABBCCDDEEFF", "") == names,
          "порожня адреса чи порожнє ім'я не потрапляють у пам'ять")
    check(G.bt_names_merge(names, "A1B2C3D4E5F6", "Інший") ["A1B2C3D4E5F6"] == "Інший",
          "перейменований прилад перезаписує стару назву, а не додає другу")

    # Довідник, без якого все працює, не сміє валити програму.
    d = tempfile.mkdtemp()
    path = G.bt_names_path(d)
    check(G.bt_names_load(path) == {}, "немає файла — просто ще нічого не знаємо")
    check(G.bt_names_save(path, names) and G.bt_names_load(path) == names,
          "записали — прочитали те саме")
    with io.open(path, "w", encoding="utf-8") as f:
        f.write("{це не json")
    check(G.bt_names_load(path) == {}, "побитий файл не валить програму, а просто мовчить")

    # ── ЗНАЙТИ ПРИЛАД САМОМУ ─────────────────────────────────────────────
    #  У системі десяток портів, жоден не підписаний іменем приладу. Але
    #  вгадувати й не треба: прилад представляється сам у відповідь на PING.
    ports = [("COM3", "CH340", USB), ("COM6", "BT", INC),
             ("COM5", "BT", OUT), ("COM7", "BT", OUT.replace("A1B2C3D4E5F6", "FFEEDDCCBBAA"))]
    order = G.port_probe_order(ports)
    #  ⚑ ВХІДНИЙ КАНАЛ НЕ ПИТАЄМО ВЗАГАЛІ. Його відкриття не просто марне — воно
    #  ВИСНЕ на кілька секунд, а таких портів у системі стільки ж, скільки
    #  спарених пристроїв. Пошук, який упирається в них усі, читався б як
    #  зависання програми.
    check("COM6" not in order, "вхідний Bluetooth-канал у пошук не потрапляє")
    check(set(order) == {"COM3", "COM5", "COM7"}, "решта портів питається — жоден не загублено")
    check(order.index("COM5") < order.index("COM3") and
          order.index("COM7") < order.index("COM3"),
          "Bluetooth питається раніше за звичайні порти")
    known = G.port_probe_order(ports, {"FFEEDDCCBBAA": "MotoBattery-BBAA"})
    check(known[0] == "COM7", "знайомий прилад питається ПЕРШИМ")

    check(G.probe_is_our_device({"ok": True, "dev": G.PROBE_DEV}),
          "наш прилад упізнається за тим, як він сам себе назвав")
    check(not G.probe_is_our_device({"ok": True, "dev": "ChinaScale"}),
          "чужий пристрій за наш не видається")
    check(not G.probe_is_our_device({"ok": True}) and not G.probe_is_our_device(None)
          and not G.probe_is_our_device("MotoBatteryReader"),
          "відповідь без імені, порожня чи не-словник — це «ні», а не виняток")

    # ── ТІ САМІ ПРАВИЛА В МОСТУ ──────────────────────────────────────────
    #  ⚑ МІСТ — ТА ПОВЕРХНЯ, ДЕ РОЗХОДЖЕННЯ ПОМІЧАЮТЬ ОСТАННІМ: ним
    #  користуються рідше за програму, і своя копія логіки могла б тихо
    #  розійтися з нею надовго. Тому перевіряємо не поведінку (її вже
    #  перевірено вище), а те, що міст бере рішення З ТОГО САМОГО модуля.
    src = io.open(os.path.join(ROOT, "usb_client", "moto_bridge.py"),
                  encoding="utf-8").read()
    check("import moto_models" in src, "міст бере логіку портів із moto_models")
    check("port_probe_order" in src and "probe_is_our_device" in src,
          "…і пошук приладу в ньому теж є")
    check("port_label" in src, "…і підписує порти тим самим підписом")
    check('u.path == "/find"' in src, "…а сторінка має до чого звертатись (/find)")
    #  Копії правил у мосту бути не повинно: одна відповідь на одне питання.
    check("BTHENUM" not in src, "міст не тримає власної копії ознаки Bluetooth")

    # ── А ТЕПЕР ГОЛОВНЕ: СТОРІНКУ ПИТАЄМО ТАК, ЯК ЇЇ ПИТАЄ БРАУЗЕР ───────
    #  ⚑ ЧОМУ ГРЕПУ ПО ВИХІДНОМУ ТЕКСТУ НЕ ВИСТАЧАЄ. Перевірки вище стерегли
    #  НАПИСАНЕ, а скарга власника була про ВІДДАНЕ: «кнопки нема». І текст, і
    #  маршрут були на місці — а сторінка не віддавалась узагалі, бо емодзі в
    #  рядку впорскування було записане парою сурогатів, і сервер падав на
    #  .encode("utf-8") ще до першого байта відповіді. Греп бачив кнопку;
    #  браузер не бачив нічого.
    #
    #  Тому тут піднімається СПРАВЖНІЙ обробник мосту на справжньому сокеті й
    #  сторінка читається по HTTP. pyserial на збірковій машині немає — і не
    #  треба: заглушка потрібна лише щоб модуль зібрався, портів у ній нема.
    import types, threading, urllib.request
    from http.server import ThreadingHTTPServer

    stub = types.ModuleType("serial"); stub.Serial = object
    tools_mod = types.ModuleType("serial.tools")
    lp = types.ModuleType("serial.tools.list_ports"); lp.comports = lambda: []
    tools_mod.list_ports = lp; stub.tools = tools_mod
    sys.modules.setdefault("serial", stub)
    sys.modules.setdefault("serial.tools", tools_mod)
    sys.modules.setdefault("serial.tools.list_ports", lp)

    argv_was, sys.argv = sys.argv, ["moto_bridge.py"]
    try:
        import moto_bridge as MB
    finally:
        sys.argv = argv_was

    srv = ThreadingHTTPServer(("127.0.0.1", 0), MB.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_address[1]
    page, err = None, None
    try:
        page = urllib.request.urlopen(base + "/", timeout=10).read().decode("utf-8")
    except Exception as e:
        err = "%s: %s" % (type(e).__name__, e)

    check(page is not None, "сторінка мосту взагалі віддається браузеру (%s)" % (err or "ок"))
    if page:
        #  Кнопку шукаємо в тому, що ПРИЙШЛО по мережі, а не у вихідному тексті.
        check("Знайти прилад" in page, "…і кнопка пошуку приладу в ній є")
        check("/find" in page, "…і вона має куди звертатись")
        #  ⚑ КНОПКА НЕ МУСИТЬ ЗАЛЕЖАТИ ВІД СПИСКУ ПОРТІВ. Тут портів немає
        #  зовсім (заглушка віддає порожньо) — саме той випадок, коли пошук
        #  потрібен найбільше. Раніше кнопка народжувалась усередині обробника
        #  /ports і при порожньому списку не з'являлась зовсім.
        i_ins, i_ports = page.rfind("insertBefore(fb"), page.find("fetch('/ports')")
        check(i_ins >= 0 and (i_ports < 0 or i_ins < i_ports),
              "…і вставляється ДО того, як міст питають про порти, а не в відповіді")
    srv.shutdown()

    # Довідка про порти по HTTP теж мусить віддаватись, а не падати.
    try:
        srv2 = ThreadingHTTPServer(("127.0.0.1", 0), MB.Handler)
        threading.Thread(target=srv2.serve_forever, daemon=True).start()
        raw = urllib.request.urlopen("http://127.0.0.1:%d/ports" % srv2.server_address[1],
                                     timeout=10).read().decode("utf-8")
        srv2.shutdown()
        ok_ports = json.loads(raw).get("ports") == []
    except Exception as e:
        ok_ports = False
    check(ok_ports, "перелік портів віддається у JSON навіть коли портів немає")

    # ── МІСТ НЕ ВМИРАЄ МОВЧКИ ────────────────────────────────────────────
    #  ⚑ ЦІЛИЙ КЛАС СКАРГ ЗВУЧИТЬ ОДНАКОВО: «сторінка не відкривається». Під
    #  цим щоразу було щось інше — і щоразу воно вбивало міст ДО того, як він
    #  устигав щось сказати. Тому тут перевіряється не окрема причина, а
    #  властивість: що б не сталося, або сторінка є, або названо адресу, де
    #  вона є.
    import re as _re

    def _spawn(argv, stdout_enc=None, squat=None):
        """Запустити main() у потоці й повернути надруковане ним."""
        buf = io.StringIO()
        if stdout_enc:
            import codecs
            class Narrow(io.StringIO):
                def write(self, t):
                    t.encode(stdout_enc)      # кине UnicodeEncodeError, як консоль
                    return io.StringIO.write(self, t)
            buf = Narrow()
        old_out, old_argv = sys.stdout, sys.argv
        sys.stdout, sys.argv = buf, argv
        try:
            threading.Thread(target=MB.main, daemon=True).start()
            time.sleep(2.0 if squat else 0.8)
        finally:
            sys.stdout, sys.argv = old_out, old_argv
        return buf.getvalue()

    import threading, time, socket, urllib.error

    def _page_at(text):
        m = _re.search(r"http://127\.0\.0\.1:\d+/", text or "")
        if not m:
            return None
        try:
            return urllib.request.urlopen(m.group(0), timeout=4).read().decode("utf-8")
        except Exception:
            return None

    #  1. Консоль, яка не вміє української. cp866 — типова консоль Windows на
    #  російській локалі; у ній немає ні «—», ні лапок-ялинок. Привітання в
    #  main() стояло ПЕРЕД serve_forever(), тож напис, якого ніхто не читає,
    #  не давав серверу початись узагалі.
    out = _spawn(["moto_bridge.py", "--no-browser", "--http", "8871"], stdout_enc="cp866")
    check(_page_at(out) is not None,
          "консоль, яка не вміє українських лапок, не заважає мосту працювати")

    #  2. Порт зайнято чужою програмою. Раніше bind() кидав виняток до першого
    #  надрукованого рядка: вікно блимало й зникало.
    squat = socket.socket(); squat.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    squat.bind(("127.0.0.1", 8873)); squat.listen(1)
    out = _spawn(["moto_bridge.py", "--no-browser", "--http", "8873"], squat=squat)
    page = _page_at(out)
    check(page is not None and "Знайти прилад" in page,
          "зайнятий порт: міст бере наступний вільний і називає адресу вголос")
    check("8873" in out and "зайнят" in out,
          "…і каже, ЧОМУ адреса не та, яку чекали")
    squat.close()

    #  3. Виняток усередині обробника. Найгірша відповідь — обірване
    #  з'єднання: браузер каже «не вдалося відкрити», і шукати нема де.
    class Boom(MB.Handler):
        def _route(self):
            raise RuntimeError("драйвер портів упав")
    srv3 = ThreadingHTTPServer(("127.0.0.1", 0), Boom)
    threading.Thread(target=srv3.serve_forever, daemon=True).start()
    body, code = None, None
    try:
        urllib.request.urlopen("http://127.0.0.1:%d/" % srv3.server_address[1], timeout=4).read()
    except urllib.error.HTTPError as e:
        code, body = e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        body = "З'ЄДНАННЯ ОБІРВАНО: %s" % e
    srv3.shutdown()
    check(code == 500 and body and "драйвер портів упав" in body,
          "виняток у обробнику стає сторінкою з причиною, а не обірваним з'єднанням")

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
