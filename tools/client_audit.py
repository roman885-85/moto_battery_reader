# Аудит клієнтів: (1) кожен onclick має існуючу функцію; (2) кожен $('id') має
# елемент; (3) API/команди, які клієнт кличе, існують у прошивці.
import re, sys, json, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
fails = 0
def bad(m):
    global fails; print("  ЗБІЙ  " + m); fails += 1

fw = (root/'web_server.h').read_text(encoding='utf-8')
sa = (root/'serial_api.h').read_text(encoding='utf-8')
routes = set(re.findall(r'server\.on\("([^"]+)"', fw))
cmds   = set(re.findall(r'cmd == "([A-Z0-9]+)"', sa))
print("прошивка: %d маршрутів, %d USB-команд" % (len(routes), len(cmds)))

for name in ('index.html', 'client_usb.html'):
    src = (root/name).read_text(encoding='utf-8')
    print("\n=== %s" % name)
    # 1. onclick -> функція існує
    calls = set(re.findall(r'onclick="([A-Za-z_$][\w$]*)\s*\(', src))
    calls |= set(re.findall(r'onchange="([A-Za-z_$][\w$]*)\s*\(', src))
    defined = set(re.findall(r'function\s+([A-Za-z_$][\w$]*)\s*\(', src))
    defined |= set(re.findall(r'(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s*)?\(', src))
    defined |= set(re.findall(r'(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s+)?function', src))
    defined |= {'sub','showSub','showPage'}
    miss = sorted(c for c in calls if c not in defined)
    print("   обробників у розмітці: %d, визначено: %d" % (len(calls), len(defined)))
    for m in miss: bad("%s: onclick=\"%s()\" — функції немає" % (name, m))

    # 2. $('id') -> елемент існує
    ids_used = set(re.findall(r"\$\('([\w-]+)'\)", src))
    ids_have = set(re.findall(r'id="([\w-]+)"', src))
    # id, які створюються динамічно всередині шаблонних рядків
    ids_have |= set(re.findall(r"id=\\?'?\"?([\w-]+)\\?'?\"?", src))
    ghosts = sorted(i for i in ids_used if i not in ids_have)
    print("   id у коді: %d, у розмітці: %d" % (len(ids_used), len(ids_have)))
    for g in ghosts: bad("%s: $('%s') — такого елемента немає" % (name, g))

    # 3. API / команди існують у прошивці
    if name == 'index.html':
        used = set(re.findall(r"['\"](/api/[\w/]+)", src))
        for u in sorted(used):
            if u not in routes: bad("%s: викликає %s — маршруту немає" % (name, u))
        print("   API-точок використано: %d" % len(used))
    else:
        used = set(re.findall(r"cmd\('([A-Z0-9]+)", src))
        for u in sorted(used):
            if u not in cmds: bad("%s: шле команду %s — її немає" % (name, u))
        print("   USB-команд використано: %d" % len(used))

# --- GUI ---
gui = (root/'usb_client'/'moto_gui.py').read_text(encoding='utf-8')
print("\n=== usb_client/moto_gui.py")
handlers = set(re.findall(r'command=self\.(\w+)', gui))
methods  = set(re.findall(r'def (\w+)\(', gui))
for h in sorted(handlers):
    if h not in methods: bad("moto_gui: command=self.%s — методу немає" % h)
print("   кнопок із command=self.X: %d, методів: %d" % (len(handlers), len(methods)))
gcmds = set(re.findall(r'(?:self\.)?cmd\(f?"([A-Z0-9]+)', gui))
for u in sorted(gcmds):
    if u not in cmds: bad("moto_gui: шле команду %s — її немає" % u)
print("   USB-команд використано: %d" % len(gcmds))

# --- паритет функцій між клієнтами ---
print("\n=== паритет: команди/API по клієнтах")
web_api = set(re.findall(r"['\"](/api/[\w/]+)", (root/'index.html').read_text(encoding='utf-8')))
usb_cmd = set(re.findall(r"cmd\('([A-Z0-9]+)", (root/'client_usb.html').read_text(encoding='utf-8')))
print("   веб: %d API   брідж: %d команд   GUI: %d команд" % (len(web_api), len(usb_cmd), len(gcmds)))
only_bridge = sorted(usb_cmd - gcmds)
only_gui    = sorted(gcmds - usb_cmd)
if only_bridge: print("   лише в бріджі: " + ", ".join(only_bridge))
if only_gui:    print("   лише в GUI:    " + ", ".join(only_gui))

# ── Ключ шифрування береться з ROM ЦІЛЬОВОГО чипа ──────────────────────────
#  Дані, які потребують шифрування, мусять шифруватись ключем із серійного
#  номера того акумулятора, у який їх пишуть. impresCryptWrite() приймає ключ
#  звичайними параметрами, тож сама сигнатура цього не гарантує — перевіряємо
#  джерела: у кожному виклику ключ має походити з ROM, і виклик має стояти під
#  умовою «ROM відомий». Інакше в пакет піде шифровка під чужим ключем, і
#  рація прочитає сміття — рівно та біда, від якої лікуємо.
print("\n=== ключ шифрування = ROM цільового чипа")
ROM_OK = ("romK1", "romK2", "chipSN2433", "rom33", "packRom33")
GUARD  = ("haveRom", "hasSN2433")
sites = 0
for name in ("restore_plan.h", "web_server.h", "recovery.h", "serial_api.h", "impres_clone.h"):
    f = root / name
    if not f.exists():
        continue
    src = f.read_text(encoding="utf-8")
    lines = src.splitlines()
    for i, ln in enumerate(lines):
        if "impresCryptWrite(" not in ln and "impresIdentityWrite(" not in ln:
            continue
        if ln.lstrip().startswith(("//", "*")) or "inline " in ln:
            continue          # оголошення й коментарі, а не виклик
        sites += 1
        if not any(k in ln for k in ROM_OK):
            bad("%s:%d — ключ не з ROM: %s" % (name, i + 1, ln.strip()[:70]))
        # Умова «ROM відомий» має стояти вище — зазвичай вона відкриває весь
        # блок запису, а той буває довгим: у restore_plan.h між if і самим
        # записом лежить підготовка полів на 29 рядків. Тому вікно широке.
        ctx = "\n".join(lines[max(0, i - 45):i + 1])
        if not any(g in ctx for g in GUARD):
            bad("%s:%d — виклик не під умовою «ROM відомий»" % (name, i + 1))
print("   місць запису шифрованого: %d" % sites)
if sites == 0:
    bad("жодного виклику не знайдено — перевірка нічого не значить")

print("\n%s (помилок: %d)" % ("Є ПОМИЛКИ" if fails else "усі перевірки пройдено", fails))
sys.exit(1 if fails else 0)
