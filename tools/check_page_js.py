#!/usr/bin/env python3
"""Синтаксична перевірка вбудованого JavaScript сторінок.

НАВІЩО. Сторінка будує майже весь інтерфейс скриптом. Одна помилка в ньому —
і браузер показує ПОРОЖНІЙ екран: HTML прийшов, розмітка є, а нічого не
намальовано. Знайти таке без браузера ніяк, і саме на це витрачається найбільше
часу в листуванні («білий екран» нічого не пояснює).

Перевіряє всі вбудовані <script> у index.html, client_usb.html і аварійній
сторінці PAGE_LITE із web_server.h. Потрібен node; якщо його немає — чесно
каже про це й нічого не стверджує.

Запуск (із теки скетча):
    python3 tools/check_page_js.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def scripts_from_html(text):
    """Уміст усіх <script> без src."""
    return [m.group(1) for m in
            re.finditer(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>', text, re.S | re.I)]


def lite_from_cpp(text):
    """Аварійна сторінка лежить у C++ як склеєний рядковий літерал."""
    m = re.search(r'static const char PAGE_LITE\[\] PROGMEM =\s*(.*?);\s*\n', text, re.S)
    if not m:
        return []
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    # Рівно те, що зробить компілятор: \" -> " і \\ -> \ , більше нічого.
    html = "".join(parts).replace('\\"', '"').replace('\\\\', '\\')
    return scripts_from_html(html)


def main() -> int:
    node = shutil.which("node") or shutil.which("nodejs")
    if not node:
        print("node не знайдено — перевірку пропущено (це не «пройдено»)")
        return 0

    jobs = []
    for name in ("index.html", "client_usb.html"):
        p = os.path.join(ROOT, name)
        if os.path.exists(p):
            jobs.append((name, scripts_from_html(open(p, encoding="utf-8").read())))
    ws = os.path.join(ROOT, "web_server.h")
    if os.path.exists(ws):
        jobs.append(("web_server.h: PAGE_LITE", lite_from_cpp(open(ws, encoding="utf-8").read())))

    bad = 0
    for name, chunks in jobs:
        if not chunks:
            print("  %-28s скриптів не знайдено" % name)
            bad += 1
            continue
        src = "\n;\n".join(chunks)
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
            f.write(src)
            tmp = f.name
        try:
            r = subprocess.run([node, "--check", tmp], capture_output=True, text=True)
        finally:
            os.unlink(tmp)
        if r.returncode:
            print("  %-28s ✗ %s" % (name, r.stderr.strip().splitlines()[0] if r.stderr else "помилка"))
            for line in r.stderr.strip().splitlines()[1:6]:
                print("      " + line)
            bad += 1
        else:
            print("  %-28s ок (%d символів у %d скриптах)" % (name, len(src), len(chunks)))

    print("\n%s" % ("Є ПОМИЛКИ" if bad else "синтаксис усіх сторінок цілий"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
