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
import sys, os, time, json, math, queue, threading
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
            # Великі команди (WRITE33 ~1 КБ) шлемо ДРІБНИМИ частинами з паузами,
            # щоб 256-байтний UART-буфер ESP32 встигав спорожнюватись у loop()
            # (інакше частина байтів губиться -> "need 512 bytes" / помилка запису).
            data = (c + "\n").encode("utf-8")
            if len(data) <= 96:
                self.ser.write(data)
                self.ser.flush()
            else:
                for i in range(0, len(data), 96):
                    self.ser.write(data[i:i + 96])
                    self.ser.flush()
                    time.sleep(0.015)      # 96 Б ≈ 8 мс передачі + запас на злив FIFO
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


class DischargeMonitor(tk.Canvas):
    """Панель процесу розряду — той самий вигляд, що у веб-версії пристрою.

    Розряд триває годинами, тож дивитись доводиться довго: рядок тексту для
    цього не годиться. Тут пульсуючий індикатор стану, смуга прогресу з
    «течією» (єдина ознака, що процес живий: між опитуваннями пристрою числа
    стоять на місці), графік напруги за ВЕСЬ сеанс, коридор уставки струму,
    шпаруватість ключа й плитки показань.

    Малює себе сама раз на 80 мс (анімація й рівний хід годинника), дані
    отримує ззовні через update_state() — опитування живе в App.
    """
    W, H = 720, 358
    PAD = 14

    def __init__(self, master):
        super().__init__(master, width=self.W, height=self.H, bg=MIL["field"],
                         highlightthickness=1, highlightbackground=MIL["line"])
        self.d = None            # останній стан із пристрою
        self.at = 0.0            # коли він прийшов — щоб годинник ішов рівно
        self.hist = []           # [(t, mv)] — крива напруги за весь сеанс
        self.step = 10           # поточний крок між точками кривої, с
        self.phase = 0.0
        self._alive = True
        self._tick()

    def destroy(self):
        self._alive = False
        super().destroy()

    def reset_history(self):
        self.hist = []; self.step = 10

    def update_state(self, d):
        if d and d.get("state") == "run":
            t, mv = d.get("elapsedS", 0), d.get("mv", 0)
            # Час пішов назад — почався НОВИЙ сеанс, стару криву не склеюємо.
            if self.hist and t < self.hist[-1][0]:
                self.reset_history()
            # Точки не викидаються з початку: коли їх забагато, ряд
            # проріджується вдвічі, а крок подвоюється — так уся крива
            # лишається на екрані незалежно від тривалості розряду.
            if not self.hist or t - self.hist[-1][0] >= self.step:
                self.hist.append((t, mv))
                if len(self.hist) > 300:
                    self.hist = self.hist[::2]; self.step *= 2
        self.d = d
        self.at = time.time()

    # ---- допоміжне малювання -------------------------------------------
    def _bar(self, x, y, w, h, frac, color, striped=False):
        self.create_rectangle(x, y, x + w, y + h, fill="#0e1108", outline=MIL["line"])
        fw = max(0, min(1.0, frac)) * (w - 2)
        if fw <= 0:
            return
        self.create_rectangle(x + 1, y + 1, x + 1 + fw, y + h - 1, fill=color, outline="")
        if not striped:
            return
        # «Течія»: похилі смужки, що біжать заливкою. Обрізаємо їх по краю
        # заливки вручну — у Tk немає відсікання області.
        off = (self.phase * 26) % 16
        sx = x + 1 - h
        while sx < x + 1 + fw:
            x0, x1 = sx + off, sx + off + h
            if x1 > x + 1 and x0 < x + 1 + fw:
                self.create_line(max(x0, x + 1), y + h - 1, min(x1, x + 1 + fw), y + 1,
                                 fill="#e7e3d2", width=3, stipple="gray25")
            sx += 16

    def _tile(self, x, y, w, h, label, value, accent=False):
        self.create_rectangle(x, y, x + w, y + h, fill=MIL["bg"],
                              outline=MIL["olive"] if accent else MIL["line"])
        self.create_text(x + 8, y + 6, text=label, anchor="nw", fill=MIL["mut"],
                         font=("Segoe UI", 8))
        self.create_text(x + 8, y + h - 8, text=value, anchor="sw", fill=MIL["fg"],
                         font=("Consolas", 13, "bold"))

    def _fmt_t(self, s):
        s = int(max(0, s))
        return "%d:%02d:%02d" % (s // 3600, (s // 60) % 60, s % 60)

    # ---- повна перемальовка --------------------------------------------
    def redraw(self):
        self.delete("all")
        P, W, H = self.PAD, self.W, self.H
        d = self.d
        state = (d or {}).get("state", "idle")
        run = state == "run"

        # Тло малюємо явно, а не покладаємось на bg полотна: так панель
        # виглядає однаково незалежно від теми і коректно лягає в postscript().
        self.create_rectangle(0, 0, W, H, fill=MIL["field"], outline="")
        # рамка стану
        edge = {"run": MIL["khaki"], "done": MIL["olive"], "abort": MIL["maroon"]}.get(state, MIL["line"])
        self.create_rectangle(1, 1, W - 2, H - 2, outline=edge, width=2)

        if not d:
            self.create_text(W // 2, H // 2, text="стан розряду не запитано",
                             fill=MIL["mut"], font=("Segoe UI", 10))
            return
        if not d.get("available"):
            self.create_text(W // 2, H // 2, text="розряд не налаштовано (LOAD_PIN у settings.h)",
                             fill=MIL["mut"], font=("Segoe UI", 10))
            return

        # --- шапка: пульсуючий вогник + стан + годинник ---
        r = 6
        if run:      # пульсація — видно, що процес іде, навіть коли числа стоять
            r = 5 + 2.2 * (0.5 + 0.5 * math.sin(self.phase * 2.2))
        dot = {"run": MIL["khaki"], "done": MIL["olive"], "abort": MIL["maroon"]}.get(state, MIL["mut"])
        self.create_oval(P + 8 - r, 20 - r, P + 8 + r, 20 + r, fill=dot, outline="")
        names = {"idle": "очікування", "run": "ІДЕ РОЗРЯД",
                 "done": "ГОТОВО — на IMPRES-ЗП", "abort": "АВАРІЯ: " + (d.get("reason") or "")}
        self.create_text(P + 22, 20, text=names.get(state, state), anchor="w",
                         fill=MIL["fg"], font=("Segoe UI", 11, "bold"))
        el = d.get("elapsedS", 0) + (time.time() - self.at if run else 0)
        self.create_text(W - P, 20, text=self._fmt_t(el), anchor="e",
                         fill=MIL["mut"], font=("Consolas", 11))

        # --- велика напруга ---
        mv, tgt, st0 = d.get("mv", 0), d.get("targetMv", 0), d.get("startMv", 0)
        big = self.create_text(P, 58, text="%.2f" % (mv / 1000.0), anchor="w",
                               fill=MIL["fg"], font=("Segoe UI", 26, "bold"))
        # Підпис «В» і рядок цілі ставимо ПО ФАКТИЧНІЙ ширині числа: шрифт
        # 26 pt на різних системах міряється по-різному, і фіксовані відступи
        # то залишали діру, то налазили на цифри.
        x = self.bbox(big)[2] + 6
        self.create_text(x, 62, text="В", anchor="w", fill=MIL["mut"], font=("Segoe UI", 11))
        self.create_text(x + 22, 62, text="старт %.2f В  →  ціль %.2f В" % (st0 / 1000.0, tgt / 1000.0),
                         anchor="w", fill=MIL["mut"], font=("Segoe UI", 9))

        # --- прогрес до цілі ---
        span, done = st0 - tgt, st0 - mv
        pct = max(0, min(100, int(round(done * 100.0 / span)))) if span > 0 else 0
        self._bar(P, 80, W - 2 * P, 17, pct / 100.0, MIL["olive"], striped=run)
        self.create_text(W - P - 8, 88, text="%d %%" % pct, anchor="e",
                         fill=MIL["fg"], font=("Segoe UI", 8, "bold"))

        # --- графік напруги за весь сеанс ---
        gx, gy, gw, gh = P, 106, W - 2 * P, 66
        self.create_rectangle(gx, gy, gx + gw, gy + gh, fill="#0e1108", outline=MIL["line"])
        if len(self.hist) >= 2:
            vs = [p[1] for p in self.hist]
            lo, hi = min(vs) - 20, max(vs) + 20
            if tgt and tgt < lo:
                lo = tgt - 20            # ціль завжди в кадрі
            t0 = self.hist[0][0]; t1 = max(self.hist[-1][0], t0 + 1)
            X = lambda t: gx + 3 + (t - t0) / float(t1 - t0) * (gw - 6)
            Y = lambda v: gy + gh - 4 - (v - lo) / float(hi - lo or 1) * (gh - 8)
            pts = [(X(t), Y(v)) for t, v in self.hist]
            poly = [gx + 3, gy + gh - 1] + [c for p in pts for c in p] + [gx + gw - 3, gy + gh - 1]
            self.create_polygon(poly, fill="#232a17", outline="")
            ty = Y(tgt)
            self.create_line(gx + 2, ty, gx + gw - 2, ty, fill=MIL["maroon"], dash=(4, 4))
            self.create_line([c for p in pts for c in p], fill=MIL["khaki"], width=2)
        else:
            self.create_text(gx + gw / 2, gy + gh / 2, text="крива напруги збирається під час розряду",
                             fill=MIL["mut"], font=("Segoe UI", 8))

        # --- струм у коридорі уставки ---
        pwm = bool(d.get("pwm"))
        ma = abs(d.get("ma", 0)); setMa = d.get("setMa", 0) or 1
        lo_ma, hi_ma = d.get("bandLoMa", 0), d.get("bandHiMa", 0)
        # Шкала — до 125 % уставки: коридор займає більшу частину доріжки, а
        # вихід за нього одразу впадає в око.
        scale = max(setMa * 1.25, ma * 1.05, 1)
        self.create_text(P, 186, text="струм / уставка", anchor="w", fill=MIL["mut"], font=("Segoe UI", 8))
        right = ("уставка %d мА · пік %d мА" % (setMa, d.get("peakMa", 0))) if pwm \
                else "ШІМ недоступний — струм не обмежено"
        self.create_text(W - P, 186, text=right, anchor="e",
                         fill=(MIL["olive"] if d.get("inBand") else MIL["khaki"]) if pwm else MIL["maroon"],
                         font=("Segoe UI", 8))
        bx, by, bw = P, 196, W - 2 * P
        self._bar(bx, by, bw, 14, ma / scale, MIL["khaki"] if (not run or d.get("inBand")) else MIL["maroon"])
        # коридор — ПОВЕРХ заливки, інакше вона його перекриє
        cl = bx + 1 + (bw - 2) * lo_ma / scale
        cr = bx + 1 + (bw - 2) * min(hi_ma, scale) / scale
        self.create_rectangle(cl, by + 1, cr, by + 13, outline="#e7e3d2", width=2)
        mkx = bx + 1 + (bw - 2) * min(setMa, scale) / scale
        self.create_line(mkx, by, mkx, by + 14, fill=MIL["fg"], width=2)

        # --- шпаруватість ключа ---
        duty = d.get("duty", 0) if pwm else 100
        self.create_text(P, 222, text="шпаруватість ключа (ШІМ)", anchor="w",
                         fill=MIL["mut"], font=("Segoe UI", 8))
        self.create_text(W - P, 222, text=("%d %%" % duty) if pwm else "ключ відкрито постійно",
                         anchor="e", fill=MIL["mut"], font=("Segoe UI", 8))
        self._bar(P, 232, W - 2 * P, 14, duty / 100.0, "#4a5a38")

        # --- плитки показань: одиниці в підписі, значення — саме число ---
        tiles = [("віддано, мА·год", str(d.get("mah", 0)), True),
                 ("DCA, мА·год",     str(d.get("dcaMah", 0)), False),
                 ("струм, мА",       str(d.get("ma", 0)), False),
                 ("потужність, Вт",  str(d.get("watts", "—")), False),
                 ("температура, °C", str(d.get("tempC", "—")), False),
                 ("ICA · старт %s" % d.get("icaStart", "—"), str(d.get("ica", 0)), False),
                 ("лишилось ≈",      self._eta(d, run), False)]
        tw, gap, ty0 = (W - 2 * P - 3 * 8) / 4.0, 8, 258
        for i, (lab, val, acc) in enumerate(tiles):
            col, row = i % 4, i // 4
            self._tile(P + col * (tw + gap), ty0 + row * 48, tw, 44, lab, val, acc)

        if pwm or not run:
            return
        self.create_text(W // 2, H - 8, anchor="s", fill="#ff9b8f", font=("Segoe UI", 8),
                         text="ШІМ недоступний: ключ відкритий постійно, струм не обмежується")

    def _eta(self, d, run):
        # Оцінка з фактичного темпу за весь сеанс. Груба (наприкінці крива
        # положистіша, та й струм ми навмисне зменшуємо), тож і подана як «≈»,
        # але дає зрозуміти, чекати десять хвилин чи дві години.
        el, mv, st0, tgt = d.get("elapsedS", 0), d.get("mv", 0), d.get("startMv", 0), d.get("targetMv", 0)
        if not run or el <= 60 or st0 <= mv:
            return "—"
        left = (mv - tgt) / ((st0 - mv) / float(el))
        return self._fmt_t(left) if left > 0 else "ось-ось"

    def _tick(self):
        if not self._alive:
            return
        self.phase += 0.08
        try:
            self.redraw()
            self.after(80, self._tick)
        except tk.TclError:
            self._alive = False          # вікно закрилось під час перемальовки


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

        self._disBusy = False

        self._build()
        self.refresh_ports()
        self.root.after(40, self._poll)
        self.root.after(1000, self._dis_tick)     # стан розряду тягнеться сам
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
        self.tabWiz = ttk.Frame(nb, padding=8); nb.add(self.tabWiz, text="🧙 Майстер")
        self.tabData = ttk.Frame(nb, padding=8); nb.add(self.tabData, text="Дані")
        self.tabFw = ttk.Frame(nb, padding=8); nb.add(self.tabFw, text="🔧 Ремонт")
        self.tabHex = ttk.Frame(nb, padding=8); nb.add(self.tabHex, text="Редактор")
        self.tabLog = ttk.Frame(nb, padding=8); nb.add(self.tabLog, text="Журнал")

        self._build_overview()
        self._build_wizard()
        self._build_data()
        self._build_fw()
        self._build_hex()
        self._build_log()

    def _scroll_area(self, tab):
        """Прокручувана область у вкладці (щоб кнопки внизу не ховались за краєм).
        Прокрутка коліщатком миші активна, поки курсор над цією вкладкою."""
        canvas = tk.Canvas(tab, highlightthickness=0, bg=MIL["bg"])
        sb = ttk.Scrollbar(tab, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        win = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(win, width=e.width))
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True); sb.pack(side="right", fill="y")

        def _wheel(e):
            d = -1 if (getattr(e, "delta", 0) > 0 or getattr(e, "num", 0) == 4) else 1
            canvas.yview_scroll(d * 3, "units")
        canvas.bind("<Enter>", lambda e: (canvas.bind_all("<MouseWheel>", _wheel),
                                          canvas.bind_all("<Button-4>", _wheel),
                                          canvas.bind_all("<Button-5>", _wheel)))
        canvas.bind("<Leave>", lambda e: (canvas.unbind_all("<MouseWheel>"),
                                          canvas.unbind_all("<Button-4>"),
                                          canvas.unbind_all("<Button-5>")))
        return inner

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

    def _build_wizard(self):
        f = self._scroll_area(self.tabWiz)
        top = ttk.Frame(f); top.pack(fill="x")
        ttk.Label(top, text="🧙 Майстер відновлення", font=("Segoe UI", 11, "bold")).pack(side="left")
        ttk.Button(top, text="🔍 Аналізувати", command=self.wiz_analyze).pack(side="right", padx=3)
        ttk.Button(top, text="↺ Скинути", command=self.wiz_reset).pack(side="right", padx=3)
        ttk.Label(f, text="Аналіз стану → проблеми → пропозиції → покрокове виконання. Багатоетапні\n"
                         "сценарії з зарядною станцією продовжуються після повернення АКБ.",
                  foreground="#b9bd86", justify="left").pack(anchor="w", pady=(2, 6))

        self.wizVerdict = ttk.Label(f, text="Натисніть «Аналізувати».", font=("Segoe UI", 10, "bold"))
        self.wizVerdict.pack(anchor="w", pady=2)
        self.wizProbFrame = ttk.LabelFrame(f, text="Проблеми", padding=6)
        self.wizPlanFrame = ttk.LabelFrame(f, text="План відновлення", padding=6)

        self.wizProgLbl = ttk.Label(self.wizPlanFrame, text="—")
        self.wizProgLbl.pack(anchor="w")
        self.wizProgBar = ttk.Progressbar(self.wizPlanFrame, maximum=100, length=320)
        self.wizProgBar.pack(anchor="w", pady=4)
        mr = ttk.Frame(self.wizPlanFrame); mr.pack(anchor="w", pady=2)
        self.wizModelLbl = ttk.Label(mr, text="Модель для відновлення:")
        self.cbWiz = ttk.Combobox(mr, width=16, state="readonly")
        self.wizStepsFrame = ttk.Frame(self.wizPlanFrame); self.wizStepsFrame.pack(fill="x", pady=4)
        self.wizBanner = ttk.Label(self.wizPlanFrame, text="", foreground="#b8860b", wraplength=460, justify="left")
        self.wizNextBtn = ttk.Button(self.wizPlanFrame, text="▶️ Виконати наступний крок", command=self.wiz_next)
        self.wizNextBtn.pack(anchor="w", pady=6)
        self._wizState = None

        # Збережені відновлення (журнали за серійником АКБ).
        self.wizJrnFrame = ttk.LabelFrame(f, text="🗂 Збережені відновлення", padding=6)
        self.wizJrnFrame.pack(fill="x", pady=6)
        ttk.Label(self.wizJrnFrame, text="Незавершені відновлення, що їх пристрій пам'ятає за серійником\n"
                                        "АКБ (напр. поки акумулятор на зарядній станції). Зайві можна видалити.",
                  foreground="#9a9c82", justify="left").pack(anchor="w")
        ttk.Button(self.wizJrnFrame, text="🔄 Оновити список", command=self.wiz_journals_load).pack(anchor="w", pady=2)
        self.wizJrnList = ttk.Frame(self.wizJrnFrame); self.wizJrnList.pack(fill="x", pady=4)

    # ---- журнали відновлення (список / видалення) ----
    def wiz_journals_load(self):
        if not self.need_conn():
            return
        self.cmd("WIZLIST", 8.0, cb=self._wiz_journals_apply)

    def _wiz_journals_apply(self, r):
        for w in self.wizJrnList.winfo_children():
            w.destroy()
        js = r.get("journals", []) if isinstance(r, dict) else []
        if not js:
            ttk.Label(self.wizJrnList, text="Немає збережених відновлень.",
                      foreground="#9a9c82").pack(anchor="w")
            return
        for j in js:
            planned = " → ".join(j.get("planned", [])) or "—"
            rem = max(0, j.get("total", 0) - j.get("done", 0))
            row = ttk.Frame(self.wizJrnList); row.pack(fill="x", pady=3)
            head = j.get("serial", "") + (" · " + j.get("model", "") if j.get("model") else "") \
                + ("   [на ЗП]" if j.get("await") else "")
            ttk.Label(row, text=head, font=("Segoe UI", 9, "bold")).pack(anchor="w")
            ttk.Label(row, text="Прогрес: %d/%d · лишилось %d" % (j.get("done", 0), j.get("total", 0), rem),
                      foreground="#9a9c82").pack(anchor="w")
            ttk.Label(row, text="Заплановано: " + planned, foreground="#8a9a5a",
                      wraplength=440, justify="left").pack(anchor="w")
            ttk.Button(row, text="🗑 Видалити з пам'яті",
                       command=lambda s=j.get("serial", ""): self.wiz_journal_delete(s)).pack(anchor="w", pady=2)

    def wiz_journal_delete(self, serial):
        if not serial or not self.need_conn():
            return
        if not messagebox.askyesno("Видалити", "Видалити збережене відновлення для %s?" % serial):
            return
        self.maybe_auth(lambda: self.cmd("WIZDEL " + serial, 8.0, cb=lambda _: self.wiz_journals_load()))

    # ---- логіка Майстра ----
    def wiz_analyze(self):
        if not self.need_conn():
            return
        self.status("Аналіз батареї…")
        self.cmd("WIZARD", 15.0, cb=self._wiz_apply)

    def _wiz_apply(self, r, msg=None):
        if not isinstance(r, dict) or not r.get("ok"):
            self.status("Помилка аналізу", False); return
        self._wizState = r
        if msg:
            self.status(msg, r.get("result", True))
        else:
            self.status("Аналіз готовий")
        # вердикт
        if not r.get("have33") and not r.get("have38"):
            self.wizVerdict.config(text="❌ Чіпи не знайдено на шині", foreground="#c0392b")
        elif r.get("healthy"):
            self.wizVerdict.config(text="✅ Батарея справна — відновлення не потрібне", foreground="#1e8449")
        else:
            self.wizVerdict.config(text="⚠️ Виявлено проблем: %d" % len(r.get("problems", [])), foreground="#b8860b")
        # проблеми
        for w in self.wizProbFrame.winfo_children():
            w.destroy()
        probs = r.get("problems", [])
        if probs:
            self.wizProbFrame.pack(fill="x", pady=6)
            for p in probs:
                ico = "⛔" if p.get("sev", 0) >= 2 else ("⚠️" if p.get("sev") == 1 else "ℹ️")
                col = "#c0392b" if p.get("sev", 0) >= 2 else ("#b8860b" if p.get("sev") == 1 else "#2471a3")
                row = ttk.Frame(self.wizProbFrame); row.pack(fill="x", pady=2)
                ttk.Label(row, text=ico, foreground=col).pack(side="left", anchor="n")
                txt = ttk.Frame(row); txt.pack(side="left", fill="x", expand=True)
                ttk.Label(txt, text=p.get("problem", ""), font=("Segoe UI", 9, "bold"),
                          wraplength=440, justify="left").pack(anchor="w")
                ttk.Label(txt, text="→ " + p.get("fix", ""), foreground="#8a9a5a",
                          wraplength=440, justify="left").pack(anchor="w")
        else:
            self.wizProbFrame.pack_forget()
        # план
        steps = r.get("steps", [])
        if steps and not r.get("healthy"):
            self.wizPlanFrame.pack(fill="x", pady=6)
            self._wiz_render_steps(r)
        else:
            self.wizPlanFrame.pack_forget()
        # оновити список збережених журналів
        self.wiz_journals_load()

    def _wiz_render_steps(self, r):
        total = r.get("total", 0); prog = r.get("progress", 0)
        self.wizProgBar["value"] = int(prog / total * 100) if total else 0
        self.wizProgLbl.config(text="Крок %d з %d • виконано: %d" % (min(prog + 1, total), total, prog))
        if r.get("needModel"):
            self.wizModelLbl.pack(side="left"); self.cbWiz.pack(side="left", padx=6)
        else:
            self.wizModelLbl.pack_forget(); self.cbWiz.pack_forget()
        for w in self.wizStepsFrame.winfo_children():
            w.destroy()
        for s in r.get("steps", []):
            cur = (s.get("idx") == prog)
            ico = "✅" if s.get("done") else ("🔌" if (cur and s.get("external")) else ("▶️" if cur else "•"))
            col = "#1e8449" if s.get("done") else ("#b8860b" if cur else "#333")
            row = ttk.Frame(self.wizStepsFrame); row.pack(fill="x", pady=1)
            ttk.Label(row, text=ico, foreground=col).pack(side="left", anchor="n")
            box = ttk.Frame(row); box.pack(side="left", fill="x", expand=True)
            ttk.Label(box, text=s.get("title", ""), font=("Segoe UI", 9, "bold" if cur else "normal"),
                      foreground=col).pack(anchor="w")
            ttk.Label(box, text=s.get("detail", ""), foreground="#9a9c82",
                      wraplength=440, justify="left").pack(anchor="w")
        if r.get("awaitCharge"):
            done = r.get("chargeDone")
            self.wizBanner.config(text="🔌 АКБ на зарядній станції. Після повного циклу поверніть її сюди."
                                  + ("\n✔ Зміни виявлено — калібрування пройшло, можна продовжити." if done else ""))
            self.wizBanner.pack(anchor="w", pady=4)
            self.wizNextBtn.config(text="✅ Продовжити" if done else "🔄 Перевірити після ЗП", state="normal")
        else:
            self.wizBanner.pack_forget()
            if prog >= total:
                self.wizNextBtn.config(text="✅ Відновлення завершено", state="disabled")
            else:
                cur = r["steps"][prog] if prog < len(r.get("steps", [])) else None
                self.wizNextBtn.config(text="▶️ Виконати: " + (cur.get("title") if cur else "наступний крок"),
                                       state="normal")

    def wiz_next(self):
        r = self._wizState
        if not r or not self.need_conn():
            return
        if r.get("awaitCharge") and not r.get("chargeDone"):
            self.wiz_analyze(); return
        idx = r.get("progress", 0)
        model = ""
        if r.get("needModel"):
            model = self.cbWiz.get()
            if not model:
                messagebox.showwarning("Модель", "Оберіть модель для відновлення"); return
        cmd = "WIZSTEP %d%s" % (idx, (" " + model) if model else "")
        self.status("Виконую крок…")
        self.maybe_auth(lambda: self.cmd(cmd, 25.0, cb=lambda res: self._wiz_after(res)))

    def _wiz_after(self, res):
        msg = res.get("msg") if isinstance(res, dict) else None
        if msg:
            msg = ("✅ " if res.get("result") else "❌ ") + msg
        self._wiz_apply(res, msg)
        self.refresh()

    def wiz_reset(self):
        if not self.need_conn():
            return
        self.cmd("WIZRESET", 5.0, cb=lambda _: self.wiz_analyze())

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
        self.tx33 = scrolledtext.ScrolledText(f, height=6, font=("Consolas", 8),
                                              bg=MIL["field"], fg="#b9bd86", insertbackground=MIL["khaki"],
                                              relief="flat", bd=0); self.tx33.pack(fill="both", expand=True)
        ttk.Label(f, text="Дамп DS2438 (64 Б):").pack(anchor="w", pady=(6, 0))
        self.tx38 = scrolledtext.ScrolledText(f, height=3, font=("Consolas", 8),
                                              bg=MIL["field"], fg="#b9bd86", insertbackground=MIL["khaki"],
                                              relief="flat", bd=0); self.tx38.pack(fill="x")

    def _row(self, parent, text, widget_builder):
        fr = ttk.Frame(parent); fr.pack(fill="x", pady=3)
        ttk.Label(fr, text=text, width=22).pack(side="left")
        return widget_builder(fr)

    def _build_fw(self):
        # Порядок блоків повторює operations.h і веб-інтерфейс: спершу копія,
        # потім РЕМОНТ (головна операція — перша), далі обов'язковий крок на ЗП,
        # обслуговування, ідентичність і лише наприкінці — незворотні дії.
        p = self._scroll_area(self.tabFw)

        b1 = ttk.LabelFrame(p, text="Крок 1 — резервна копія (робіть ЗАВЖДИ перед записом)", padding=8); b1.pack(fill="x", pady=4)
        ttk.Button(b1, text="🔍 Зчитати АКБ", command=self.do_read).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2433", command=lambda: self.save_dump("GET33", 512, "ds2433.bin")).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2438", command=lambda: self.save_dump("GET38", 64, "ds2438.bin")).pack(side="left", padx=3)

        b2b = ttk.LabelFrame(p, text="Крок 2 — РЕМОНТ. Після заміни елементів починайте звідси", padding=8); b2b.pack(fill="x", pady=4)
        ttk.Label(b2b, text="Для АКБ, яку рація бачить «невідома» / не бере на калібрування після заміни банок.\n"
                            "У DS2433 СКИДАЄТЬСЯ навчений хвіст 0x18A–0x1FF: скелет записів і сталі моделі\n"
                            "лишаються, виміряні параметри старих/донорських банок обнуляються, суми правильні.\n"
                            "Ідентичність, крива, заводська таблиця й ЗАПИС МОДЕЛІ не чіпаються. У DS2438\n"
                            "обнуляються лише лічильники; конфіг, калібрування АЦП і дзеркало зберігаються.\n"
                            "НЕ стирає хвіст у 0xFF — на стертому хвості ЗП не завершує калібрування.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        ttk.Button(b2b, text="🔧 Ремонт після заміни елементів",
                   command=lambda: self.simple_op("RECAL", "Ремонт після заміни елементів?\nСкидає навчений хвіст DS2433 (0x18A–0x1FF) у чистий вигляд і обнуляє\nлічильники DS2438. Модель, крива й ідентичність лишаються.\nДалі — калібрування на IMPRES-ЗП.", 25.0)).pack(anchor="w", pady=3)
        ttk.Button(b2b, text="🧹 Глибока чистка",
                   command=lambda: self.simple_op("RECAL DEEP", "Глибока чистка?\nДодатково стирає навчені записи ємності (0x153–0x189) і журнал використання.\nВмикайте, лише якщо після звичайного ремонту ЗП тримається за стару ємність.", 25.0)).pack(anchor="w", pady=3)
        ttk.Button(b2b, text="🛠 Ремонт цілісності (контрольні суми + дзеркало)",
                   command=lambda: self.simple_op("REPAIR", "Відновити цілісність і записати?")).pack(anchor="w", pady=3)

        b2d = ttk.LabelFrame(p, text="Розряд перед калібруванням (навантаження MOSFET)", padding=8); b2d.pack(fill="x", pady=4)
        ttk.Label(b2d, text="Станція не бере АКБ на калібрування, поки бачить його зарядженим. Розряд до 7.2 В\n"
                            "змушує її піти в повний цикл; заразом рахується реальна ємність нових банок.\n"
                            "Струм обмежується ШІМом і веде за напругою: 1000 мА на 8.40 В → 300 мА на 7.20 В.\n"
                            "Аварійна зупинка: < 6.00 В, перегрів 45 °C, стеля часу, втрата зв'язку з монітором.\n"
                            "Резистор гріється — не лишайте без нагляду.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        df = ttk.Frame(b2d); df.pack(anchor="w", pady=3)
        ttk.Button(df, text="🪫 Почати розряд (7.2 В)", command=self.discharge_start).pack(side="left", padx=2)
        ttk.Button(df, text="⏹ Зупинити", command=self.discharge_stop).pack(side="left", padx=2)
        ttk.Button(df, text="🔄 Оновити зараз", command=self.discharge_status).pack(side="left", padx=2)
        # Стан тягнеться сам (див. _dis_tick) — кнопка лишилась тільки щоб не
        # чекати періоду, коли й так стоїш біля пристрою.
        self.monDis = DischargeMonitor(b2d); self.monDis.pack(anchor="w", pady=(6, 0))

        b2c = ttk.LabelFrame(p, text="Крок 3 — калібрування на IMPRES-ЗП (обов'язково)", padding=8); b2c.pack(fill="x", pady=4)
        ttk.Label(b2c, text="Після ремонту навчена калібровка порожня — рація приймає пакет як фірмовий і просить\n"
                            "калібрування. Поставте АКБ на оригінальну IMPRES-ЗП на повний цикл (заряд → розряд →\n"
                            "заряд): саме станція виміряє нові банки й запише калібровку в 0x18A–0x1FF.\n"
                            "Прошивкою цей крок не замінюється.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")

        b2 = ttk.LabelFrame(p, text="Обслуговування (безпечно для ідентичності)", padding=8); b2.pack(fill="x", pady=4)
        ttk.Button(b2, text="♻️ Скидання лічильників", command=lambda: self.simple_op("RESET", "Обнулити лічильники DS2438 (ETM/CCA/DCA)?\nНавчену калібровку й ідентичність не чіпає.")).pack(side="left", padx=3)
        ttk.Button(b2, text="🧹 Очистити дані (лишити ID/калібр.)", command=lambda: self.simple_op("CLEAN", "Стерти дані використання, лишивши ID/калібрування?")).pack(side="left", padx=3)

        b5 = ttk.LabelFrame(p, text="Заряд", padding=8); b5.pack(fill="x", pady=4)
        self.eMah = self._row(b5, "Заряд, мА·год:", lambda fr: self._entry(fr, 10, "0"))
        ttk.Button(b5, text="💾 Записати мА·год", command=self.set_mah).pack(anchor="w", pady=2)
        self.eChg = self._row(b5, "Заряд, %:", lambda fr: self._entry(fr, 10, ""))
        cf = ttk.Frame(b5); cf.pack(anchor="w", pady=2)
        ttk.Button(cf, text="⚡ Заряд по напрузі (авто)", command=self.set_charge_auto).pack(side="left", padx=2)
        ttk.Button(cf, text="💾 Записати заряд %", command=self.set_charge_pct).pack(side="left", padx=2)

        b5c = ttk.LabelFrame(p, text="Дата першого використання (рація рахує як «час − ETM»)", padding=8); b5c.pack(fill="x", pady=4)
        self.eEtmDate = self._row(b5c, "Дата (YYYY-MM-DD):", lambda fr: self._entry(fr, 12))
        ttk.Button(b5c, text="📅 Записати дату (ETM)", command=self.set_etm).pack(anchor="w", pady=2)

        b3 = ttk.LabelFrame(p, text="Ідентичність — модель", padding=8); b3.pack(fill="x", pady=4)
        self.eModel = self._row(b3, "Модель (3–9, A–Z0–9):", lambda fr: self._entry(fr, 12))
        ttk.Button(b3, text="💾 Записати модель", command=self.set_model).pack(anchor="w", pady=2)

        b4r = ttk.LabelFrame(p, text="🛠️ Відновити модельну частину еталона", padding=8); b4r.pack(fill="x", pady=4)
        self.cbRest = self._row(b4r, "Модель-еталон:", lambda fr: self._combo(fr, 18))
        ttk.Label(b4r, text="Пише ідентичність 0x000–0x065, криву, COPYRIGHT, заводську таблицю й запис моделі.\n"
                           "Навчений хвіст 0x18A–0x1FF лишається порожнім — його запише зарядна станція.\n"
                           "Працює й на порожній/битій мікросхемі.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        ttk.Button(b4r, text="🛠️ Відновити модельну частину (DS2433+DS2438)", command=self.restore_battery).pack(anchor="w", pady=2)
        ttk.Button(b4r, text="🧪 Байт-у-байт (ручний режим, для аналізу)", command=self.restore_battery_verbatim).pack(anchor="w", pady=2)

        b4 = ttk.LabelFrame(p, text="🆕 Новий акумулятор (порожній чип)", padding=8); b4.pack(fill="x", pady=4)
        self.cbInit = self._row(b4, "Модель-еталон:", lambda fr: self._combo(fr, 18))
        self.eInitMah = self._row(b4, "Заряд, мА·год:", lambda fr: self._entry(fr, 10, "1000"))
        ttk.Button(b4, text="🆕 Записати новий АКБ (DS2433+DS2438)", command=self.init_battery).pack(anchor="w", pady=2)

        b8 = ttk.LabelFrame(p, text="🧪 Ручний режим / експерт", padding=8); b8.pack(fill="x", pady=4)
        ttk.Label(b8, text="Строк служби в прошивці НЕ зберігається — рація рахує його сама. Поле нижче править\n"
                           "байт у ЗАВОДСЬКІЙ таблиці моделі (0x129) і показань станції не змінить.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.eCap = self._row(b8, "Байт заводської таблиці, %:", lambda fr: self._entry(fr, 10, "100"))
        ttk.Button(b8, text="💾 Записати %", command=self.set_cap).pack(anchor="w", pady=2)

        b6 = ttk.LabelFrame(p, text="⛔ Небезпечна зона (незворотно!)", padding=8); b6.pack(fill="x", pady=4)
        rf = ttk.Frame(b6); rf.pack(fill="x", pady=2)
        ttk.Button(rf, text="📤 Записати DS2433 з .bin (512 Б)", command=lambda: self.write_file(512, "WRITE33")).pack(side="left", padx=3)
        ttk.Button(rf, text="🔬 DS2438 з .bin (64 Б)", command=lambda: self.write_file(64, "WRITE38")).pack(side="left", padx=3)
        ttk.Button(b6, text="🔥 ПОВНЕ стирання DS2433", command=self.wipe33).pack(anchor="w", pady=2)
        ttk.Button(b6, text="🔥 ПОВНЕ стирання DS2438", command=self.wipe38).pack(anchor="w", pady=2)

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
        # padx/bd/highlightthickness заголовків = такі самі, як у Text-панелях
        # нижче, інакше адреси «з'їжджають» по горизонталі відносно "Offset(h)".
        hdr = tk.Frame(f, bg=MIL["bg"]); hdr.pack(fill="x", pady=(6, 0))
        tk.Label(hdr, text="Offset(h)", width=10, font=mono, anchor="w", fg=MIL["olive"], bg=MIL["bg"],
                 padx=5, bd=0, highlightthickness=0).pack(side="left")
        tk.Label(hdr, text="00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F",
                 font=mono, anchor="w", fg=MIL["khaki"], bg=MIL["bg"],
                 padx=5, bd=0, highlightthickness=0).pack(side="left")
        tk.Label(hdr, text="Декодований текст", font=mono, anchor="w", fg=MIL["olive"], bg=MIL["bg"],
                 padx=5, bd=0, highlightthickness=0).pack(side="left")

        body = tk.Frame(f, bg=MIL["bg"]); body.pack(fill="both", expand=True)
        self.hxSb = ttk.Scrollbar(body, orient="vertical", command=self._hx_yview)
        self.hxSb.pack(side="right", fill="y")
        self.hxGut = tk.Text(body, width=10, font=mono, wrap="none", padx=5, spacing1=0,
                             bg="#12140d", fg=MIL["olive"], relief="flat", bd=0, highlightthickness=0,
                             state="disabled", cursor="arrow", takefocus=0)
        self.hxGut.pack(side="left", fill="y")
        self.hxHex = tk.Text(body, width=50, font=mono, wrap="none", padx=5,
                             relief="flat", bd=0, highlightthickness=0,
                             bg=MIL["field"], fg=MIL["fg"], insertbackground=MIL["khaki"])
        self.hxHex.pack(side="left", fill="both", expand=True)
        self.hxAsc = tk.Text(body, width=18, font=mono, wrap="none", padx=5,
                             bg="#12140d", fg="#b9bd86", relief="flat", bd=0, highlightthickness=0,
                             insertbackground=MIL["khaki"])
        self.hxAsc.pack(side="left", fill="y")
        # Модель редактора: справжні байти. Обидві панелі (hex і текст) лише
        # ВІДОБРАЖАЮТЬ модель; правки йдуть у модель через перехоплення клавіш,
        # тож роздільники/адреси/вирівнювання НЕ псуються, а к-сть байт завжди точна.
        self.edBytes = bytearray()
        self.edSize = 512
        # Синхронне вертикальне прокручування трьох панелей.
        self.hxHex.config(yscrollcommand=self._hx_on_scroll)
        for w in (self.hxGut, self.hxHex, self.hxAsc):
            w.bind("<MouseWheel>", self._hx_wheel)
            w.bind("<Button-4>", self._hx_wheel)
            w.bind("<Button-5>", self._hx_wheel)
        # Редагування лише ніблів (hex) і символів (ASCII); решта клавіш — блок.
        self.hxHex.bind("<Key>", self._hx_key)
        self.hxAsc.bind("<Key>", self._hx_asc_key)

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

    # ---- геометрія сітки (16 байт/рядок, подвійний пробіл після 8-го) --------
    @staticmethod
    def _hx_col(j):            # колонка старшого нібла байта j у hex-панелі
        return j * 3 if j < 8 else 25 + (j - 8) * 3

    @classmethod
    def _hx_byte_at(cls, col):  # (індекс_у_рядку, нібл 0/1) або None (роздільник)
        for j in range(16):
            c = cls._hx_col(j)
            if col == c:     return (j, 0)
            if col == c + 1: return (j, 1)
        return None

    @staticmethod
    def _hx_asc_col(j):        # колонка байта j у текстовій панелі
        return j if j < 8 else 10 + (j - 8)

    # ---- рендер трьох панелей З МОДЕЛІ (єдине джерело правди) -----------------
    def _hx_render(self):
        n = len(self.edBytes)
        guts, hexs, ascs = [], [], []
        for i in range(0, max(n, 1), 16):
            guts.append("%08X" % i)
            row = self.edBytes[i:i + 16]
            hx = " ".join("%02X" % b for b in row[:8])
            if len(row) > 8:
                hx += "  " + " ".join("%02X" % b for b in row[8:16])
            hexs.append(hx)
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
            ascs.append((asc[:8] + "  " + asc[8:]) if len(asc) > 8 else asc)
        if n == 0:
            guts, hexs, ascs = [], [], []
        self._set_ro(self.hxGut, "\n".join(guts))
        # hex/asc лишаємо редагованими (state=normal), просто перезаписуємо вміст
        self.hxHex.delete("1.0", "end"); self.hxHex.insert("1.0", "\n".join(hexs))
        self.hxAsc.delete("1.0", "end"); self.hxAsc.insert("1.0", "\n".join(ascs))

    def _hx_setcur(self, w, r, c):
        try:
            w.mark_set("insert", "%d.%d" % (r, c)); w.see("insert")
        except Exception:
            pass

    # ---- редагування hex-панелі: лише нібли -----------------------------------
    def _hx_key(self, e):
        if e.keysym in ("Left", "Right", "Up", "Down", "Home", "End",
                        "Prior", "Next", "Tab", "Shift_L", "Shift_R"):
            return None                                   # навігація — вільно
        ch = e.char
        if not ch or ch not in "0123456789abcdefABCDEF":
            return "break"                                # пробіли/Backspace/букви — блок
        try:
            r, c = map(int, self.hxHex.index("insert").split("."))
        except Exception:
            return "break"
        bn = self._hx_byte_at(c)
        if bn is None:                                    # курсор на роздільнику
            return "break"
        j, nib = bn
        bi = (r - 1) * 16 + j
        if bi >= len(self.edBytes):
            return "break"
        d = int(ch, 16)
        v = self.edBytes[bi]
        v = ((d << 4) | (v & 0x0F)) if nib == 0 else ((v & 0xF0) | d)
        self.edBytes[bi] = v
        self._hx_render()
        # перехід на наступний нібл
        if nib == 0:
            self._hx_setcur(self.hxHex, r, c + 1)
        else:
            nb = bi + 1
            if nb < len(self.edBytes):
                self._hx_setcur(self.hxHex, nb // 16 + 1, self._hx_col(nb % 16))
            else:
                self._hx_setcur(self.hxHex, r, c)
        return "break"

    # ---- редагування текстової панелі: друкований символ -> байт --------------
    def _hx_asc_key(self, e):
        if e.keysym in ("Left", "Right", "Up", "Down", "Home", "End",
                        "Prior", "Next", "Tab", "Shift_L", "Shift_R"):
            return None
        ch = e.char
        if not ch or not (32 <= ord(ch) < 127):
            return "break"
        try:
            r, c = map(int, self.hxAsc.index("insert").split("."))
        except Exception:
            return "break"
        # колонка -> індекс байта (пропускаємо подвійний пробіл між групами)
        j = None
        for k in range(16):
            if c == self._hx_asc_col(k):
                j = k; break
        if j is None:
            return "break"
        bi = (r - 1) * 16 + j
        if bi >= len(self.edBytes):
            return "break"
        self.edBytes[bi] = ord(ch)
        self._hx_render()
        nb = bi + 1
        if nb < len(self.edBytes):
            self._hx_setcur(self.hxAsc, nb // 16 + 1, self._hx_asc_col(nb % 16))
        return "break"

    def _build_log(self):
        self.txLog = scrolledtext.ScrolledText(self.tabLog, font=("Consolas", 8),
                                               bg=MIL["field"], fg="#b9bd86", insertbackground=MIL["khaki"],
                                               relief="flat", bd=0); self.txLog.pack(fill="both", expand=True)

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
        _src = d.get("chargeSrc", "")
        _srclbl = {"ICA": "ICA", "volt": "напруга", "U!": "за напругою (паливомір не калібр.)"}.get(_src, _src)
        self.ovCharge.config(text=(f"{ch}%  ({_srclbl})" if isinstance(ch, int) and ch >= 0 else "—"))
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

    # ---- керований розряд ---------------------------------------------
    def _dis_show(self, r):
        # Уся візуалізація — у DischargeMonitor; тут лише передаємо стан і
        # знімаємо ознаку «запит у польоті».
        self._disBusy = False
        d = (r or {}).get("discharge") if isinstance(r, dict) else None
        self.monDis.update_state(d)

    def _dis_tick(self):
        """Автоопитування стану розряду.

        Раз на 3 с, поки розряд іде, і раз на 30 с у спокої — щоб побачити
        розряд, запущений кнопкою на самому пристрої, а не звідси. Прапорець
        _disBusy не дає накопичувати запити в черзі порту, якщо пристрій
        відповідає повільно (а він відповідає повільно: під час розряду кожне
        опитування — це два читання 1-Wire з паузами на вимір).
        """
        try:
            d = self.monDis.d
            run = bool(d) and d.get("state") == "run"
            if self.connected and not self._disBusy:
                self._disBusy = True
                self.cmd("DISCHARGE ?", 8.0, cb=self._dis_show)
            self.root.after(3000 if run else 30000, self._dis_tick)
        except tk.TclError:
            pass                      # вікно закрилось

    def discharge_status(self):
        if not self.need_conn():
            return
        self._disBusy = True
        self.cmd("DISCHARGE ?", 8.0, cb=self._dis_show)

    def discharge_start(self):
        if not self.need_conn():
            return
        if not messagebox.askyesno("Розряд",
                "Почати розряд до 7.2 В?\n\n"
                "Струм обмежується ШІМом: 1000 мА на повному заряді, лінійно до 300 мА на 7.20 В.\n"
                "Навантаження буде увімкнено, резистор нагріється.\n"
                "Не лишайте пристрій без нагляду."):
            return
        def done(r):
            if isinstance(r, dict) and r.get("ok"):
                self.status("Розряд почато"); self.monDis.reset_history(); self._dis_show(r)
            else:
                self._disBusy = False
                self.status("Помилка: " + str((r or {}).get("err", "")))
        self.maybe_auth(lambda: self.cmd("DISCHARGE 7200", 15.0, cb=done))

    def discharge_stop(self):
        if not self.need_conn():
            return
        self.cmd("DISCHARGE STOP", 10.0, cb=lambda r: (self.status("Розряд зупинено"), self._dis_show(r)))

    def restore_battery(self, verbatim=False):
        # verbatim=False — переносимо лише модельну частину еталона; навчений
        # хвіст 0x18A–0x1FF лишається порожнім. Побайтовий запис віддає новому
        # пакету ЧУЖУ навчену калібровку, після чого рація каже «невідомий
        # акумулятор» — тому це окремий, явно позначений ручний режим.
        if not self.need_conn():
            return
        model = self.cbRest.get()
        if not model:
            messagebox.showwarning("Модель", "Оберіть модель-еталон"); return
        if verbatim:
            q = (f"РУЧНИЙ РЕЖИМ: записати еталон {model} БАЙТ-У-БАЙТ?\n"
                 f"Разом із навченою калібровкою донора — рація після цього зазвичай каже "
                 f"«невідомий акумулятор». Для аналізу, не для ремонту.")
        else:
            q = (f"Відновити модельну частину еталона {model}?\n"
                 f"Перезапише ОБИДВІ мікросхеми; навчений хвіст 0x18A–0x1FF лишиться порожнім "
                 f"(його запише зарядна станція). Працює й на порожньому/битому чипі.")
        if not messagebox.askyesno("Відновити еталон", q):
            return
        def done(r):
            if isinstance(r, dict) and r.get("ok"):
                both = r.get("ds2438")
                self._after_write(r, f"✅ Еталон {model} записано" + (" (DS2433+DS2438)" if both else " (лише DS2433)"))
            else:
                self._after_write(r, "")
        arg = f"RESTORE {model}" + (" VERBATIM" if verbatim else "")
        self.maybe_auth(lambda: (self.status("Відновлення еталона..."),
                                 self.cmd(arg, 25.0, cb=done)))

    def restore_battery_verbatim(self):
        self.restore_battery(verbatim=True)

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

    def set_charge_auto(self):
        if not self.need_conn():
            return
        if not messagebox.askyesno("Заряд", "Виставити рівень заряду з поточної напруги?\n(7.0 В = 0%, 8.4 В = 100%; зарядка потім уточнить)"):
            return
        self.maybe_auth(lambda: self.cmd("SETCHG auto", 15.0,
            cb=lambda r: self._after_write(r, f"✅ Заряд {r.get('pct','?')}% (ICA {r.get('ica','?')})")))

    def set_charge_pct(self):
        if not self.need_conn():
            return
        try:
            v = int(self.eChg.get())
        except ValueError:
            messagebox.showwarning("Заряд %", "Вкажіть 0..100"); return
        if v < 0 or v > 100:
            messagebox.showwarning("Заряд %", "0..100"); return
        self.maybe_auth(lambda: self.cmd(f"SETCHG {v}", 15.0,
            cb=lambda r: self._after_write(r, f"✅ Заряд {v}%")))

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

    def wipe38(self):
        if not self.need_conn():
            return
        if not messagebox.askyesno("ПОВНЕ стирання", "🔥 Стерти ВЕСЬ DS2438 у 0xFF?\nДзеркало калібрування зникне, АКБ треба відновити («Новий АКБ»).\nВи зробили копію?"):
            return
        if not messagebox.askyesno("Підтвердження", "Останнє попередження: стерти DS2438 ПОВНІСТЮ?"):
            return
        self.maybe_auth(lambda: (self.status("Стирання 2438..."),
                                 self.cmd("WIPE38", 25.0, cb=lambda r: self._after_write(r, "✅ DS2438 стерто. Відновіть АКБ."))))

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
        for cb in (self.cbInit, self.cbRest, self.cbWiz):
            cb["values"] = models
            if models and not cb.get():
                cb.current(0)

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
        self.edSize = 64 if is38 else 512
        self.cmd("GET38" if is38 else "GET33", 15.0, cb=self._hx_show)

    def _hx_show(self, r):
        if not r.get("ok") or not r.get("hex"):
            messagebox.showwarning("Дамп", "Зчитайте АКБ"); return
        import re
        hexs = re.sub(r"[^0-9a-fA-F]", "", r["hex"])
        self.edBytes = bytearray(int(hexs[i:i + 2], 16) for i in range(0, len(hexs), 2))
        self.edSize = len(self.edBytes)
        self._hx_render()

    def hx_write(self):
        if not self.need_conn():
            return
        is38 = self.hxTarget.current() == 1
        need = 64 if is38 else 512
        # Пишемо СТРОГО з моделі — рівно потрібна к-сть байт, без ризику
        # зіпсувати вирівнювання/роздільники (звідси й були помилки запису).
        if len(self.edBytes) != need:
            messagebox.showwarning("Байти",
                f"У редакторі {len(self.edBytes)} байт, треба {need}. Натисніть «↻ Завантажити».")
            return
        if not messagebox.askyesno("Запис", f"⚡ Запис {need} байт НЕЗВОРОТНІЙ. Продовжити?"):
            return
        command = "WRITE38" if is38 else ("WRITEFIX33" if self.hxFix.get() else "WRITE33")
        hexs = "".join("%02x" % b for b in self.edBytes)
        self.maybe_auth(lambda: (self.status("Запис байтів..."),
                                 self.cmd(command + " " + hexs, 25.0, cb=lambda r: self._after_write(r, "✅ Записано"))))


# Мілітарна тема НГУ: оливково-хакі поле + акцент «краповий» (maroon).
MIL = dict(bg="#20241a", bg_dark="#14160f", fg="#e7e3d2", mut="#9a9c82",
           line="#3b4230", field="#171a10", btn="#2c3222", btn_act="#3a4230",
           maroon="#7d2230", olive="#6f8f3a", khaki="#c8b04a")

def apply_military_theme(root):
    try:
        st = ttk.Style(); st.theme_use("clam")
    except Exception:
        return
    m = MIL
    root.configure(bg=m["bg_dark"])
    root.option_add("*TCombobox*Listbox.background", m["field"])
    root.option_add("*TCombobox*Listbox.foreground", m["fg"])
    root.option_add("*TCombobox*Listbox.selectBackground", m["maroon"])
    st.configure(".", background=m["bg"], foreground=m["fg"], fieldbackground=m["field"],
                 bordercolor=m["line"], lightcolor=m["bg"], darkcolor=m["bg"],
                 troughcolor=m["field"], focuscolor=m["maroon"])
    st.configure("TFrame", background=m["bg"])
    st.configure("TLabel", background=m["bg"], foreground=m["fg"])
    st.configure("TLabelframe", background=m["bg"], bordercolor=m["line"])
    st.configure("TLabelframe.Label", background=m["bg"], foreground=m["khaki"])
    st.configure("TButton", background=m["btn"], foreground=m["fg"], bordercolor=m["line"], padding=5)
    st.map("TButton", background=[("active", m["btn_act"]), ("pressed", m["maroon"])],
           foreground=[("disabled", m["mut"])])
    st.configure("TCheckbutton", background=m["bg"], foreground=m["fg"])
    st.map("TCheckbutton", background=[("active", m["bg"])])
    st.configure("TEntry", fieldbackground=m["field"], foreground=m["fg"], bordercolor=m["line"], insertcolor=m["fg"])
    st.configure("TCombobox", fieldbackground=m["field"], foreground=m["fg"], background=m["btn"],
                 bordercolor=m["line"], arrowcolor=m["khaki"])
    st.map("TCombobox", fieldbackground=[("readonly", m["field"])])
    st.configure("TProgressbar", background=m["olive"], troughcolor=m["field"], bordercolor=m["line"])
    st.configure("TNotebook", background=m["bg_dark"], bordercolor=m["line"])
    st.configure("TNotebook.Tab", background=m["btn"], foreground=m["mut"], padding=(12, 6))
    st.map("TNotebook.Tab", background=[("selected", m["maroon"])], foreground=[("selected", "#ffffff")])
    st.configure("TScrollbar", background=m["btn"], troughcolor=m["bg_dark"], bordercolor=m["line"], arrowcolor=m["khaki"])


def main():
    root = tk.Tk()
    apply_military_theme(root)
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
