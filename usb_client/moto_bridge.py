#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Moto IMPRES USB bridge — нативний клієнт для COM-порту.

Навіщо: Web Serial API працює лише у Chrome/Edge і лише з https/localhost.
Цей міст говорить з ESP32 напряму по COM-порту (pyserial), а сам показує той
самий інтерфейс (client_usb.html) у БУДЬ-ЯКОМУ браузері через локальний веб-сервер.
Пакується у один .exe (див. build.bat) — тоді користувачу не треба ні Python,
ні Chrome, ні возні з Web Serial.

Запуск із коду:   python moto_bridge.py            (автовибір єдиного COM-порту)
                  python moto_bridge.py --port COM5
Збірка .exe:      build.bat
"""
import sys, os, time, json, threading, webbrowser, urllib.parse, argparse
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ⚑ ТОЙ САМИЙ МОДУЛЬ, ЩО Й У ПРОГРАМИ. Підпис порту, розрізнення вхідного та
#  вихідного Bluetooth-каналів, порядок опитування й ознака «це наш прилад» —
#  усе це вже написане й покрите тестом у moto_models.py. Друга копія тут
#  розійшлася б із першою на першій же правці, а міст саме та поверхня, де
#  розходження помічають останнім.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import moto_models as mm


# ── ВИВІД У КОНСОЛЬ, ЯКИЙ НЕ МОЖЕ ВБИТИ МІСТ ──────────────────────────────
#  ⚑ ЗВИЧАЙНИЙ print() ТУТ — ЦЕ РИЗИК ВТРАТИТИ ВЕСЬ ПРИЛАД. Консоль Windows на
#  російській/українській локалі це cp866, і в ній немає ні «—», ні лапок
#  «ялинок». Коли вивід ще й перенаправлено (ярлик, запуск із іншої програми,
#  збірка PyInstaller), print() кидає UnicodeEncodeError — а стоїть він у
#  main() ПЕРЕД serve_forever(). Тобто привітання, яке ніхто не читає, не
#  давало серверу початись узагалі: вікно блимало й закривалось, браузер діставав
#  «connection refused».
#
#  Напис ніколи не важливіший за роботу. Не вліз у кодування консолі — хай
#  втратить розкіш (— стане -, «» стануть ""), але не роботу.
def say(text):
    try:
        print(text)
        return
    except Exception:
        pass
    try:
        enc = (getattr(sys.stdout, "encoding", None) or "ascii")
        sys.stdout.write(text.encode(enc, "replace").decode(enc, "replace") + "\n")
        sys.stdout.flush()
    except Exception:
        pass          # консолі може не бути взагалі (--windowed) — це не привід падати

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Потрібен пакет pyserial. Встановіть:  pip install pyserial")
    sys.exit(1)


def resource_path(name):
    """Шлях до файлу і в звичайному запуску, і всередині .exe (PyInstaller)."""
    if getattr(sys, "_MEIPASS", None):
        return os.path.join(sys._MEIPASS, name)
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, name), os.path.join(here, "..", name)):
        if os.path.exists(cand):
            return cand
    return os.path.join(here, name)


HTML_FILE = resource_path("client_usb.html")


class Bridge:
    """Один COM-порт, строго послідовний обмін запит/відповідь (#R#JSON)."""
    def __init__(self, baud=115200):
        self.baud = baud
        self.ser = None
        self.port = None
        self.lock = threading.Lock()

    def open(self, port):
        self.close()
        # timeout малий: read() повертається часто, ми самі стежимо за дедлайном.
        self.ser = serial.Serial(port, self.baud, timeout=0.2)
        self.port = port
        time.sleep(1.8)                 # ESP32 перезавантажується при відкритті порту
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        return True

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.port = None

    def cmd(self, c, timeout=8.0):
        if not self.ser:
            return {"ok": False, "err": "порт не відкрито"}
        with self.lock:
            try:
                self.ser.reset_input_buffer()      # відкинути асинхронні відладкові рядки
                # Великі команди шлемо частинами по 200 Б із мікропаузами, щоб не
                # переповнити 256-байтний UART-буфер ESP32 (без setRxBufferSize).
                data = (c + "\n").encode("utf-8")
                for i in range(0, len(data), 200):
                    self.ser.write(data[i:i + 200])
                    self.ser.flush()
                    if len(data) > 200:
                        time.sleep(0.01)
                deadline = time.time() + timeout
                buf = b""
                while time.time() < deadline:
                    chunk = self.ser.read(512)
                    if chunk:
                        buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.rstrip(b"\r").decode("utf-8", errors="replace")
                        if line.startswith("#R#"):
                            try:
                                return json.loads(line[3:])
                            except Exception:
                                return {"ok": False, "err": "bad json"}
                        # інші рядки (відладка ESP) — ігноруємо
                return {"ok": False, "err": "таймаут"}
            except Exception as e:
                return {"ok": False, "err": str(e)}


bridge = Bridge()
DEFAULT_PORT = [None]


def _ports_raw():
    return [(p.device, p.description or "", p.hwid or "")
            for p in serial.tools.list_ports.comports()]


def _bt_names():
    return mm.bt_names_load(mm.bt_names_path(os.path.expanduser("~")))


def list_ports():
    """Підписи — з moto_models: спарований прилад дає ДВА порти з однаковим
    описом «Standard Serial over Bluetooth link», і без підпису вибір між ними
    був підкиданням монетки."""
    names = _bt_names()
    return [{"port": d, "desc": mm.port_label(d, desc, hw, names)}
            for d, desc, hw in _ports_raw()]


def find_device():
    """Опитати порти й повернути той, який назветься нашим приладом.

    ⚑ ВХІДНІ BLUETOOTH-КАНАЛИ НЕ ПИТАЮТЬСЯ ВЗАГАЛІ — їх відкриття виснуть на
    кілька секунд, а таких портів стільки ж, скільки спарених пристроїв.
    Порядок і відбір рахує moto_models.port_probe_order().
    """
    ports = _ports_raw()
    bt = {d: mm.port_is_bluetooth(h) for d, _, h in ports}
    for dev in mm.port_probe_order(ports, _bt_names()):
        try:
            bridge.close()
            bridge.open(dev)
            # USB-порт скидає ESP32 при відкритті; Bluetooth нічого не скидає.
            time.sleep(0.4 if bt.get(dev) else 1.8)
            if mm.probe_is_our_device(bridge.cmd("PING", 2.5)):
                return dev
            bridge.close()
        except Exception:
            # Порт зайнятий, немає прав, це взагалі модем — не наша біда й не
            # привід зупиняти пошук.
            try:
                bridge.close()
            except Exception:
                pass
    return None


# Шар, що підмінює транспорт client_usb.html з Web Serial на локальний міст.
# Впорскується перед </body>; перевизначає глобальні cmd()/toggleConn().
#
#  ⚑ ТЕКСТ ТУТ ПИШЕТЬСЯ ТЕКСТОМ, А НЕ \\uXXXX. Файл у UTF-8, сторінка
#  віддається в UTF-8 — екранувати нема від чого. А ціна екранування виявилась
#  не косметичною: у звичайному (не r"") рядку Python \\uXXXX розбирає САМ, і
#  емодзі, записане парою сурогатів, ставало в рядку двома половинками, яких у
#  UTF-8 не існує. Сторінка після цього не віддавалась взагалі — сервер падав
#  на .encode("utf-8") ще до першого байта відповіді.
INJECT = """
<script>
(function(){
  var $=function(id){return document.getElementById(id);};
  var u=$('unsupported'); if(u) u.style.display='none';
  // Команди йдуть через локальний Python-міст, а не Web Serial:
  cmd=function(c,timeout){ timeout=timeout||8000;
    return fetch('/cmd?c='+encodeURIComponent(c)+'&t='+timeout)
      .then(function(r){return r.json();})
      .catch(function(e){return {ok:false,err:e.message};}); };
  // Підключення = відкрити COM-порт на мосту:
  toggleConn=async function(){
    if(connected){ await fetch('/close').catch(function(){}); connected=false;
      $('dot').classList.remove('on'); $('btnConn').textContent='🔌 Підключити';
      $('st').textContent='Не підключено'; return; }
    $('st').textContent='Відкриття порту...';
    try{
      var sel=$('bridgePort'); var pq=(sel&&sel.value)?('?port='+encodeURIComponent(sel.value)):'';
      var r=await (await fetch('/open'+pq)).json();
      if(r.ok){ connected=true; $('dot').classList.add('on'); $('btnConn').textContent='⏏ Відключити';
        $('st').textContent='Підключено ('+r.port+')';
        await refresh(); await loadTemplates(); await sndLoad(); await clLoadSamples(); await disTick(); }
      else { $('st').textContent='Помилка: '+(r.err||''); }
    }catch(e){ $('st').textContent='Міст недоступний: '+e.message; }
  };

  var btn=$('btnConn'); if(!btn) return;

  // ⚑ КНОПКА ПОШУКУ СТАВИТЬСЯ ОДРАЗУ, А НЕ У ВІДПОВІДІ ПРО ПОРТИ. Раніше вона
  //  народжувалась усередині обробника /ports і зникала разом із ним: не
  //  відповів міст, віддав порожній список, упало перерахування портів — і
  //  кнопки просто немає, без жодного пояснення. А потрібна вона саме тоді,
  //  коли з портами незрозуміло.
  var fb=document.createElement('button');
  fb.textContent='🔍 Знайти прилад';
  fb.style.cssText='margin-right:8px;background:#1b2430;color:#e7ecf3;border:1px solid #2a323f;border-radius:8px;padding:6px 10px;cursor:pointer';
  fb.onclick=async function(){
    fb.disabled=true;
    $('st').textContent='Пошук приладу...';
    try{
      var r=await (await fetch('/find')).json();
      if(r.ok){ var sel=$('bridgePort');
                if(sel){ var seen=false;
                  for(var i=0;i<sel.options.length;i++) if(sel.options[i].value===r.port){ sel.selectedIndex=i; seen=true; }
                  if(!seen){ var o=document.createElement('option'); o.value=r.port; o.textContent=r.port;
                             sel.appendChild(o); sel.selectedIndex=sel.options.length-1; } }
                if(!connected) await toggleConn(); }
      else $('st').textContent='Не знайдено: '+(r.err||'');
    }catch(e){ $('st').textContent='Міст недоступний: '+e.message; }
    fb.disabled=false;
  };
  btn.parentNode.insertBefore(fb, btn);

  // Список COM-портів поруч із кнопкою «Підключити» — окремо й після кнопки
  // пошуку, бо його поява залежить від відповіді мосту, а її може й не бути.
  fetch('/ports').then(function(r){return r.json();}).then(function(d){
    var ps=(d&&d.ports)||[]; if(!ps.length) return;
    var sel=document.createElement('select'); sel.id='bridgePort';
    sel.style.cssText='margin-right:8px;background:#0e1218;color:#e7ecf3;border:1px solid #2a323f;border-radius:8px;padding:6px';
    ps.forEach(function(p){var o=document.createElement('option');o.value=p.port;o.textContent=p.port+(p.desc?(' — '+p.desc):'');sel.appendChild(o);});
    fb.parentNode.insertBefore(sel, fb);
  }).catch(function(){});
})();
</script>
"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ── ЖОДЕН ЗАПИТ НЕ СМІЄ ЛИШИТИСЬ БЕЗ ВІДПОВІДІ ────────────────────────
    #  ⚑ ОБІРВАНЕ З'ЄДНАННЯ — НАЙГІРША З УСІХ ВІДПОВІДЕЙ. Саме так виглядав
    #  дефект із сурогатною парою: сторінка не віддавалась, браузер писав
    #  «не вдалося відкрити», і жодного натяку, ДЕ шукати. Виняток у обробнику
    #  за замовчуванням друкує слід у консоль (яку ніхто не бачить) і мовчки
    #  закриває сокет.
    #
    #  Тепер будь-який виняток стає сторінкою з причиною. Кодування тут
    #  навмисно поблажливе ("replace") — це остання відповідь, і краще показати
    #  текст із зіпсованим символом, ніж не показати нічого. У _send()
    #  кодування лишається суворим: інакше та сама сурогатна пара просто
    #  перетворилась би на «????» і дефект знову ніхто б не помітив.
    def do_GET(self):
        try:
            self._route()
        except Exception:
            body = ("<h2>Міст спіткнувся на запиті " + self.path + "</h2>"
                    "<p>Це не «сторінка не відкривається» — це сторінка, яка "
                    "каже, що саме сталося. Покажіть цей текст розробнику.</p>"
                    "<pre>" + traceback.format_exc() + "</pre>")
            try:
                self.send_response(500)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                b = body.encode("utf-8", "replace")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            except Exception:
                pass

    def _route(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path in ("/", "/index.html", "/client_usb.html"):
            try:
                html = open(HTML_FILE, encoding="utf-8").read()
            except Exception as e:
                self._send(500, "text/plain; charset=utf-8", "client_usb.html не знайдено: " + str(e))
                return
            html = html.replace("</body>", INJECT + "</body>")
            self._send(200, "text/html; charset=utf-8", html)
            return
        if u.path == "/ports":
            self._send(200, "application/json", json.dumps({"ports": list_ports()}))
            return
        if u.path == "/open":
            port = q.get("port", [DEFAULT_PORT[0]])[0]
            if not port:
                ps = list_ports()
                port = ps[0]["port"] if ps else None
            if not port:
                self._send(200, "application/json", json.dumps({"ok": False, "err": "нема COM-портів"}))
                return
            try:
                bridge.open(port)
                DEFAULT_PORT[0] = port
                r = {"ok": True, "port": port}
            except Exception as e:
                r = {"ok": False, "err": str(e)}
            self._send(200, "application/json", json.dumps(r, ensure_ascii=False))
            return
        if u.path == "/find":
            dev = find_device()
            if dev:
                DEFAULT_PORT[0] = dev
                r = {"ok": True, "port": dev}
            else:
                r = {"ok": False, "err": "прилад не відповів на жодному порту"}
            self._send(200, "application/json", json.dumps(r, ensure_ascii=False))
            return
        if u.path == "/close":
            bridge.close()
            self._send(200, "application/json", '{"ok":true}')
            return
        if u.path == "/cmd":
            c = q.get("c", [""])[0]
            t = float(q.get("t", ["8000"])[0]) / 1000.0
            r = bridge.cmd(c, t)
            self._send(200, "application/json", json.dumps(r, ensure_ascii=False))
            return
        self._send(404, "text/plain", "not found")

    def log_message(self, *a):
        pass   # тиша в консолі


def _bridge_answers(port):
    """Чи сидить на порту ЖИВИЙ міст. Питаємо його ж /ports: чужа програма на
    тому самому порту відповість чим завгодно, тільки не нашим JSON."""
    try:
        import urllib.request
        with urllib.request.urlopen("http://127.0.0.1:%d/ports" % port, timeout=1.5) as r:
            return isinstance(json.loads(r.read().decode("utf-8")).get("ports"), list)
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description="Moto IMPRES USB bridge")
    ap.add_argument("--port", help="COM-порт (напр. COM5). За замовч. — автовибір єдиного")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", type=int, default=8765, help="локальний HTTP-порт")
    ap.add_argument("--no-browser", action="store_true", help="не відкривати браузер")
    a = ap.parse_args()

    bridge.baud = a.baud
    ports = list_ports()
    if a.port:
        DEFAULT_PORT[0] = a.port
    elif len(ports) == 1:
        DEFAULT_PORT[0] = ports[0]["port"]

    # ── ЗАЙНЯТИЙ ПОРТ — НЕ ПРИВІД ПОМЕРТИ МОВЧКИ ──────────────────────────
    #  ⚑ ЦЕ ТРАПЛЯЄТЬСЯ ЧАСТІШЕ ЗА ВСЕ ІНШЕ. Міст закрили хрестиком, а процес
    #  лишився; або він і не закривався. Другий запуск падав на bind() з
    #  «Address already in use» ще ДО того, як щось надрукує: вікно блимало й
    #  зникало, а браузер відкривався на ту саму адресу — і його зустрічав
    #  СТАРИЙ примірник. Тобто оновлення виглядало як «нічого не змінилось» або
    #  як «сторінка померла», залежно від того, в якому стані був старий.
    #
    #  Тому: зайнято — питаємо, ХТО там. Свій міст уже працює? Кажемо про це й
    #  відкриваємо його. Хтось чужий? Беремо наступний вільний порт і чесно
    #  повідомляємо новий номер. Померти мовчки не можна в жодному з випадків.
    srv, port, note = None, a.http, None
    for cand in range(a.http, a.http + 12):
        try:
            srv = ThreadingHTTPServer(("127.0.0.1", cand), Handler)
            port = cand
            break
        except OSError:
            if cand == a.http and _bridge_answers(cand):
                say("Міст уже запущено на http://127.0.0.1:%d/ — відкриваю його." % cand)
                say("Якщо потрібен саме новий примірник, закрийте старий або "
                    "вкажіть інший порт: --http %d" % (cand + 1))
                if not a.no_browser:
                    webbrowser.open("http://127.0.0.1:%d/" % cand)
                return
            note = a.http
    if srv is None:
        say("Не вдалося зайняти жодного порту з %d..%d. Закрийте зайві програми "
            "або вкажіть вільний: --http <номер>" % (a.http, a.http + 11))
        return

    url = "http://127.0.0.1:%d/" % port
    say("=" * 52)
    say(" Moto IMPRES USB bridge")
    say(" Інтерфейс:  " + url)
    if note:
        say(" Увага:      порт %d зайнятий — узяв %d" % (note, port))
    say(" COM-порти:  " + (", ".join(p["port"] for p in ports) or "(не знайдено)"))
    if DEFAULT_PORT[0]:
        say(" За замовч.: " + DEFAULT_PORT[0])
    say(" Натисніть «Підключити» у браузері. Ctrl+C — вихід.")
    say("=" * 52)
    if not a.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
