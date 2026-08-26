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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ⚑ ТОЙ САМИЙ МОДУЛЬ, ЩО Й У ПРОГРАМИ. Підпис порту, розрізнення вхідного та
#  вихідного Bluetooth-каналів, порядок опитування й ознака «це наш прилад» —
#  усе це вже написане й покрите тестом у moto_models.py. Друга копія тут
#  розійшлася б із першою на першій же правці, а міст саме та поверхня, де
#  розходження помічають останнім.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import moto_models as mm

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
      $('dot').classList.remove('on'); $('btnConn').textContent='\\uD83D\\uDD0C \\u041F\\u0456\\u0434\\u043A\\u043B\\u044E\\u0447\\u0438\\u0442\\u0438';
      $('st').textContent='\\u041D\\u0435 \\u043F\\u0456\\u0434\\u043A\\u043B\\u044E\\u0447\\u0435\\u043D\\u043E'; return; }
    $('st').textContent='\\u0412\\u0456\\u0434\\u043A\\u0440\\u0438\\u0442\\u0442\\u044F \\u043F\\u043E\\u0440\\u0442\\u0443...';
    try{
      var sel=$('bridgePort'); var pq=(sel&&sel.value)?('?port='+encodeURIComponent(sel.value)):'';
      var r=await (await fetch('/open'+pq)).json();
      if(r.ok){ connected=true; $('dot').classList.add('on'); $('btnConn').textContent='\\u23CF \\u0412\\u0456\\u0434\\u043A\\u043B\\u044E\\u0447\\u0438\\u0442\\u0438';
        $('st').textContent='\\u041F\\u0456\\u0434\\u043A\\u043B\\u044E\\u0447\\u0435\\u043D\\u043E ('+r.port+')';
        await refresh(); await loadTemplates(); await sndLoad(); await clLoadSamples(); await disTick(); }
      else { $('st').textContent='\\u041F\\u043E\\u043C\\u0438\\u043B\\u043A\\u0430: '+(r.err||''); }
    }catch(e){ $('st').textContent='\\u041C\\u0456\\u0441\\u0442 \\u043D\\u0435\\u0434\\u043E\\u0441\\u0442\\u0443\\u043F\\u043D\\u0438\\u0439: '+e.message; }
  };
  // Показати випадаючий список COM-портів поруч із кнопкою «Підключити».
  fetch('/ports').then(function(r){return r.json();}).then(function(d){
    var ps=(d&&d.ports)||[]; if(!ps.length) return;
    var sel=document.createElement('select'); sel.id='bridgePort';
    sel.style.cssText='margin-right:8px;background:#0e1218;color:#e7ecf3;border:1px solid #2a323f;border-radius:8px;padding:6px';
    ps.forEach(function(p){var o=document.createElement('option');o.value=p.port;o.textContent=p.port+(p.desc?(' — '+p.desc):'');sel.appendChild(o);});
    var btn=$('btnConn'); btn.parentNode.insertBefore(sel, btn);
    // ⚑ КНОПКА ПОШУКУ — ГОЛОВНА ДЛЯ ТОГО, ХТО НЕ ЗНАЄ НОМЕРА ПОРТУ. У системі
    //  десяток COM-портів, жоден не підписаний іменем приладу; прилад
    //  представляється сам у відповідь на PING, тож питаємо кожен.
    var fb=document.createElement('button');
    fb.textContent='\uD83D\uDD0D \u0417\u043D\u0430\u0439\u0442\u0438 \u043F\u0440\u0438\u043B\u0430\u0434';
    fb.style.cssText='margin-right:8px;background:#1b2430;color:#e7ecf3;border:1px solid #2a323f;border-radius:8px;padding:6px 10px;cursor:pointer';
    fb.onclick=async function(){
      fb.disabled=true;
      $('st').textContent='\u041F\u043E\u0448\u0443\u043A \u043F\u0440\u0438\u043B\u0430\u0434\u0443...';
      try{
        var r=await (await fetch('/find')).json();
        if(r.ok){ for(var i=0;i<sel.options.length;i++) if(sel.options[i].value===r.port) sel.selectedIndex=i;
                  if(!connected) await toggleConn(); }
        else $('st').textContent='\u041D\u0435 \u0437\u043D\u0430\u0439\u0434\u0435\u043D\u043E: '+(r.err||'');
      }catch(e){ $('st').textContent='\u041C\u0456\u0441\u0442 \u043D\u0435\u0434\u043E\u0441\u0442\u0443\u043F\u043D\u0438\u0439: '+e.message; }
      fb.disabled=false;
    };
    btn.parentNode.insertBefore(fb, btn);
  });
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

    def do_GET(self):
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

    srv = ThreadingHTTPServer(("127.0.0.1", a.http), Handler)
    url = "http://127.0.0.1:%d/" % a.http
    print("=" * 52)
    print(" Moto IMPRES USB bridge")
    print(" Інтерфейс:  " + url)
    print(" COM-порти:  " + (", ".join(p["port"] for p in ports) or "(не знайдено)"))
    if DEFAULT_PORT[0]:
        print(" За замовч.: " + DEFAULT_PORT[0])
    print(" Натисніть «Підключити» у браузері. Ctrl+C — вихід.")
    print("=" * 52)
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
