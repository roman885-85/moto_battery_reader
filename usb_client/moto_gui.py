#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Moto IMPRES USB — нативний графічний клієнт (Tkinter, без браузера).

Говорить з ESP32 напряму по COM-порту (pyserial). Не потребує Chrome/Web Serial.
Пакується в один moto_usb.exe (див. build.bat) — тоді у користувача не треба
ні Python, ні браузера.

Запуск із коду:  python moto_gui.py
Збірка .exe:     build.bat
"""
import sys, os, time, json, queue, threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext


def resource_path(name):
    """Шлях до вкладеного ресурсу і в звичайному запуску, і всередині .exe."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    try:
        tk.Tk().withdraw()
        messagebox.showerror("Немає pyserial", "Встановіть залежність:\n\npip install pyserial")
    except Exception:
        print("Потрібен pyserial:  pip install pyserial")
    sys.exit(1)


# --------------------------------------------------------------------------
# Фоновий потік послідовного обміну з портом (щоб не блокувати GUI).
# Кожна робота: ('open',port,baud) | ('close',) | ('cmd',c,timeout).
# Результат разом із токеном кладеться у results; GUI-потік їх забирає.
# --------------------------------------------------------------------------
class SerialWorker(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.ser = None
        self.jobs = queue.Queue()
        self.results = queue.Queue()
        self.running = True

    def run(self):
        while self.running:
            try:
                job = self.jobs.get(timeout=0.2)
            except queue.Empty:
                continue
            kind, args, token = job[0], job[1:-1], job[-1]
            if kind == "open":
                port, baud = args
                try:
                    if self.ser:
                        self.ser.close()
                    self.ser = serial.Serial(port, baud, timeout=0.2)
                    time.sleep(1.8)                # ESP32 перезавантажується при відкритті
                    self.ser.reset_input_buffer()
                    self.results.put((token, {"ok": True, "port": port}))
                except Exception as e:
                    self.ser = None
                    self.results.put((token, {"ok": False, "err": str(e)}))
            elif kind == "close":
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.results.put((token, {"ok": True}))
            elif kind == "cmd":
                c, timeout = args
                self.results.put((token, self._cmd(c, timeout)))

    def _cmd(self, c, timeout):
        if not self.ser:
            return {"ok": False, "err": "порт не відкрито"}
        try:
            self.ser.reset_input_buffer()
            # Великі команди (WRITE33 ~1 КБ) шлемо частинами по 200 Б із
            # мікропаузами, щоб не переповнити 256-байтний UART-буфер ESP32.
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
            return {"ok": False, "err": "таймаут"}
        except Exception as e:
            return {"ok": False, "err": str(e)}


class App:
    def __init__(self, root):
        self.root = root
        root.title("Moto IMPRES — USB")
        root.geometry("760x620")
        root.minsize(680, 560)

        self.worker = SerialWorker()
        self.worker.start()
        self._tok = 0
        self._cb = {}
        self.connected = False
        self.info = {}

        try:
            ico = resource_path("icon.ico")
            if os.path.exists(ico):
                root.iconbitmap(ico)
        except Exception:
            pass

        self._build()
        self.refresh_ports()
        self.root.after(40, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- обмін із фоновим потоком --------------------------------------
    def _submit(self, kind, *args, cb=None):
        self._tok += 1
        tok = self._tok
        if cb:
            self._cb[tok] = cb
        self.worker.jobs.put((kind,) + args + (tok,))

    def cmd(self, c, timeout=8.0, cb=None):
        self.log("> " + ("AUTH ***" if c.startswith("AUTH ") else (c[:70] + ("…" if len(c) > 70 else ""))))
        self._submit("cmd", c, timeout, cb=cb)

    def _poll(self):
        while True:
            try:
                tok, res = self.worker.results.get_nowait()
            except queue.Empty:
                break
            cb = self._cb.pop(tok, None)
            if cb:
                try:
                    cb(res)
                except Exception as e:
                    self.log("! UI: " + str(e))
        self.root.after(40, self._poll)

    # ---- побудова інтерфейсу -------------------------------------------
    def _build(self):
        top = ttk.Frame(self.root, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="COM-порт:").pack(side="left")
        self.cbPort = ttk.Combobox(top, width=28, state="readonly")
        self.cbPort.pack(side="left", padx=4)
        ttk.Button(top, text="⟳", width=3, command=self.refresh_ports).pack(side="left")
        self.btnConn = ttk.Button(top, text="🔌 Підключити", command=self.toggle_conn)
        self.btnConn.pack(side="left", padx=6)
        ttk.Label(top, text="Пароль (опц.):").pack(side="left")
        self.pw = ttk.Entry(top, width=12, show="•")
        self.pw.pack(side="left", padx=2)
        self.lblStatus = ttk.Label(self.root, text="Не підключено", foreground="#a00", padding=(8, 0))
        self.lblStatus.pack(fill="x")

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=6, pady=6)
        self.tabOv = ttk.Frame(nb, padding=8); nb.add(self.tabOv, text="Огляд")
        self.tabData = ttk.Frame(nb, padding=8); nb.add(self.tabData, text="Дані")
        self.tabFw = ttk.Frame(nb, padding=8); nb.add(self.tabFw, text="Прошивка")
        self.tabHex = ttk.Frame(nb, padding=8); nb.add(self.tabHex, text="Редактор")
        self.tabLog = ttk.Frame(nb, padding=8); nb.add(self.tabLog, text="Журнал")

        self._build_overview()
        self._build_data()
        self._build_fw()
        self._build_hex()
        self._build_log()

    def _kv(self, parent, label, r):
        ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w", pady=2)
        v = ttk.Label(parent, text="—", font=("Segoe UI", 10, "bold"))
        v.grid(row=r, column=1, sticky="w", padx=10)
        return v

    def _build_overview(self):
        f = self.tabOv
        ttk.Button(f, text="🔍 Зчитати АКБ", command=self.do_read).grid(row=0, column=0, sticky="w", pady=4)
        ttk.Button(f, text="🔄 Оновити", command=self.refresh).grid(row=0, column=1, sticky="w", padx=6)
        box = ttk.LabelFrame(f, text="Стан", padding=8); box.grid(row=1, column=0, columnspan=2, sticky="we", pady=8)
        self.ovCharge = self._kv(box, "Заряд:", 0)
        self.ovVolt = self._kv(box, "Напруга:", 1)
        self.ovTemp = self._kv(box, "Температура:", 2)
        self.ovModel = self._kv(box, "Модель:", 3)
        self.ovCap = self._kv(box, "Ємність / знос:", 4)
        self.ovCyc = self._kv(box, "Циклів:", 5)
        self.ovAuth = self._kv(box, "Справжність:", 6)
        self.ovInteg = self._kv(box, "Цілісність:", 7)

    def _build_data(self):
        f = self.tabData
        box = ttk.LabelFrame(f, text="DS2438 / ідентичність", padding=8); box.pack(fill="x")
        self.dSerial = self._kv(box, "Серійний (ROM):", 0)
        self.dModel = self._kv(box, "Модель:", 1)
        self.dFirst = self._kv(box, "Перше використання (≈):", 8)
        self.dEtm = self._kv(box, "Наробіток (ETM):", 9)
        self.dV = self._kv(box, "Напруга:", 2)
        self.dI = self._kv(box, "Струм:", 3)
        self.dT = self._kv(box, "Температура:", 4)
        self.dICA = self._kv(box, "Залишок ICA:", 5)
        self.dCCA = self._kv(box, "Заряджено CCA:", 6)
        self.dDCA = self._kv(box, "Розряджено DCA:", 7)
        ttk.Label(f, text="Дамп DS2433 (512 Б):").pack(anchor="w", pady=(8, 0))
        self.tx33 = scrolledtext.ScrolledText(f, height=6, font=("Consolas", 8)); self.tx33.pack(fill="both", expand=True)
        ttk.Label(f, text="Дамп DS2438 (64 Б):").pack(anchor="w", pady=(6, 0))
        self.tx38 = scrolledtext.ScrolledText(f, height=3, font=("Consolas", 8)); self.tx38.pack(fill="x")

    def _row(self, parent, text, widget_builder):
        fr = ttk.Frame(parent); fr.pack(fill="x", pady=3)
        ttk.Label(fr, text=text, width=22).pack(side="left")
        return widget_builder(fr)

    def _build_fw(self):
        f = ttk.Frame(self.tabFw)
        canvas = tk.Canvas(self.tabFw, highlightthickness=0)
        sb = ttk.Scrollbar(self.tabFw, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw", width=700)
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True); sb.pack(side="right", fill="y")
        p = inner

        b1 = ttk.LabelFrame(p, text="Резервна копія (спочатку!)", padding=8); b1.pack(fill="x", pady=4)
        ttk.Button(b1, text="🔍 Зчитати АКБ", command=self.do_read).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2433", command=lambda: self.save_dump("GET33", 512, "ds2433.bin")).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2438", command=lambda: self.save_dump("GET38", 64, "ds2438.bin")).pack(side="left", padx=3)

        b2 = ttk.LabelFrame(p, text="Обслуговування (безпечно для ідентичності)", padding=8); b2.pack(fill="x", pady=4)
        ttk.Button(b2, text="♻️ Скидання лічильників", command=lambda: self.simple_op("RESET", "Скинути лічильники і записати у чіп?")).pack(side="left", padx=3)
        ttk.Button(b2, text="🛠 Ремонт цілісності", command=lambda: self.simple_op("REPAIR", "Відновити цілісність і записати?")).pack(side="left", padx=3)

        b2b = ttk.LabelFrame(p, text="Ремонт після заміни елементів (→ калібрування на ЗП)", padding=8); b2b.pack(fill="x", pady=4)
        ttk.Label(b2b, text="Для АКБ, яку рація бачить «невідома» після заміни банок: стирає старе\n"
                            "learned-калібрування (0x0A) + обнуляє лічильники. Далі — калібрування на IMPRES-ЗП.",
                  justify="left").pack(anchor="w")
        ttk.Button(b2b, text="🔧 Підготувати до калібрування",
                   command=lambda: self.simple_op("RECAL", "Стерти старе learned-калібрування і обнулити лічильники?\nМодель/крива лишаються. Далі — калібрування на IMPRES-ЗП.", 25.0)).pack(anchor="w", pady=3)

        b3 = ttk.LabelFrame(p, text="Модель (ручний запис)", padding=8); b3.pack(fill="x", pady=4)
        self.eModel = self._row(b3, "Модель (3–9, A–Z0–9):", lambda fr: self._entry(fr, 12))
        ttk.Button(b3, text="💾 Записати модель", command=self.set_model).pack(anchor="w", pady=2)

        b4 = ttk.LabelFrame(p, text="🆕 Новий акумулятор (порожній чип)", padding=8); b4.pack(fill="x", pady=4)
        self.cbInit = self._row(b4, "Модель-еталон:", lambda fr: self._combo(fr, 18))
        self.eInitMah = self._row(b4, "Ємність, мА·год:", lambda fr: self._entry(fr, 10, "2500"))
        ttk.Button(b4, text="🆕 Записати новий АКБ (DS2433+DS2438)", command=self.init_battery).pack(anchor="w", pady=2)

        b5 = ttk.LabelFrame(p, text="Заряд / здоров'я", padding=8); b5.pack(fill="x", pady=4)
        self.eMah = self._row(b5, "Заряд, мА·год:", lambda fr: self._entry(fr, 10, "0"))
        ttk.Button(b5, text="💾 Записати мА·год", command=self.set_mah).pack(anchor="w", pady=2)
        self.eCap = self._row(b5, "Ємність/здоров'я, %:", lambda fr: self._entry(fr, 10, "100"))
        ttk.Button(b5, text="💾 Записати %", command=self.set_cap).pack(anchor="w", pady=2)

        b5c = ttk.LabelFrame(p, text="Дата першого використання (рація рахує як «час − ETM»)", padding=8); b5c.pack(fill="x", pady=4)
        self.eEtmDate = self._row(b5c, "Дата (YYYY-MM-DD):", lambda fr: self._entry(fr, 12))
        ttk.Button(b5c, text="📅 Записати дату (ETM)", command=self.set_etm).pack(anchor="w", pady=2)

        b6 = ttk.LabelFrame(p, text="⛔ Небезпечна зона (незворотно!)", padding=8); b6.pack(fill="x", pady=4)
        ttk.Button(b6, text="🧹 Очистити дані (лишити ID/калібр.)", command=lambda: self.simple_op("CLEAN", "Стерти всі дані використання, лишивши ID/калібрування?")).pack(anchor="w", pady=2)
        rf = ttk.Frame(b6); rf.pack(fill="x", pady=2)
        ttk.Button(rf, text="📤 Записати DS2433 з .bin (512 Б)", command=lambda: self.write_file(512, "WRITE33")).pack(side="left", padx=3)
        ttk.Button(rf, text="🔬 DS2438 з .bin (64 Б)", command=lambda: self.write_file(64, "WRITE38")).pack(side="left", padx=3)
        ttk.Button(b6, text="🔥 ПОВНЕ стирання DS2433", command=self.wipe33).pack(anchor="w", pady=2)

        b7 = ttk.LabelFrame(p, text="Пристрій", padding=8); b7.pack(fill="x", pady=4)
        ttk.Button(b7, text="🔁 Перезавантажити ESP32", command=self.reboot).pack(side="left", padx=3)

    def _build_hex(self):
        f = self.tabHex
        mono = ("Consolas", 10)
        bar = ttk.Frame(f); bar.pack(fill="x")
        ttk.Label(bar, text="Мікросхема:").pack(side="left")
        self.hxTarget = ttk.Combobox(bar, width=16, state="readonly", values=["DS2433 (512 Б)", "DS2438 (64 Б)"])
        self.hxTarget.current(0); self.hxTarget.pack(side="left", padx=4)
        ttk.Button(bar, text="↻ Завантажити", command=self.hx_load).pack(side="left", padx=3)
        self.hxFix = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Автовиправлення (DS2433)", variable=self.hxFix).pack(side="left", padx=8)
        ttk.Button(bar, text="⚡ Записати байти", command=self.hx_write).pack(side="left", padx=3)

        # Заголовок колонок (як у HxD): Offset | 16 байт (розбито по 8) | текст
        hdr = tk.Frame(f); hdr.pack(fill="x", pady=(6, 0))
        tk.Label(hdr, text="Offset(h) ", width=10, font=mono, anchor="w", fg="#2a7d4f").pack(side="left")
        tk.Label(hdr, text="00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F ",
                 font=mono, anchor="w", fg="#3a6d9a").pack(side="left")
        tk.Label(hdr, text=" Декодований текст", font=mono, anchor="w", fg="#7a5").pack(side="left")

        body = tk.Frame(f); body.pack(fill="both", expand=True)
        self.hxSb = ttk.Scrollbar(body, orient="vertical", command=self._hx_yview)
        self.hxSb.pack(side="right", fill="y")
        self.hxGut = tk.Text(body, width=10, font=mono, wrap="none", padx=5, spacing1=0,
                             bg="#eef1f4", fg="#2a6", relief="flat", state="disabled", cursor="arrow",
                             takefocus=0)
        self.hxGut.pack(side="left", fill="y")
        self.hxHex = tk.Text(body, width=50, font=mono, wrap="none", undo=True, padx=5,
                             relief="flat", bg="#ffffff", insertbackground="#111")
        self.hxHex.pack(side="left", fill="both", expand=True)
        self.hxAsc = tk.Text(body, width=18, font=mono, wrap="none", padx=5,
                             bg="#eef1f4", fg="#357", relief="flat", state="disabled", cursor="arrow",
                             takefocus=0)
        self.hxAsc.pack(side="left", fill="y")
        # Синхронне вертикальне прокручування трьох панелей: тягне лише hxHex,
        # решта слідують; смуга/колесо рухають усі три.
        self.hxHex.config(yscrollcommand=self._hx_on_scroll)
        for w in (self.hxGut, self.hxHex, self.hxAsc):
            w.bind("<MouseWheel>", self._hx_wheel)
            w.bind("<Button-4>", self._hx_wheel)
            w.bind("<Button-5>", self._hx_wheel)
        self.hxHex.bind("<KeyRelease>", self._hx_aux)

    # ---- синхронізація прокручування hex-панелей -----------------------
    def _hx_yview(self, *args):
        for w in (self.hxGut, self.hxHex, self.hxAsc):
            w.yview(*args)

    def _hx_on_scroll(self, first, last):
        self.hxSb.set(first, last)
        self.hxGut.yview_moveto(first)
        self.hxAsc.yview_moveto(first)

    def _hx_wheel(self, e):
        step = -3 if (getattr(e, "delta", 0) > 0 or getattr(e, "num", 0) == 4) else 3
        self.hxHex.yview_scroll(step, "units")
        return "break"

    def _set_ro(self, w, text):
        w.config(state="normal")
        w.delete("1.0", "end")
        w.insert("1.0", text)
        w.config(state="disabled")

    # Перерахунок колонок Offset і ASCII з поточних байтів hex-панелі (як у HxD).
    def _hx_aux(self, *_):
        import re
        hexs = re.sub(r"[^0-9a-fA-F]", "", self.hxHex.get("1.0", "end"))
        n = len(hexs) // 2
        guts, ascs = [], []
        for i in range(0, max(n, 1), 16):
            guts.append("%08X" % i)
            row = ""
            for j in range(i, min(i + 16, n)):
                b = int(hexs[j * 2:j * 2 + 2], 16)
                row += chr(b) if 32 <= b < 127 else "."
            ascs.append((row[:8] + " " + row[8:]) if len(row) > 8 else row)
        if n == 0:
            guts, ascs = [], []
        self._set_ro(self.hxGut, "\n".join(guts))
        self._set_ro(self.hxAsc, "\n".join(ascs))

    def _build_log(self):
        self.txLog = scrolledtext.ScrolledText(self.tabLog, font=("Consolas", 8)); self.txLog.pack(fill="both", expand=True)

    def _entry(self, parent, width, default=""):
        e = ttk.Entry(parent, width=width); e.pack(side="left")
        if default:
            e.insert(0, default)
        return e

    def _combo(self, parent, width):
        c = ttk.Combobox(parent, width=width, state="readonly"); c.pack(side="left")
        return c

    # ---- утиліти -------------------------------------------------------
    def log(self, s):
        self.txLog.insert("end", s + "\n"); self.txLog.see("end")

    def status(self, s, ok=None):
        self.lblStatus.config(text=s, foreground=("#080" if ok else ("#a00" if ok is False else "#333")))

    def need_conn(self):
        if not self.connected:
            self.status("Спочатку підключіться", False)
            return False
        return True

    def maybe_auth(self, then):
        """Пароль опційний: якщо введено — надішлемо AUTH (для статусу), тоді then()."""
        p = self.pw.get().strip()
        if p:
            self.cmd("AUTH " + p, 5.0, cb=lambda r: then())
        else:
            then()

    def refresh_ports(self):
        ports = [f"{p.device} — {(p.description or '')[:28]}" for p in serial.tools.list_ports.comports()]
        self._portmap = {}
        vals = []
        for p in serial.tools.list_ports.comports():
            label = f"{p.device} — {(p.description or '')[:28]}"
            self._portmap[label] = p.device
            vals.append(label)
        self.cbPort["values"] = vals
        if vals and not self.cbPort.get():
            self.cbPort.current(0)

    # ---- підключення ---------------------------------------------------
    def toggle_conn(self):
        if self.connected:
            self._submit("close", cb=lambda r: self._set_disconnected())
        else:
            label = self.cbPort.get()
            if not label:
                self.status("Оберіть COM-порт", False); return
            port = self._portmap.get(label, label.split(" ")[0])
            self.status("Відкриття порту (скидання ESP ~2 с)...")
            self.btnConn.config(state="disabled")
            self._submit("open", port, 115200, cb=self._on_open)

    def _on_open(self, r):
        self.btnConn.config(state="normal")
        if r.get("ok"):
            self.connected = True
            self.btnConn.config(text="⏏ Відключити")
            self.status("Підключено (" + r.get("port", "") + ")", True)
            self.cmd("PING", 3.0, cb=lambda _: (self.load_templates(), self.refresh()))
        else:
            self.status("Помилка порту: " + r.get("err", ""), False)

    def _set_disconnected(self):
        self.connected = False
        self.btnConn.config(text="🔌 Підключити")
        self.status("Не підключено", False)

    def _on_close(self):
        try:
            self.worker.running = False
        except Exception:
            pass
        self.root.destroy()

    # ---- операції читання ----------------------------------------------
    def do_read(self):
        if not self.need_conn():
            return
        self.status("Зчитування...")
        self.cmd("READ", 15.0, cb=lambda r: (self.status("Зчитано" if r.get("ok") else "Помилка читання", r.get("ok")), self.refresh()))

    def refresh(self, *_):
        if not self.connected:
            return
        self.cmd("INFO", 15.0, cb=self._apply_info)

    def _apply_info(self, d):
        if not d.get("ok"):
            return
        self.info = d
        ch = d.get("charge")
        self.ovCharge.config(text=(f"{ch}%  ({d.get('chargeSrc','')})" if isinstance(ch, int) and ch >= 0 else "—"))
        v = d.get("voltage"); t = d.get("temperature")
        self.ovVolt.config(text=(f"{v:.2f} В" if isinstance(v, (int, float)) else "—"))
        self.ovTemp.config(text=(f"{t:.1f} °C" if isinstance(t, (int, float)) else "—"))
        self.dV.config(text=(f"{v:.2f} В" if isinstance(v, (int, float)) else "—"))
        self.dT.config(text=(f"{t:.1f} °C" if isinstance(t, (int, float)) else "—"))
        m = d.get("model") or "—"
        self.ovModel.config(text=m); self.dModel.config(text=m)
        cap = d.get("capacity"); wear = d.get("wear")
        self.ovCap.config(text=(f"{cap}% / знос {wear}%" if isinstance(cap, int) and cap >= 0 else "—"))
        if d.get("ccaCycles") is not None:
            self.ovCyc.config(text=f"{d['ccaCycles']} зар. / {d['dcaCycles']} розр.")
        if "genuine" in d:
            self.ovAuth.config(text=("OK" if d["genuine"] else "РИЗИК (" + str(d.get("authReason", "")) + ")"))
        if "headerOk" in d:
            self.ovInteg.config(text=("заголовок " + ("OK" if d["headerOk"] else "✗") + " · дзеркало " + ("OK" if d.get("mirrorOk") else "✗")))
        self.dSerial.config(text=d.get("serial") or "—")
        etm = d.get("etmSec")
        if isinstance(etm, int):
            import datetime
            YS = 31557600  # 365.25 дн
            days = etm // 86400
            first = datetime.date.today() - datetime.timedelta(seconds=etm)  # так рахує рація
            if etm > 34 * YS:  # нереально як напрацювання -> у полі зашита Unix-дата
                as_unix = datetime.datetime.utcfromtimestamp(etm).date()
                self.dEtm.config(text=f"⚠ {days // 365} р ({etm} с) — некоректно", foreground="#c0392b")
                self.dFirst.config(text=f"⚠ рація покаже {first.isoformat()} (поле=Unix {as_unix.isoformat()})",
                                   foreground="#c0392b")
            else:
                self.dEtm.config(text=f"{days // 365} р {days % 365} дн ({etm} с)", foreground="")
                self.dFirst.config(text=first.isoformat(), foreground="")
            if hasattr(self, "eEtmDate") and not self.eEtmDate.get():
                self.eEtmDate.delete(0, "end"); self.eEtmDate.insert(0, first.isoformat())
        self.dI.config(text=(str(d.get("currentMa")) + " мА") if d.get("currentMa") is not None else "—")
        self.dICA.config(text=(f"≈{d.get('icaMah')} мА·год (raw {d.get('ica')})") if d.get("icaMah") is not None else "—")
        self.dCCA.config(text=(f"{d.get('ccaCycles')} ц (≈{d.get('ccaMah')} мА·год)") if d.get("ccaMah") is not None else "—")
        self.dDCA.config(text=(f"{d.get('dcaCycles')} ц (≈{d.get('dcaMah')} мА·год)") if d.get("dcaMah") is not None else "—")
        if isinstance(cap, int) and cap >= 0:
            self._set_entry(self.eCap, str(cap))
        if d.get("icaMah") is not None:
            self._set_entry(self.eMah, str(d.get("icaMah")))
        self._set_text(self.tx33, d.get("hex33", ""))
        self._set_text(self.tx38, d.get("hex38", ""))

    def _set_entry(self, e, val):
        e.delete(0, "end"); e.insert(0, val)

    def _set_text(self, t, s):
        t.delete("1.0", "end"); t.insert("1.0", s)

    # ---- операції запису -----------------------------------------------
    def simple_op(self, command, confirm, timeout=15.0):
        if not self.need_conn():
            return
        if not messagebox.askyesno("Підтвердження", confirm):
            return
        self.maybe_auth(lambda: (self.status("Виконання..."),
                                 self.cmd(command, timeout, cb=lambda r: self._after_write(r))))

    def _after_write(self, r, okmsg="✅ Готово"):
        self.status(okmsg if r.get("ok") else ("Помилка: " + str(r.get("err", ""))), r.get("ok"))
        if not r.get("ok"):
            messagebox.showerror("Помилка", str(r.get("err", "")))
        self.refresh()

    def set_model(self):
        if not self.need_conn():
            return
        m = self.eModel.get().strip().upper()
        import re
        if not re.match(r"^[A-Z0-9]{3,9}$", m):
            messagebox.showwarning("Модель", "3–9 символів A–Z / 0–9"); return
        if not messagebox.askyesno("Запис моделі", f"Записати модель «{m}» у DS2433?"):
            return
        self.maybe_auth(lambda: self.cmd("SETMODEL " + m, 15.0, cb=lambda r: self._after_write(r, "✅ Модель записано")))

    def init_battery(self):
        if not self.need_conn():
            return
        model = self.cbInit.get()
        try:
            mah = int(self.eInitMah.get())
        except ValueError:
            messagebox.showwarning("Ємність", "Вкажіть ціле число мА·год"); return
        if not model:
            messagebox.showwarning("Модель", "Оберіть модель-еталон"); return
        if mah <= 0:
            messagebox.showwarning("Ємність", "Ємність має бути > 0"); return
        if not messagebox.askyesno("Новий АКБ", f"Ініціалізувати чип як НОВИЙ {model} ({mah} мА·год)?\nПерезапише ОБИДВІ мікросхеми. Лише для порожнього чипа."):
            return
        self.maybe_auth(lambda: (self.status("Запис нового АКБ..."),
                                 self.cmd(f"INITBAT {model} {mah}", 25.0, cb=lambda r: self._after_write(r, f"✅ Новий {model} записано"))))

    def set_mah(self):
        if not self.need_conn():
            return
        try:
            v = int(self.eMah.get())
        except ValueError:
            messagebox.showwarning("мА·год", "Вкажіть число"); return
        if not messagebox.askyesno("Заряд", f"Записати залишкову ємність {v} мА·год?"):
            return
        self.maybe_auth(lambda: self.cmd(f"SETMAH {v}", 15.0, cb=lambda r: self._after_write(r, "✅ Записано")))

    def set_cap(self):
        if not self.need_conn():
            return
        try:
            v = int(self.eCap.get())
        except ValueError:
            messagebox.showwarning("%", "Вкажіть 0..100"); return
        if v < 0 or v > 100:
            messagebox.showwarning("%", "0..100"); return
        if not messagebox.askyesno("Здоров'я", f"Записати ємність {v}%?"):
            return
        self.maybe_auth(lambda: self.cmd(f"SETCAP {v}", 15.0, cb=lambda r: self._after_write(r, "✅ Записано")))

    def set_etm(self):
        if not self.need_conn():
            return
        import datetime
        try:
            y, m, dd = [int(x) for x in self.eEtmDate.get().strip().split("-")]
            target = datetime.date(y, m, dd)
        except Exception:
            messagebox.showwarning("Дата", "Формат: YYYY-MM-DD"); return
        sec = int((datetime.date.today() - target).total_seconds())
        if sec < 0:
            messagebox.showwarning("Дата", "Дата в майбутньому"); return
        if sec > 0xFFFFFFFF:
            sec = 0xFFFFFFFF
        if not messagebox.askyesno("Дата", f"Записати дату «{target.isoformat()}» (ETM={sec} с)?\nПеревірте на рації."):
            return
        self.maybe_auth(lambda: self.cmd(f"SETETM {sec}", 15.0, cb=lambda r: self._after_write(r, "✅ Дату записано")))

    def wipe33(self):
        if not self.need_conn():
            return
        if not messagebox.askyesno("ПОВНЕ стирання", "🔥 Стерти ВЕСЬ DS2433 у 0xFF?\nМодель/ID/калібрування зникнуть, АКБ не працюватиме до запису еталона.\nВи зробили копію?"):
            return
        if not messagebox.askyesno("Підтвердження", "Останнє попередження: стерти DS2433 ПОВНІСТЮ?"):
            return
        self.maybe_auth(lambda: (self.status("Стирання..."),
                                 self.cmd("WIPE33", 25.0, cb=lambda r: self._after_write(r, "✅ Стерто. Запишіть еталон."))))

    def reboot(self):
        if not self.need_conn():
            return
        if not messagebox.askyesno("Перезавантаження", "Перезавантажити ESP32? Порт відключиться."):
            return
        self.maybe_auth(lambda: self.cmd("REBOOT", 3.0, cb=lambda r: self._submit("close", cb=lambda _: self._set_disconnected())))

    # ---- шаблони / файли -----------------------------------------------
    def load_templates(self, *_):
        self.cmd("TEMPLATES", 5.0, cb=self._apply_templates)

    def _apply_templates(self, r):
        models = r.get("models", []) if r.get("ok") else []
        self.cbInit["values"] = models
        if models and not self.cbInit.get():
            self.cbInit.current(0)

    def save_dump(self, getcmd, size, default):
        if not self.need_conn():
            return
        def done(r):
            if not r.get("ok") or not r.get("hex"):
                messagebox.showwarning("Немає дампа", "Спочатку зчитайте АКБ"); return
            try:
                data = bytes(int(x, 16) for x in r["hex"].split())
            except Exception as e:
                messagebox.showerror("Дамп", str(e)); return
            if len(data) != size:
                messagebox.showwarning("Дамп", f"Очікувалось {size} Б, отримано {len(data)}"); return
            fn = filedialog.asksaveasfilename(defaultextension=".bin", initialfile=default,
                                              filetypes=[("BIN", "*.bin")])
            if fn:
                open(fn, "wb").write(data)
                self.status("Збережено: " + fn, True)
        self.cmd(getcmd, 15.0, cb=done)

    def write_file(self, size, command):
        if not self.need_conn():
            return
        fn = filedialog.askopenfilename(filetypes=[("BIN", "*.bin"), ("All", "*.*")])
        if not fn:
            return
        data = open(fn, "rb").read()
        if len(data) != size:
            messagebox.showwarning("Файл", f"Файл має бути {size} байт (зараз {len(data)})"); return
        if not messagebox.askyesno("Запис", f"⚡ Запис {size} байт НЕЗВОРОТНІЙ. Продовжити?"):
            return
        hexs = "".join(f"{b:02x}" for b in data)
        self.maybe_auth(lambda: (self.status("Запис..."),
                                 self.cmd(command + " " + hexs, 25.0, cb=lambda r: self._after_write(r, "✅ Записано"))))

    # ---- редактор байтів -----------------------------------------------
    def hx_load(self):
        if not self.need_conn():
            return
        is38 = self.hxTarget.current() == 1
        self.cmd("GET38" if is38 else "GET33", 15.0, cb=self._hx_show)

    def _hx_show(self, r):
        if not r.get("ok") or not r.get("hex"):
            messagebox.showwarning("Дамп", "Зчитайте АКБ"); return
        parts = r["hex"].split()
        lines = []
        for i in range(0, len(parts), 16):
            row = parts[i:i + 16]
            left = " ".join(row[:8])
            right = " ".join(row[8:16])
            lines.append((left + "  " + right).rstrip())
        self.hxHex.delete("1.0", "end")
        self.hxHex.insert("1.0", "\n".join(lines))
        self._hx_aux()

    def hx_write(self):
        if not self.need_conn():
            return
        is38 = self.hxTarget.current() == 1
        import re
        hexs = re.sub(r"[^0-9a-fA-F]", "", self.hxHex.get("1.0", "end"))
        need = (64 if is38 else 512) * 2
        if len(hexs) != need:
            messagebox.showwarning("Байти", f"Треба {need // 2} байт, зараз {len(hexs) // 2}"); return
        if not messagebox.askyesno("Запис", f"⚡ Запис {need // 2} байт НЕЗВОРОТНІЙ. Продовжити?"):
            return
        if is38:
            command = "WRITE38"
        else:
            command = "WRITEFIX33" if self.hxFix.get() else "WRITE33"
        self.maybe_auth(lambda: (self.status("Запис байтів..."),
                                 self.cmd(command + " " + hexs, 25.0, cb=lambda r: self._after_write(r, "✅ Записано"))))


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
