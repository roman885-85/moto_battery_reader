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
import sys, os, re, time, json, math, queue, threading, datetime
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
from tkinter import font as tkfont

# ===========================================================================
#  МАСШТАБУВАННЯ ВМІСТУ
# ===========================================================================
#  Вікно можна тягнути й розгортати на весь екран, і вміст має рости разом із
#  ним, а не лишатися острівцем дрібного тексту посеред порожнечі.
#
#  Механізм — ІМЕНОВАНІ шрифти Tk. Віджет, якому дали такий шрифт, підхоплює
#  будь-яку його зміну сам: досить переналаштувати розмір — і весь інтерфейс
#  перемальовується без перебудови. Тому в коді немає жодного font=fnt("Segoe UI",
#  10) — усюди fnt("Segoe UI", 10), який віддає саме такий іменований шрифт.
#
#  Розміри в дужках — БАЗОВІ, для масштабу 100 %. Реальний розмір щоразу
#  перераховується як базовий * поточний масштаб.
_FONTS = {}                 # (сімейство, базовий розмір, накреслення) -> tkfont.Font
_TK_BASE = {}               # вбудовані шрифти Tk -> їхній рідний розмір
_FONT_SCALE = 1.0

# Tk трактує ВІД'ЄМНИЙ розмір як пікселі, додатний — як пункти. Зберігаємо знак,
# інакше на системах із піксельними шрифтами (типовий Linux) масштаб перевернув
# би одиниці й текст стрибнув би в кілька разів.
def _scaled(base, k):
    v = max(6, int(round(abs(base) * k)))
    return -v if base < 0 else v

def fnt(family, size, weight="normal"):
    key = (family, size, weight)
    f = _FONTS.get(key)
    if f is None:
        f = tkfont.Font(family=family, size=_scaled(size, _FONT_SCALE), weight=weight)
        _FONTS[key] = f
    return f

def fonts_init():
    """Запам'ятати рідні розміри вбудованих шрифтів Tk.

    Під ними живуть УСІ ttk-віджети (кнопки, вкладки, поля), яким шрифт явно не
    задавали. Масштабувати треба й їх, інакше половина інтерфейсу росла б, а
    половина — ні.
    """
    for name in ("TkDefaultFont", "TkTextFont", "TkFixedFont", "TkMenuFont",
                 "TkHeadingFont", "TkCaptionFont", "TkSmallCaptionFont", "TkIconFont"):
        try:
            _TK_BASE[name] = tkfont.nametofont(name).cget("size")
        except tk.TclError:
            pass

def fonts_rescale(k):
    global _FONT_SCALE
    _FONT_SCALE = k
    for (family, size, weight), f in _FONTS.items():
        f.configure(size=_scaled(size, k))
    for name, base in _TK_BASE.items():
        try:
            tkfont.nametofont(name).configure(size=_scaled(base, k))
        except tk.TclError:
            pass


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


# ===========================================================================
#  БУФЕР ОБМІНУ Й КОНТЕКСТНЕ МЕНЮ
# ===========================================================================
#  Дві окремі причини, чому копіювання не працювало.
#
#  1. РОЗКЛАДКА. Tk віддає у keysym СИМВОЛ, а не клавішу. На українській або
#     російській розкладці Ctrl+C — це не 'c', а 'Cyrillic_es', і стандартна
#     прив'язка <<Copy>> просто не спрацьовує. Тому ловимо обидва набори імен.
#
#  2. ПЕРЕХОПЛЕННЯ КЛАВІШ У РЕДАКТОРІ. Панелі hex/ASCII мають власний обробник
#     <Key>, який блокує все, що не є шістнадцятковою цифрою. Ctrl+C приходить
#     туди як символ \x03 — теж «не цифра», отже блокувався ще до вбудованого
#     копіювання. Тепер Ctrl-поєднання розбираються ПЕРШИМИ.
#
#  Контекстного меню в Tk немає взагалі — його треба будувати руками, що й
#  зроблено нижче для всіх полів вводу й обох панелей редактора.
# Пункт списку «шунт беремо не з бібліотеки, а руками / зі свого пакета».
RP_RS_MANUAL = "— вручну —"
# Режим навченого хвоста 0x18A..0x1FF; порядок важливий — [0] типовий, [1] стирання.
TAIL_MODES = ("Свіжий (скелет + нулі)", "Стерти в 0xFF")
# Звідки брати наробіток; порядок важливий — [0] свій, [1] із дати запуску.
RP_ETM_SRC = ("наробіток самого пакета", "порахувати з дати першого запуску")
# Типові шунти для режиму копії: коли в дампі шунта немає, беруть один із них.
# Перший пункт означає «лишити той, що в дампі».
CLONE_SHUNTS = ("— з дампа копії —", "4565 — 45.65 мОм (родина 4409)",
                "2490 — 24.90 мОм (4488/4493)", "2500 — 25.00 мОм (4809)")


def _dnum(v):
    """Дата з плану (число YYYYMMDD) у людський вигляд; 0 -> прочерк."""
    return "%04d-%02d-%02d" % (v // 10000, (v // 100) % 100, v % 100) if v else "—"

CTRL_MASK = 0x0004
_CTRL_KEYS = {"c": ("c", "cyrillic_es"),      # С
              "v": ("v", "cyrillic_em"),      # М
              "x": ("x", "cyrillic_che"),     # Ч
              "a": ("a", "cyrillic_ef")}      # Ф

def ctrl_combo(e):
    """'c'/'v'/'x'/'a', якщо натиснуто відповідне Ctrl-поєднання; інакше None."""
    if not (e.state & CTRL_MASK):
        return None
    ks = (e.keysym or "").lower()
    for k, names in _CTRL_KEYS.items():
        if ks in names:
            return k
    return None

def popup_menu(menu, e):
    try:
        menu.tk_popup(e.x_root, e.y_root)
    finally:
        menu.grab_release()

def select_all(w):
    try:
        if isinstance(w, tk.Text):
            w.tag_add("sel", "1.0", "end-1c")
            w.mark_set("insert", "1.0")
        else:
            w.select_range(0, "end")
    except tk.TclError:
        pass


class DischargeMonitor(tk.Canvas):
    """Панель процесу розряду — той самий вигляд, що у веб-версії пристрою.

    Розряд триває годинами, тож дивитись доводиться довго: рядок тексту для
    цього не годиться. Тут пульсуючий індикатор стану, смуга прогресу з
    «течією» (єдина ознака, що процес живий: між опитуваннями пристрою числа
    стоять на місці), графік напруги за ВЕСЬ сеанс, коридор уставки струму,
    шпаруватість ключа й плитки показань.

    Малює себе сама раз на 80 мс (анімація й рівний хід годинника), дані
    отримує ззовні через update_state() — опитування живе в App.

    МАСШТАБ. redraw() малює в БАЗОВИХ координатах (W×H нижче), а наприкінці всю
    картинку розтягує canvas.scale(). Шрифти при цьому не чіпаються — вони й так
    приходять із fnt(), тобто вже масштабовані тим самим коефіцієнтом. Тому
    достатньо тримати self.k рівним масштабу інтерфейсу, і панель росте разом із
    рештою вікна, а розмічати її можна й далі в зручних числах.
    """
    W, H = 720, 358          # базові («дизайнерські») розміри, масштаб 100 %
    PAD = 14

    def __init__(self, master):
        super().__init__(master, width=self.W, height=self.H, bg=MIL["field"],
                         highlightthickness=1, highlightbackground=MIL["line"])
        self.d = None            # останній стан із пристрою
        self.at = 0.0            # коли він прийшов — щоб годинник ішов рівно
        self.hist = []           # [(t, mv)] — крива напруги за весь сеанс
        self.step = 10           # поточний крок між точками кривої, с
        self.phase = 0.0
        self.k = 1.0             # масштаб вмісту
        self._alive = True
        self._tick()

    def set_scale(self, k):
        if abs(k - self.k) < 0.01:
            return
        self.k = k
        try:
            self.config(width=int(self.W * k), height=int(self.H * k))
            self.redraw()
        except tk.TclError:
            pass

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
                         font=fnt("Segoe UI", 8))
        self.create_text(x + 8, y + h - 8, text=value, anchor="sw", fill=MIL["fg"],
                         font=fnt("Consolas", 13, "bold"))

    def _fmt_t(self, s):
        s = int(max(0, s))
        return "%d:%02d:%02d" % (s // 3600, (s // 60) % 60, s % 60)

    # ---- повна перемальовка --------------------------------------------
    def redraw(self):
        self.delete("all")
        self._redraw_base()
        # Розмітка вище — у базових координатах; тут розтягуємо її під поточний
        # масштаб. Шрифти вже масштабовані (див. fnt), тож текст і позиції
        # ростуть разом. Товщину ліній лишаємо як є: волосяні лінії на великому
        # масштабі виглядають краще за роздуті.
        if self.k != 1.0:
            self.scale("all", 0, 0, self.k, self.k)

    def _redraw_base(self):
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
                             fill=MIL["mut"], font=fnt("Segoe UI", 10))
            return
        if not d.get("available"):
            self.create_text(W // 2, H // 2, text="розряд не налаштовано (LOAD_PIN у settings.h)",
                             fill=MIL["mut"], font=fnt("Segoe UI", 10))
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
                         fill=MIL["fg"], font=fnt("Segoe UI", 11, "bold"))
        el = d.get("elapsedS", 0) + (time.time() - self.at if run else 0)
        self.create_text(W - P, 20, text=self._fmt_t(el), anchor="e",
                         fill=MIL["mut"], font=fnt("Consolas", 11))

        # --- велика напруга ---
        mv, tgt, st0 = d.get("mv", 0), d.get("targetMv", 0), d.get("startMv", 0)
        big = self.create_text(P, 58, text="%.2f" % (mv / 1000.0), anchor="w",
                               fill=MIL["fg"], font=fnt("Segoe UI", 26, "bold"))
        # Підпис «В» і рядок цілі ставимо ПО ФАКТИЧНІЙ ширині числа: шрифт
        # 26 pt на різних системах міряється по-різному, і фіксовані відступи
        # то залишали діру, то налазили на цифри. bbox() віддає ширину вже
        # масштабованого шрифта, а розмічаємо ми в базових координатах — тому
        # ділимо на масштаб, інакше на 150 % підпис поїхав би праворуч.
        x = P + (self.bbox(big)[2] - P) / self.k + 6
        self.create_text(x, 62, text="В", anchor="w", fill=MIL["mut"], font=fnt("Segoe UI", 11))
        self.create_text(x + 22, 62, text="старт %.2f В  →  ціль %.2f В" % (st0 / 1000.0, tgt / 1000.0),
                         anchor="w", fill=MIL["mut"], font=fnt("Segoe UI", 9))

        # --- прогрес до цілі ---
        span, done = st0 - tgt, st0 - mv
        pct = max(0, min(100, int(round(done * 100.0 / span)))) if span > 0 else 0
        self._bar(P, 80, W - 2 * P, 17, pct / 100.0, MIL["olive"], striped=run)
        self.create_text(W - P - 8, 88, text="%d %%" % pct, anchor="e",
                         fill=MIL["fg"], font=fnt("Segoe UI", 8, "bold"))

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
                             fill=MIL["mut"], font=fnt("Segoe UI", 8))

        # --- струм у коридорі уставки ---
        pwm = bool(d.get("pwm"))
        ma = abs(d.get("ma", 0)); setMa = d.get("setMa", 0) or 1
        lo_ma, hi_ma = d.get("bandLoMa", 0), d.get("bandHiMa", 0)
        # Шкала — до 125 % уставки: коридор займає більшу частину доріжки, а
        # вихід за нього одразу впадає в око.
        scale = max(setMa * 1.25, ma * 1.05, 1)
        self.create_text(P, 186, text="струм / уставка", anchor="w", fill=MIL["mut"], font=fnt("Segoe UI", 8))
        right = ("уставка %d мА · пік %d мА" % (setMa, d.get("peakMa", 0))) if pwm \
                else "ШІМ недоступний — струм не обмежено"
        self.create_text(W - P, 186, text=right, anchor="e",
                         fill=(MIL["olive"] if d.get("inBand") else MIL["khaki"]) if pwm else MIL["maroon"],
                         font=fnt("Segoe UI", 8))
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
                         fill=MIL["mut"], font=fnt("Segoe UI", 8))
        self.create_text(W - P, 222, text=("%d %%" % duty) if pwm else "ключ відкрито постійно",
                         anchor="e", fill=MIL["mut"], font=fnt("Segoe UI", 8))
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
        self.create_text(W // 2, H - 8, anchor="s", fill="#ff9b8f", font=fnt("Segoe UI", 8),
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


class ChargeMonitor(ttk.Frame):
    """Панель стану заряду через DC/DC — простіша за DischargeMonitor: ціль
    фіксована (CHARGE_TARGET_MV), тож немає що малювати «уставку струму за
    напругою», графік історії не потрібен. Прості ttk-віджети замість Canvas.
    """
    def __init__(self, master):
        super().__init__(master)
        self.d = None
        self.lblState = ttk.Label(self, text="—", font=fnt("Segoe UI", 11, "bold"))
        self.lblState.grid(row=0, column=0, columnspan=3, sticky="w")
        self.lblClock = ttk.Label(self, text="0:00:00", foreground=MIL["mut"])
        self.lblClock.grid(row=0, column=3, sticky="e")
        self.lblMv = ttk.Label(self, text="—", font=fnt("Consolas", 20, "bold"))
        self.lblMv.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self.lblSub = ttk.Label(self, text="—", foreground=MIL["mut"])
        self.lblSub.grid(row=1, column=2, columnspan=2, sticky="e", pady=(4, 0))
        self.bar = ttk.Progressbar(self, maximum=100, length=420)
        self.bar.grid(row=2, column=0, columnspan=4, sticky="we", pady=(4, 4))
        self.lblLim = ttk.Label(self, text="—")
        self.lblLim.grid(row=3, column=0, columnspan=4, sticky="w")
        self.lblOut = ttk.Label(self, text="—")
        self.lblOut.grid(row=4, column=0, columnspan=4, sticky="w")
        tiles = ttk.Frame(self); tiles.grid(row=5, column=0, columnspan=4, sticky="we", pady=(6, 0))
        self.tileVars = {}
        for i, (key, label) in enumerate([
            ("mah", "отримано, мА·год"), ("cca", "CCA, мА·год"), ("ma", "струм, мА"),
            ("w", "потужність, Вт"), ("t", "температура, °C"), ("ica", "паливомір ICA"),
        ]):
            f = ttk.Frame(tiles); f.grid(row=i // 3, column=i % 3, sticky="we", padx=3, pady=3)
            ttk.Label(f, text=label, foreground=MIL["mut"], font=fnt("Segoe UI", 8)).pack(anchor="w")
            v = ttk.Label(f, text="—", font=fnt("Consolas", 12, "bold")); v.pack(anchor="w")
            self.tileVars[key] = v
        self.lblWarn = ttk.Label(self, text="", foreground=MIL["maroon"], wraplength=420, justify="left")
        self.lblWarn.grid(row=6, column=0, columnspan=4, sticky="w", pady=(4, 0))

    def _fmt_t(self, s):
        s = int(max(0, s))
        return "%d:%02d:%02d" % (s // 3600, (s // 60) % 60, s % 60)

    def update_state(self, d):
        self.d = d
        if not d:
            self.lblState.config(text="—")
            return
        if not d.get("available"):
            self.lblState.config(text="не налаштовано (CHARGE_PWM_PIN/ISENSE/VSENSE)")
            return
        state = d.get("state", "idle")
        run = state == "run"
        txt = {"idle": "очікування", "run": "ІДЕ ЗАРЯД",
               "done": "✅ ЗАРЯД ЗАВЕРШЕНО", "abort": "⛔ " + str(d.get("reason") or "аварія")}.get(state, state)
        # ЖИВЛЕННЯ +14 В — попереду стану заряду: несправний блок означає, що
        # заряд не піде взагалі, і «очікування» поверх цього приховало б єдину
        # причину, яку користувач може усунути сам.
        bad_psu = d.get("psuSensed") and not d.get("psuOk", True)
        if bad_psu:
            txt = "⛔ %s (%.2f В, потрібно %.1f…%.1f В)" % (
                d.get("psuText", "живлення поза допуском"), d.get("psuMv", 0) / 1000.0,
                d.get("psuMinMv", 0) / 1000.0, d.get("psuMaxMv", 0) / 1000.0)
        self.lblState.config(text=txt,
                             foreground=(MIL["maroon"] if bad_psu else
                                         MIL["olive"] if run else MIL["fg"]))
        mv, pct = d.get("mv", 0), d.get("pct", 0)
        self.lblMv.config(text="%.2f В" % (mv / 1000.0))
        self.lblSub.config(text="%d %% · старт %.2f В → ціль %.2f В"
                                 % (pct, d.get("startMv", 0) / 1000.0, d.get("targetMv", 0) / 1000.0))
        self.bar["value"] = max(0, min(100, pct))
        ma, setMa, pwm = d.get("ma", 0), d.get("setMa", 0), d.get("pwm", False)
        self.lblLim.config(text=("уставка %d мА · зараз %d мА" % (setMa, ma)) if pwm else "⚠ керування недоступне",
                            foreground=MIL["olive"] if pwm else MIL["maroon"])
        # Керування тепер — ШПАРУВАТІСТЬ ключа (PNP B772M через керуючий NPN), а не «цільова
        # напруга DC/DC». Поруч — вершина пульсацій струму дроселя: вона злітає,
        # коли дросель фактично випав із кола (обрив, насичення, пробитий ключ).
        duty, dutyFull = d.get("duty", 0), d.get("dutyFull", 1) or 1
        dutyMax, dutyPct = d.get("dutyMax", dutyFull), d.get("dutyPct", 0)
        peak, peakMax = d.get("peakMa", 0), d.get("peakMaxMa", 0)
        shunt = (d.get("shuntMohm", 0) or 0) / 1000.0
        self.lblOut.config(text=("ШІМ %d%% (%d/%d, стеля %d) · пік %d мА з %d (шунт %.2f Ом)"
                                 % (dutyPct, duty, dutyFull, dutyMax, peak, peakMax, shunt))
                                 if pwm else "керування недоступне")
        self.tileVars["mah"].config(text=str(d.get("mah", 0)))
        self.tileVars["cca"].config(text=str(d.get("ccaMah", 0)))
        self.tileVars["ma"].config(text=str(ma))
        self.tileVars["w"].config(text=str(d.get("watts", 0)))
        self.tileVars["t"].config(text=str(d.get("tempC", 0)))
        self.tileVars["ica"].config(text=str(d.get("ica", 0)))
        if not pwm and (run or state == "done"):
            self.lblWarn.config(text="Керування недоступне: каналу LEDC не знайшлося — "
                                      "вимкніть заряд і перевірте CHARGE_LEDC_CH у settings.h.")
        else:
            self.lblWarn.config(text="")
        self.lblClock.config(text=self._fmt_t(d.get("elapsedS", 0)))


class App:
    # Базовий розмір вікна = масштаб 100 %. Автомасштаб рахується як відношення
    # поточного розміру до цього — по МЕНШІЙ зі сторін, інакше широке й низьке
    # вікно роздуло б шрифти так, що вміст довелося б прокручувати.
    BASE_W, BASE_H = 760, 620
    ZOOM_MIN, ZOOM_MAX = 0.8, 2.2

    def __init__(self, root):
        self.root = root
        root.title("Moto IMPRES — USB")
        root.geometry("%dx%d" % (self.BASE_W, self.BASE_H))
        # Мінімум — під найменший масштаб: нижче вміст усе одно прокручується.
        root.minsize(620, 480)
        root.resizable(True, True)

        fonts_init()
        self.zoom = 1.0
        self.zoomAuto = tk.BooleanVar(value=True)
        self._zoomJob = None
        self._fullscreen = False

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
        self._chgBusy = False

        self._build()
        self.attach_menus_all()
        self._bind_zoom()
        self.refresh_ports()
        self.root.after(40, self._poll)
        self.root.after(1000, self._dis_tick)     # стан розряду тягнеться сам
        self.root.after(1000, self._chg_tick)     # стан заряду тягнеться сам
        self.root.after(500, self._psu_blink)     # блимання смуги аварії живлення
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- контекстне меню для звичайних полів ---------------------------
    def attach_menus_all(self, parent=None):
        """Обійти всі поля вводу й повісити меню правої кнопки.

        Обхід деревом, а не список вручну: полів десятки (журнал, дампи,
        ємність, модель, дата, пароль...), і будь-яке нове поле інакше знову
        лишилось би без копіювання — саме так і виходило досі.
        """
        parent = parent or self.root
        for c in parent.winfo_children():
            if c in (self.hxHex, self.hxAsc):
                pass                                   # у редактора власне меню
            elif isinstance(c, ttk.Combobox):
                pass                                   # там правою кнопкою нічого не роблять
            elif isinstance(c, tk.Text):               # сюди ж ScrolledText
                self.attach_menu(c, editable=str(c.cget("state")) == "normal")
            elif isinstance(c, (tk.Entry, ttk.Entry)):
                self.attach_menu(c, editable=str(c.cget("state")) not in ("readonly", "disabled"))
            self.attach_menus_all(c)

    def attach_menu(self, w, editable=True):
        """Меню правої кнопки + Ctrl-поєднання, що не залежать від розкладки.

        Копіювання/вставка йдуть через ВІРТУАЛЬНІ події Tk (<<Copy>> тощо) —
        так однаково працює і для Entry, і для Text, і для ttk-віджетів.
        """
        m = tk.Menu(w, tearoff=0)
        if editable:
            m.add_command(label="Вирізати", command=lambda: w.event_generate("<<Cut>>"))
        m.add_command(label="Копіювати", command=lambda: w.event_generate("<<Copy>>"))
        if editable:
            m.add_command(label="Вставити", command=lambda: w.event_generate("<<Paste>>"))
        m.add_separator()
        m.add_command(label="Виділити все", command=lambda: select_all(w))

        def on_rmb(e):
            w.focus_set()
            # Курсор пересуваємо ЛИШЕ якщо виділення немає — інакше клац правою
            # по виділеному тексту знищував би саме те, що збираються копіювати.
            try:
                w.index("sel.first")
            except tk.TclError:
                try:
                    w.mark_set("insert", "@%d,%d" % (e.x, e.y))     # Text
                except (tk.TclError, AttributeError):
                    try:
                        w.icursor("@%d" % e.x)                      # Entry
                    except (tk.TclError, AttributeError):
                        pass
            popup_menu(m, e)
            return "break"

        w.bind("<Button-3>", on_rmb)
        w.bind("<Key>", lambda e: self.ctx_key(w, e, editable), add="+")
        return m

    def ctx_key(self, w, e, editable=True):
        """Ctrl+C/V/X/A незалежно від розкладки. Окремим методом, а не
        замиканням, — щоб логіку можна було перевірити без справжніх подій."""
        k = ctrl_combo(e)
        if not k:
            return None
        if k == "a":
            select_all(w); return "break"
        if k in ("v", "x") and not editable:
            return "break"
        w.event_generate({"c": "<<Copy>>", "v": "<<Paste>>", "x": "<<Cut>>"}[k])
        return "break"

    # ---- буфер обміну в hex-редакторі ----------------------------------
    #  Тут не можна просто копіювати текст віджета: панелі — це ВІДОБРАЖЕННЯ
    #  моделі edBytes, з роздільниками й вирівнюванням. Копіюємо байти під
    #  виділенням, а вставляємо — розібравши шістнадцяткові цифри з буфера
    #  просто в модель. Інакше вставка зсунула б колонки й зіпсувала дамп.
    def _hx_panel(self):
        """Панель, до якої зараз ставиться дія буфера обміну.

        Не через focus_get(): він каже правду лише коли вікно застосунку
        активне, а команду з контекстного меню якраз і викликають кліком, після
        якого фокус може бути де завгодно. Тому запам'ятовуємо панель самі —
        на кліку й на отриманні фокуса.
        """
        return self._hxLast if self._hxLast is not None else self.hxHex

    def _hx_byte_of(self, w, idx):
        """Індекс байта під позицією idx. На роздільнику — найближчий ліворуч."""
        try:
            r, c = map(int, w.index(idx).split("."))
        except Exception:
            return None
        if w is self.hxAsc:
            j = c if c < 8 else (c - 2 if c >= 10 else 7)
        else:
            bn = self._hx_byte_at(c)
            if bn is not None:
                j = bn[0]
            else:
                j = 0
                for k in range(15, -1, -1):
                    if self._hx_col(k) <= c:
                        j = k; break
        if j > 15:
            j = 15
        i = (r - 1) * 16 + j
        if i < 0:
            i = 0
        return i if i < len(self.edBytes) else (len(self.edBytes) - 1 if self.edBytes else None)

    def _hx_sel(self, w):
        """(перший, останній) байт виділення включно, або None."""
        try:
            a = self._hx_byte_of(w, "sel.first")
            b = self._hx_byte_of(w, "sel.last-1c")
        except tk.TclError:
            return None
        if a is None or b is None:
            return None
        return (a, b) if a <= b else (b, a)

    def hx_copy(self, whole=False):
        if not self.edBytes:
            return "break"
        w = self._hx_panel()
        sel = None if whole else self._hx_sel(w)
        a, b = sel if sel else (0, len(self.edBytes) - 1)
        data = self.edBytes[a:b + 1]
        if w is self.hxAsc and not whole:
            txt = "".join(chr(x) if 32 <= x < 127 else "." for x in data)
        else:
            # По 16 байт у рядку — так само, як на екрані, і так само читається
            # будь-яким іншим hex-редактором.
            rows = ["".join("%02X " % x for x in data[i:i + 16]).strip()
                    for i in range(0, len(data), 16)]
            txt = "\n".join(rows)
        self.root.clipboard_clear()
        self.root.clipboard_append(txt)
        self.status("Скопійовано байт: %d" % len(data))
        return "break"

    @staticmethod
    def clipboard_text(root):
        """Текст із буфера обміну.

        Простий clipboard_get() не завжди спрацьовує: у Windows текст із
        зовнішньої програми лежить у CF_UNICODETEXT, і Tk, попросивши STRING,
        отримує TclError. Тому пробуємо кілька форм по черзі.
        """
        for kind in (None, "STRING", "UTF8_STRING"):
            try:
                return root.clipboard_get() if kind is None else root.clipboard_get(type=kind)
            except tk.TclError:
                continue
        try:
            return root.selection_get(selection="CLIPBOARD")
        except tk.TclError:
            return ""

    @staticmethod
    def hex_from_text(raw):
        """Байти з довільного hex-тексту.

        ⚠️ Головна пастка — АДРЕСНИЙ СТОВПЕЦЬ. Копію дампа беруть із нашого ж
        перегляду («000: 8D F8 …  ....») або з будь-якого hex-редактора, і
        адреса — теж шістнадцяткова. Якщо просто вибрати з рядка всі hex-цифри,
        адреса підмішається в дані: байти поїдуть, а кількість цифр стане
        непарною — рівно та відмова «вставити не можу», яку видно на практиці.
        Тому спершу знімаємо з кожного рядка адресу (цифри до «:» чи «|») і
        ASCII-колонку (усе після двох пробілів у хвості).
        """
        out = []
        for line in (raw or "").replace("\r", "\n").split("\n"):
            # адреса з роздільником: «000:», «0x1F0:», «0100|»
            line = re.sub(r"^\s*(0[xX])?[0-9A-Fa-f]{2,8}\s*[:|]\s*", "", line)
            # адреса без роздільника, як у hexdump -C: «00000010  8D 8E …».
            # Вимагаємо ЩОНАЙМЕНШЕ 4 цифри й два пробіли — інакше під це
            # правило потрапив би звичайний байт, за яким стоїть подвійний
            # пробіл, і перший байт вставки мовчки зникав би.
            line = re.sub(r"^\s*[0-9A-Fa-f]{4,8}\s\s+", "", line)
            # ASCII-колонка праворуч. Відрізаємо її по двох і більше пробілах,
            # але ЛИШЕ якщо в хвості є символ, якого в hex-даних бути не може.
            # Інакше під це правило потрапляв би звичайний подвійний пробіл між
            # байтами («8D  8E 8F»), і все після нього мовчки зникало б.
            parts = re.split(r"\s{2,}", line.strip(), maxsplit=1)
            if len(parts) == 2 and re.search(r"[^0-9A-Fa-f\s]", parts[1]):
                line = parts[0]
            out.append(re.sub(r"0[xX]|[^0-9a-fA-F]", "", line))
        return "".join(out)

    def hx_paste(self):
        raw = self.clipboard_text(self.root)
        if not raw:
            self.status("Буфер обміну порожній"); return "break"
        if not self.edBytes:
            messagebox.showwarning("Вставка", "Спочатку натисніть «↻ Завантажити»."); return "break"
        # Приймаємо будь-яку розумну форму: «01 A5 FF», «0x01,0xA5», «01A5FF»,
        # рядки з адресним стовпцем і ASCII-колонкою, переноси й табуляції.
        digits = self.hex_from_text(raw)
        if not digits:
            messagebox.showwarning("Вставка", "У буфері немає шістнадцяткових даних."); return "break"
        if len(digits) % 2:
            messagebox.showwarning("Вставка",
                                   "Непарна кількість шістнадцяткових цифр (%d) — байти не складаються."
                                   % len(digits))
            return "break"
        data = bytes(int(digits[i:i + 2], 16) for i in range(0, len(digits), 2))

        w = self._hx_panel()
        sel = self._hx_sel(w)
        start = sel[0] if sel else (self._hx_byte_of(w, "insert") or 0)
        room = len(self.edBytes) - start
        if len(data) > room:
            # Довжину дампа міняти НЕ можна: чіп має рівно 512 (або 64) байт, і
            # запис іде строго з моделі. Тому питаємо, а не мовчки обрізаємо.
            if not messagebox.askyesno("Вставка",
                    "У буфері %d байт, а від позиції 0x%X лишилось %d.\n"
                    "Вставити перші %d байт?" % (len(data), start, room, room)):
                return "break"
            data = data[:room]
        self.edBytes[start:start + len(data)] = data
        self._hx_render()
        self._hx_setcur(w, start // 16 + 1,
                        self._hx_asc_col(start % 16) if w is self.hxAsc else self._hx_col(start % 16))
        self.status("Вставлено байт: %d з 0x%X" % (len(data), start))
        return "break"

    def hx_select_all(self):
        select_all(self._hx_panel())
        return "break"

    def _hx_menu(self, w):
        m = tk.Menu(w, tearoff=0)
        m.add_command(label="Копіювати виділене", command=self.hx_copy)
        m.add_command(label="Копіювати весь дамп", command=lambda: self.hx_copy(whole=True))
        m.add_command(label="Вставити", command=self.hx_paste)
        m.add_separator()
        m.add_command(label="Виділити все", command=self.hx_select_all)

        def on_rmb(e):
            self._hxLast = w
            w.focus_set()
            # Курсор пересуваємо ЛИШЕ без виділення — інакше клац правою по
            # виділеному знищував би те, що збираються копіювати.
            try:
                w.index("sel.first")
            except tk.TclError:
                w.mark_set("insert", "@%d,%d" % (e.x, e.y))
            popup_menu(m, e)
            return "break"
        w.bind("<Button-3>", on_rmb)

    # ---- масштабування вмісту ------------------------------------------
    def _bind_zoom(self):
        r = self.root
        r.bind("<Configure>", self._on_resize)
        for k in ("<Control-plus>", "<Control-equal>", "<Control-KP_Add>"):
            r.bind(k, lambda e: self.zoom_step(+0.1))
        for k in ("<Control-minus>", "<Control-KP_Subtract>"):
            r.bind(k, lambda e: self.zoom_step(-0.1))
        r.bind("<Control-0>", lambda e: self.zoom_set(1.0))
        # Ctrl+коліщатко. Прив'язка з модифікатором точніша за просту
        # <MouseWheel> зі _scroll_area, тож Tk обере саме її — прокрутка й
        # масштаб не заважають одне одному.
        r.bind_all("<Control-MouseWheel>", lambda e: self.zoom_step(0.1 if e.delta > 0 else -0.1))
        r.bind_all("<Control-Button-4>", lambda e: self.zoom_step(+0.1))   # Linux
        r.bind_all("<Control-Button-5>", lambda e: self.zoom_step(-0.1))
        r.bind("<F11>", lambda e: self.toggle_fullscreen())
        r.bind("<Escape>", lambda e: self.toggle_fullscreen(False))
        self._apply_zoom(self._auto_zoom(), force=True)

    def _auto_zoom(self):
        w = max(1, self.root.winfo_width())
        h = max(1, self.root.winfo_height())
        if w < 50 or h < 50:                    # вікно ще не розкладене
            return self.zoom
        return min(w / float(self.BASE_W), h / float(self.BASE_H))

    def _on_resize(self, e):
        # Подія спливає й від дочірніх віджетів — реагуємо лише на саме вікно.
        if e.widget is not self.root or not self.zoomAuto.get():
            return
        # Затримка: під час перетягування краю подія летить десятками разів,
        # а перерахунок шрифтів — робота не безкоштовна.
        if self._zoomJob:
            self.root.after_cancel(self._zoomJob)
        self._zoomJob = self.root.after(180, lambda: self._apply_zoom(self._auto_zoom()))

    def zoom_step(self, d):
        self.zoomAuto.set(False)                # руками — значить руками
        self.zoom_set(self.zoom + d)

    def zoom_set(self, k):
        self.zoomAuto.set(False)
        self._apply_zoom(k, force=True)

    def zoom_auto_now(self):
        if self.zoomAuto.get():
            self._apply_zoom(self._auto_zoom(), force=True)

    def _apply_zoom(self, k, force=False):
        k = max(self.ZOOM_MIN, min(self.ZOOM_MAX, k))
        # Поріг потрібен не лише щоб не смикати інтерфейс: зміна шрифтів може
        # трохи посунути розмір вікна, звідки прилетить нова <Configure>.
        # Без порога це замкнулося б у петлю «більший шрифт -> більше вікно».
        if not force and abs(k - self.zoom) < 0.05:
            return
        self.zoom = k
        fonts_rescale(k)
        # Відступи ttk у шрифт не входять — масштабуємо окремо, інакше на 200 %
        # великий текст сидів би у вузьких кнопках і тісних вкладках.
        try:
            st = ttk.Style()
            st.configure("TButton", padding=max(2, int(round(5 * k))))
            st.configure("TNotebook.Tab", padding=(int(round(12 * k)), int(round(6 * k))))
        except tk.TclError:
            pass
        if hasattr(self, "monDis"):
            self.monDis.set_scale(k)
        if hasattr(self, "lblZoom"):
            self.lblZoom.config(text="%d %%" % round(k * 100))

    def toggle_fullscreen(self, on=None):
        self._fullscreen = (not self._fullscreen) if on is None else on
        try:
            self.root.attributes("-fullscreen", self._fullscreen)
        except tk.TclError:
            pass                                # деякі середовища не вміють
        self.zoom_auto_now()

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
        # Другий рядок: стан ліворуч, керування масштабом праворуч. Верхній
        # рядок для масштабу не годиться — там і так тісно, і на 175 % поле
        # пароля вичавлювало за край.
        bar = ttk.Frame(self.root); bar.pack(fill="x")
        # Масштаб вмісту. «Авто» (типово) веде розмір за розміром вікна: тягнеш
        # край або розгортаєш на весь екран — і все росте разом. Кнопки ± та
        # Ctrl+коліщатко перемикають на ручний режим, Ctrl+0 — рівно 100 %,
        # F11 — на весь екран. Пакується ПЕРШИМ: у pack хто раніше, той і
        # отримує місце, а керування масштабом має лишатись досяжним завжди.
        zf = ttk.Frame(bar); zf.pack(side="right", padx=(6, 8))
        ttk.Checkbutton(zf, text="авто", variable=self.zoomAuto,
                        command=self.zoom_auto_now).pack(side="right", padx=(6, 0))
        ttk.Button(zf, text="+", width=3, command=lambda: self.zoom_step(+0.1)).pack(side="right")
        self.lblZoom = ttk.Label(zf, text="100 %", width=6, anchor="center")
        self.lblZoom.pack(side="right")
        ttk.Button(zf, text="−", width=3, command=lambda: self.zoom_step(-0.1)).pack(side="right")
        ttk.Label(zf, text="Масштаб:").pack(side="right", padx=(0, 4))

        self.lblStatus = ttk.Label(bar, text="Не підключено", foreground="#a00", padding=(8, 0))
        self.lblStatus.pack(side="left", fill="x", expand=True)

        # ── АВАРІЯ ЖИВЛЕННЯ +14 В ─────────────────────────────────────────
        # Над вкладками, а не всередині «Заряду»: без блока живлення заряд не
        # піде взагалі, і дізнатись про це лише відкривши потрібну вкладку
        # означало б дізнатись запізно. Пакується ДО Notebook, щоб лишалась
        # угорі й не з'їдалась розтягуванням вкладок.
        self.psuBar = tk.Frame(self.root, bg=MIL["maroon"])
        self.lblPsu = tk.Label(self.psuBar, text="", bg=MIL["maroon"], fg="#ffffff",
                               font=fnt("Segoe UI", 10, "bold"), justify="left",
                               anchor="w", padx=10, pady=6)
        self.lblPsu.pack(fill="x")
        self._psuBlink = False        # фаза блимання
        self._psuShown = False        # чи смуга зараз на екрані

        nb = ttk.Notebook(self.root)
        self.nb = nb                  # потрібен смузі аварії живлення (pack before=)
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
        v = ttk.Label(parent, text="—", font=fnt("Segoe UI", 10, "bold"))
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
        ttk.Label(top, text="🧙 Майстер відновлення", font=fnt("Segoe UI", 11, "bold")).pack(side="left")
        ttk.Button(top, text="🔍 Аналізувати", command=self.wiz_analyze).pack(side="right", padx=3)
        ttk.Button(top, text="↺ Скинути", command=self.wiz_reset).pack(side="right", padx=3)
        ttk.Label(f, text="Аналіз стану → проблеми → пропозиції → покрокове виконання. Багатоетапні\n"
                         "сценарії з зарядною станцією продовжуються після повернення АКБ.",
                  foreground="#b9bd86", justify="left").pack(anchor="w", pady=(2, 6))

        self.wizVerdict = ttk.Label(f, text="Натисніть «Аналізувати».", font=fnt("Segoe UI", 10, "bold"))
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
            ttk.Label(row, text=head, font=fnt("Segoe UI", 9, "bold")).pack(anchor="w")
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
                ttk.Label(txt, text=p.get("problem", ""), font=fnt("Segoe UI", 9, "bold"),
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
            # Крок Майстра — теж запис: кажемо, у яку мікросхему він піде.
            ttl = s.get("title", "")
            if s.get("chips") and s.get("chipsText"):
                ttl += "   ·  пише в " + s["chipsText"]
            ttk.Label(box, text=ttl, font=fnt("Segoe UI", 9, "bold" if cur else "normal"),
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
        # Майстер бере ТІ САМІ правки, що й картка «Відновити еталон»: інакше
        # галочки (наробіток, шунт, ємність) для нього нічого б не значили.
        cmd = "WIZSTEP %d%s%s TAIL=%s" % (idx, (" " + model) if model else "",
                                          self._rp_fixes_arg(), self._tail_mode())
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
        self.dRs = self._kv(box, "Шунт вимірювача:", 10)
        self.dSerial33 = self._kv(box, "Серійний DS2433:", 11)

        # Штатні поля Motorola. Читаються тим самим алгоритмом, що й фірмове ПЗ
        # (звірено на 53 еталонних пакетах). Цикли лежать у гістограмі й ключа
        # не потребують; знос, дати й калібрування зашифровані ключем із ROM-ID
        # чипа DS2433 — саме тому раніше ці поля здавалися відсутніми.
        bbox = ttk.LabelFrame(f, text="Штатні лічильники Motorola", padding=8)
        bbox.pack(fill="x", pady=(8, 0))
        self.bWarn = ttk.Label(bbox, text="", foreground="#c0392b", wraplength=560, justify="left")
        self.bWarn.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 4))
        self.bWarn.grid_remove()
        self.bCyc = self._kv(bbox, "Циклів заряду (IMPRES):", 1)
        self.bCycN = self._kv(bbox, "Циклів не-IMPRES:", 2)
        self.bPot = self._kv(bbox, "Реальна ємність:", 3)
        self.bHealth = self._kv(bbox, "Знос / здоров'я:", 4)
        self.bCal = self._kv(bbox, "Калібрувань пройдено:", 5)
        self.bMfg = self._kv(bbox, "Дата виготовлення:", 6)
        self.bUse = self._kv(bbox, "Перше користування:", 7)
        self.bKey = self._kv(bbox, "Ключ:", 8)
        ttk.Label(f, text="Дамп DS2433 (512 Б):").pack(anchor="w", pady=(8, 0))
        self.tx33 = scrolledtext.ScrolledText(f, height=6, font=fnt("Consolas", 8), wrap="none",
                                              bg=MIL["field"], fg="#b9bd86", insertbackground=MIL["khaki"],
                                              relief="flat", bd=0); self.tx33.pack(fill="both", expand=True)
        ttk.Label(f, text="Дамп DS2438 (64 Б):").pack(anchor="w", pady=(6, 0))
        self.tx38 = scrolledtext.ScrolledText(f, height=3, font=fnt("Consolas", 8), wrap="none",
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
        # Сторінка була на 15 блоків в один скрол, і знайти потрібний можна було
        # лише прокруткою. Розкладаємо по під-вкладках у тому ж порядку, що й у
        # вебі: ремонт -> калібрування -> значення -> еталон -> експерт ->
        # небезпечна зона. Налаштування пристрою й звук — окрема вкладка.
        nbf = ttk.Notebook(self.tabFw); nbf.pack(fill="both", expand=True)
        def _sub(title):
            f = ttk.Frame(nbf, padding=6); nbf.add(f, text=title)
            return self._scroll_area(f)
        p_rep  = _sub("🩹 Ремонт")
        p_cal  = _sub("🔌 Калібрування")
        p_val  = _sub("🎚 Значення")
        p_id   = _sub("🪪 Еталон і модель")
        p_exp  = _sub("🧪 Експерт")
        p_dang = _sub("⛔ Небезпечно")
        p_cfg  = _sub("⚙️ Налаштування")

        b1 = ttk.LabelFrame(p_rep, text="Крок 1 — резервна копія (робіть ЗАВЖДИ перед записом)", padding=8); b1.pack(fill="x", pady=4)
        ttk.Button(b1, text="🔍 Зчитати АКБ", command=self.do_read).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2433", command=lambda: self.save_dump("GET33", 512, "ds2433.bin")).pack(side="left", padx=3)
        ttk.Button(b1, text="⬇ Копія DS2438", command=lambda: self.save_dump("GET38", 64, "ds2438.bin")).pack(side="left", padx=3)

        b2b = ttk.LabelFrame(p_rep, text="Крок 2 — РЕМОНТ. Після заміни елементів починайте звідси  ·  пише в DS2433 + DS2438", padding=8); b2b.pack(fill="x", pady=4)
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
        ttk.Label(b2b, text="🔌 Якщо чіп стертий, а зарядна станція WPLN4226A сама встигла дописати\n"
                            "дзеркало заголовка з DS2438 (суму не виправила) — кнопка нижче добудовує\n"
                            "рівно це; профіль і модель цим не відновлюються.",
                  foreground="#b9bd86", justify="left").pack(anchor="w", pady=(4, 0))
        ttk.Button(b2b, text="🔌 Добудувати заголовок після станції",
                   command=self.hdr_fix).pack(anchor="w", pady=3)

        b2d = ttk.LabelFrame(p_cal, text="Розряд перед калібруванням (навантаження MOSFET)  ·  пише в DS2438", padding=8); b2d.pack(fill="x", pady=4)
        ttk.Label(b2d, text="Розряд — це приймальний контроль після перепайки: він міряє реальну ємність нових\n"
                            "банок. Для калібрування він НЕ обов'язковий — фірмова станція бере в цикл навіть\n"
                            "повністю заряджений пакет, якщо він оригінальний. Але якщо станція вперлась і не\n"
                            "переходить у калібрування, часткова розрядка це підштовхує.\n"
                            "Аварійна зупинка: < 6.00 В, перегрів 45 °C, стеля часу, втрата зв'язку з монітором,\n"
                            "зависання головного циклу. Резистор гріється — не лишайте без нагляду.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        # Ціль розряду обирає користувач; лінійка струму будується під неї —
        # 300 мА припадають рівно на обрану напругу, а не завжди на 7.20 В.
        tf = ttk.Frame(b2d); tf.pack(anchor="w", pady=(4, 0))
        ttk.Label(tf, text="Розряджати до:").pack(side="left")
        self.disTarget = tk.IntVar(value=7200)
        for mv in (7800, 7600, 7400, 7200, 7000):
            ttk.Radiobutton(tf, text="%.2f В" % (mv / 1000.0), value=mv,
                            variable=self.disTarget,
                            command=self._dis_ramp_note).pack(side="left", padx=2)
        ttk.Label(tf, text="або, мВ:").pack(side="left", padx=(8, 2))
        self.eDisTarget = ttk.Entry(tf, width=6)
        self.eDisTarget.pack(side="left")
        self.eDisTarget.bind("<KeyRelease>", lambda e: self._dis_ramp_note())
        self.lblDisRamp = ttk.Label(b2d, text="", foreground="#b9bd86", justify="left")
        self.lblDisRamp.pack(anchor="w", pady=(2, 0))
        # Графік уставки струму за напругою — перемальовується щоразу, коли
        # змінюють ціль: видно, що 300 мА припадають рівно на неї.
        self.cvDisRamp = tk.Canvas(b2d, width=420, height=96, highlightthickness=1,
                                   highlightbackground=MIL["line"], bg=MIL["field"])
        self.cvDisRamp.pack(anchor="w", pady=(4, 0))
        self._dis_ramp_note()

        df = ttk.Frame(b2d); df.pack(anchor="w", pady=3)
        ttk.Button(df, text="🪫 Почати розряд", command=self.discharge_start).pack(side="left", padx=2)
        ttk.Button(df, text="⏹ Зупинити", command=self.discharge_stop).pack(side="left", padx=2)
        ttk.Button(df, text="🔄 Оновити зараз", command=self.discharge_status).pack(side="left", padx=2)
        # Стан тягнеться сам (див. _dis_tick) — кнопка лишилась тільки щоб не
        # чекати періоду, коли й так стоїш біля пристрою.
        self.monDis = DischargeMonitor(b2d); self.monDis.pack(anchor="w", pady=(6, 0))

        b2e = ttk.LabelFrame(p_cal, text="Заряд через DC/DC (готова плата на TL494)  ·  пише в DS2438", padding=8); b2e.pack(fill="x", pady=4)
        ttk.Label(b2e, text="Керований заряд: enable-каскад + аналогове керування вихідною напругою (ШІМ+RC).\n"
                            "Профіль струму масштабується під обрану ціль — заряд завжди закінчується плавним\n"
                            "спадом струму перед самою ціллю, хай яку обрано.\n"
                            "Пакет не прийме струм, доки не піднято enable-сигнал (та сама лінія, що й для читання/\n"
                            "запису пам'яті) — прошивка тримає його сама весь час заряду.\n"
                            "Аварійна зупинка: напруга вище межі ОБРАНОЇ цілі, перегрів 45 °C, стеля часу 6 год,\n"
                            "втрата зв'язку з монітором. Заряд і розряд не можуть іти одночасно.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        # Ціль заряду у % — той самий принцип, що й ціль розряду (self.disTarget),
        # лише пресети у відсотках замість мілівольт.
        tfc = ttk.Frame(b2e); tfc.pack(anchor="w", pady=(4, 0))
        ttk.Label(tfc, text="Заряджати до:").pack(side="left")
        self.chgTarget = tk.IntVar(value=100)
        for pct in (100, 95, 90, 85, 80):
            ttk.Radiobutton(tfc, text="%d%%" % pct, value=pct,
                            variable=self.chgTarget).pack(side="left", padx=2)
        ttk.Label(tfc, text="або, %:").pack(side="left", padx=(8, 2))
        self.eChgTarget = ttk.Entry(tfc, width=4)
        self.eChgTarget.pack(side="left")
        cf = ttk.Frame(b2e); cf.pack(anchor="w", pady=3)
        ttk.Button(cf, text="🔋 Почати заряд", command=self.charge_start).pack(side="left", padx=2)
        ttk.Button(cf, text="⏹ Зупинити", command=self.charge_stop).pack(side="left", padx=2)
        ttk.Button(cf, text="🔄 Оновити зараз", command=self.charge_status).pack(side="left", padx=2)
        self.monChg = ChargeMonitor(b2e); self.monChg.pack(anchor="w", pady=(6, 0))

        b2c = ttk.LabelFrame(p_cal, text="Крок 3 — калібрування на IMPRES-ЗП (обов'язково)", padding=8); b2c.pack(fill="x", pady=4)
        ttk.Label(b2c, text="Після ремонту навчена калібровка порожня — рація приймає пакет як фірмовий і просить\n"
                            "калібрування. Поставте АКБ на оригінальну IMPRES-ЗП на повний цикл (заряд → розряд →\n"
                            "заряд): саме станція виміряє нові банки й запише калібровку в 0x18A–0x1FF.\n"
                            "Прошивкою цей крок не замінюється.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")

        b2 = ttk.LabelFrame(p_cal, text="Обслуговування (безпечно для ідентичності)  ·  пише в DS2433 + DS2438", padding=8); b2.pack(fill="x", pady=4)
        ttk.Button(b2, text="♻️ Скидання лічильників", command=lambda: self.simple_op("RESET", "Обнулити лічильники DS2438 (ETM/CCA/DCA)?\nНавчену калібровку й ідентичність не чіпає.")).pack(side="left", padx=3)
        ttk.Button(b2, text="🧹 Очистити дані (лишити ID/калібр.)", command=lambda: self.simple_op("CLEAN", "Стерти дані використання, лишивши ID/калібрування?")).pack(side="left", padx=3)

        # Ємність і відсоток — ДВІ окремі операції. Пишуть вони в один і той
        # самий регістр ICA, але це різні задачі: точний залишок у мА·год
        # (як його рахує Motorola) і груба оцінка у відсотках.
        b5 = ttk.LabelFrame(p_val, text="🔋 Ємність — внести залишок у мА·год  ·  пише в DS2438",
                            padding=8); b5.pack(fill="x", pady=4)
        ttk.Label(b5, text="Залишок ЗАРЯДУ в паливомірі (регістр ICA), одиниця = 0.4882 мВ·год / шунт\n"
                           "цього пакета. Це не паспортна ємність (вона в DS2433, 0x008) і не знос.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.eMah = self._row(b5, "Залишок, мА·год:", lambda fr: self._entry(fr, 10, "0"))
        ttk.Button(b5, text="💾 Записати мА·год", command=self.set_mah).pack(anchor="w", pady=2)

        b5p = ttk.LabelFrame(p_val, text="⚡ Рівень заряду у відсотках  ·  пише в DS2438",
                             padding=8); b5p.pack(fill="x", pady=4)
        ttk.Label(b5p, text="Той самий регістр ICA, але у відсотках від паспортної ємності —\n"
                            "коли точних мА·год немає. Станція згодом уточнить значення сама.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.eChg = self._row(b5p, "Заряд, %:", lambda fr: self._entry(fr, 10, ""))
        cf = ttk.Frame(b5p); cf.pack(anchor="w", pady=2)
        # Підпис зі шкалою ставить ПРИСТРІЙ (поле scaleTxt у відповіді INFO):
        # тримати тут власну копію чисел означає рано чи пізно почати брехати —
        # саме так у діалозі нижче до останнього висіли 7.0/8.4 В.
        self.btnChgAuto = ttk.Button(cf, text="⚡ За напругою",
                                     command=self.set_charge_auto)
        self.btnChgAuto.pack(side="left", padx=2)
        ttk.Button(cf, text="💾 Записати заряд %", command=self.set_charge_pct).pack(side="left", padx=2)

        # Знос — те число, яке рація й фірмове ПЗ справді показують (поле CTS у
        # зашифрованому блоці RECOND). Байт заводської таблиці 0x129 — не воно;
        # він лишився в «Ручному режимі» для аналізу.
        b5h = ttk.LabelFrame(p_val, text="🩺 Знос / здоров'я  ·  пише в DS2433 (CTS у блоці RECOND)",
                             padding=8); b5h.pack(fill="x", pady=4)
        ttk.Label(b5h, text="Після заміни елементів виправляти треба саме це число. Знос залежить і від\n"
                            "ШУНТА, і від ПАСПОРТНОЇ ЄМНОСТІ: 100 % — це «вся паспортна», тож спершу\n"
                            "задайте ємність нових банок у «Ремонті». Ключ — з ROM-ID чипа DS2433.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.lblHpNow = ttk.Label(b5h, text="зараз у чипі: —", foreground="#c8b04a")
        self.lblHpNow.pack(anchor="w", pady=(2, 0))
        self.eHp = self._row(b5h, "Знос, % (1..100):", lambda fr: self._entry(fr, 10, ""))
        ttk.Button(b5h, text="💾 Записати знос", command=self.set_health).pack(anchor="w", pady=2)

        b5c = ttk.LabelFrame(p_val, text="Дата першого використання (рація рахує як «час − ETM»)  ·  пише в DS2438", padding=8); b5c.pack(fill="x", pady=4)
        self.eEtmDate = self._row(b5c, "Дата (YYYY-MM-DD):", lambda fr: self._entry(fr, 12))
        ttk.Button(b5c, text="📅 Записати дату (ETM)", command=self.set_etm).pack(anchor="w", pady=2)

        b3 = ttk.LabelFrame(p_id, text="Ідентичність — модель  ·  пише в DS2433", padding=8); b3.pack(fill="x", pady=4)
        self.eModel = self._row(b3, "Модель (3–9, A–Z0–9):", lambda fr: self._entry(fr, 12))
        ttk.Button(b3, text="💾 Записати модель", command=self.set_model).pack(anchor="w", pady=2)

        b4r = ttk.LabelFrame(p_id, text="🛠️ Відновити модельну частину еталона  ·  пише в DS2433 (+ DS2438, якщо є еталон монітора)", padding=8); b4r.pack(fill="x", pady=4)
        self.cbRest = self._row(b4r, "Модель-еталон:", lambda fr: self._combo(fr, 18))
        ttk.Label(b4r, text="Пише ідентичність 0x000–0x065, криву, COPYRIGHT, заводську таблицю й запис моделі.\n"
                           "Навчений хвіст 0x18A–0x1FF лишається порожнім — його запише зарядна станція.\n"
                           "Працює й на порожній/битій мікросхемі.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        # ---- правки під ЦЕЙ пакет -------------------------------------
        #  Еталон — побайтова копія ОДНОГО акумулятора, і разом із моделлю він
        #  несе числа донора: його рівень заряду, його шунт, його калібрування
        #  нуля АЦП. Показуємо їх поруч із реальними числами пакета, щоб
        #  користувач вирішував, а не довіряв наосліп.
        self.frRp = ttk.LabelFrame(b4r, text="🔎 Правки під ЦЕЙ пакет — перевірте перед записом", padding=6)
        ttk.Label(self.frRp, foreground="#b9bd86", justify="left",
                  text="Зніміть галочку, щоб лишити число еталона.").pack(anchor="w")
        self.lblRpWarn = ttk.Label(self.frRp, foreground="#ffd9a8", justify="left", wraplength=640)
        self.rpGrid = ttk.Frame(self.frRp); self.rpGrid.pack(fill="x", pady=(4, 2))
        self.lblRpSum = ttk.Label(self.frRp, foreground="#b9bd86", justify="left", wraplength=640)
        self.lblRpSum.pack(anchor="w", pady=(4, 0))
        rpb = ttk.Frame(self.frRp); rpb.pack(anchor="w", pady=(4, 0))
        ttk.Button(rpb, text="🔄 Перечитати акумулятор",
                   command=lambda: self.rp_load(True)).pack(side="left", padx=(0, 6))
        ttk.Button(rpb, text="💾 Записати лише правки",
                   command=self.rp_write).pack(side="left")
        ttk.Label(self.frRp, foreground="#b9bd86", justify="left", wraplength=640,
                  text="«Записати лише правки» міняє рівно обрані поля й не чіпає ні ідентичність,\n"
                       "ні навчену калібровку, ні лічильники CCA/DCA. Повне «Відновити модельну\n"
                       "частину» — коли треба перезаписати й еталон.").pack(anchor="w", pady=(2, 0))
        self.rpVars, self.rpPlan = {}, None

        # Режим навченого хвоста 0x18A..0x1FF. «Свіжий» лишає скелет записів із
        # нулями — калібрування на ЗП здатне завершитись. «Стерти» — саме той
        # стан, у якому ЗП починала ЗАРЯДЖАТИ (dumps/11,12,13), але навчений
        # блок може так і не стати валідним.
        frTail = ttk.Frame(b4r); frTail.pack(anchor="w", pady=(4, 0))
        ttk.Label(frTail, text="Навчений хвіст:", foreground="#b9bd86").pack(side="left")
        self.cbTail = ttk.Combobox(frTail, values=list(TAIL_MODES), state="readonly", width=26)
        self.cbTail.set(TAIL_MODES[0])
        self.cbTail.pack(side="left", padx=4)
        ttk.Label(b4r, foreground="#b9bd86", justify="left", wraplength=640,
                  text="«Стерти в 0xFF» — пробуйте, якщо ЗП світить зеленим і не заряджає: саме в\n"
                       "цьому стані вона починала заряджати й переходити в калібрування. Ціна —\n"
                       "калібрування може не завершитись.").pack(anchor="w", pady=(2, 2))

        ttk.Button(b4r, text="🛠️ Відновити модельну частину (DS2433+DS2438)", command=self.restore_battery).pack(anchor="w", pady=2)
        ttk.Button(b4r, text="🧪 Байт-у-байт (ручний режим, для аналізу)", command=self.restore_battery_verbatim).pack(anchor="w", pady=2)

        b4 = ttk.LabelFrame(p_id, text="🆕 Новий акумулятор (порожній чип)  ·  пише в DS2433 + DS2438", padding=8); b4.pack(fill="x", pady=4)
        ttk.Label(b4, text="З еталона береться лише МОДЕЛЬНА частина. Ідентичність не копіюється, а\n"
                           "ГЕНЕРУЄТЬСЯ з ROM-ID саме цього чипа: зашифровані поля еталона зашифровані\n"
                           "ROM-ом донора, і рація прочитала б їх як сміття («невідомий акумулятор»).\n"
                           "Дата виготовлення — сьогоднішня, пакет ще не ввімкнений. Чип має бути\n"
                           "ПРОЧИТАНИЙ: без ROM-ID генерувати нема з чого.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.cbInit = self._row(b4, "Модель-еталон:", lambda fr: self._combo(fr, 18))
        self.eInitMah = self._row(b4, "Заряд, мА·год:", lambda fr: self._entry(fr, 10, "1000"))
        ttk.Button(b4, text="🆕 Записати новий АКБ (DS2433+DS2438)", command=self.init_battery).pack(anchor="w", pady=2)

        b8 = ttk.LabelFrame(p_exp, text="🧪 Ручний режим / експерт  ·  пише в ту мікросхему, яку обрано в редакторі", padding=8); b8.pack(fill="x", pady=4)
        ttk.Label(b8, text="Строк служби в прошивці НЕ зберігається — рація рахує його сама. Поле нижче править\n"
                           "байт у ЗАВОДСЬКІЙ таблиці моделі (0x129) і показань станції не змінить.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.eCap = self._row(b8, "Байт заводської таблиці, %:", lambda fr: self._entry(fr, 10, "100"))
        ttk.Button(b8, text="💾 Записати %", command=self.set_cap).pack(anchor="w", pady=2)

        # Крайній засіб: відновлення за зразком копії. У копій уся потрібна рації
        # інформація живе в DS2438 (сторінки 3..6 — дзеркало заголовка DS2433, і
        # серед них байт паспортної ємності), тому вони працюють із порожнім
        # DS2433. Повторюємо це.
        bcl = ttk.LabelFrame(p_dang, text="🧬 Відновлення за зразком копії  ·  крайній засіб",
                             padding=8); bcl.pack(fill="x", pady=4)
        ttk.Label(bcl, text="Коли жодна інша спроба не вдалася. Монітор пишеться зі зразка копії,\n"
                            "лічильники в нуль, паливомір — із поточної напруги, DS2433 стирається.\n"
                            "Ідентичність у DS2433 — окремою галочкою й лише як ЕКСПЕРИМЕНТ: каркас\n"
                            "береться з еталона родини 4409, а дані шифруються ключем із ROM цього чипа.",
                  foreground="#d08a3a", justify="left").pack(anchor="w")
        self.cbClSample = self._row(bcl, "Вбудований зразок:", lambda fr: self._combo(fr, 32))
        self.cbClSample["values"] = ["— свій файл —"]
        self.cbClSample.set("— свій файл —")
        self.cbClSample.bind("<<ComboboxSelected>>", lambda _e: self._clone_sample_pick())
        cf = ttk.Frame(bcl); cf.pack(fill="x", pady=2)
        ttk.Button(cf, text="📂 Обрати дамп DS2438 (64 Б)",
                   command=self.clone_pick).pack(side="left", padx=3)
        self.lblClone = ttk.Label(cf, text="не обрано", foreground="#b9bd86")
        self.lblClone.pack(side="left", padx=4)
        self.eClRated = self._row(bcl, "Ємність, мА·год:", lambda fr: self._entry(fr, 10, ""))
        self.cbClRs = self._row(bcl, "Шунт:", lambda fr: self._combo(fr, 26))
        self.cbClRs["values"] = list(CLONE_SHUNTS)
        self.cbClRs.set(CLONE_SHUNTS[0])
        self.eClModel = self._row(bcl, "Модель:", lambda fr: self._entry(fr, 12, ""))
        self.eClMfg = self._row(bcl, "Дата виготовлення (РРРР-ММ-ДД):", lambda fr: self._entry(fr, 12, ""))
        self.eClUse = self._row(bcl, "Дата першого запуску:", lambda fr: self._entry(fr, 12, ""))
        self.eClHp = self._row(bcl, "Знос, %:", lambda fr: self._entry(fr, 6, ""))
        self.vClZero = tk.BooleanVar(value=True)
        ttk.Checkbutton(bcl, text="Скинути лічильники (наробіток, CCA, DCA) — числа копії до ваших банок не стосуються",
                        variable=self.vClZero).pack(anchor="w")
        self.vClRecheck = tk.BooleanVar(value=True)
        ttk.Checkbutton(bcl, text="Перевірити заряд після запису — до запису в чипі стояв шунт копії",
                        variable=self.vClRecheck).pack(anchor="w")
        self.vClId33 = tk.BooleanVar(value=False)
        ttk.Checkbutton(bcl, text="Записати ЕКСПЕРИМЕНТАЛЬНУ ідентичність у DS2433 (каркас 4409)",
                        variable=self.vClId33).pack(anchor="w", pady=(2, 0))
        ttk.Button(bcl, text="🧬 Відновити за зразком копії",
                   command=self.clone_restore).pack(anchor="w", pady=2)

        b6 = ttk.LabelFrame(p_dang, text="⛔ Небезпечна зона (незворотно!)  ·  стирає обрану мікросхему повністю", padding=8); b6.pack(fill="x", pady=4)
        rf = ttk.Frame(b6); rf.pack(fill="x", pady=2)
        ttk.Button(rf, text="📤 Записати DS2433 з .bin (512 Б)", command=lambda: self.write_file(512, "WRITE33")).pack(side="left", padx=3)
        ttk.Button(rf, text="🔬 DS2438 з .bin (64 Б)", command=lambda: self.write_file(64, "WRITE38")).pack(side="left", padx=3)
        ttk.Button(b6, text="🔥 ПОВНЕ стирання DS2433", command=self.wipe33).pack(anchor="w", pady=2)
        ttk.Button(b6, text="🔥 ПОВНЕ стирання DS2438", command=self.wipe38).pack(anchor="w", pady=2)

        # Дата пристрою. Годинника реального часу немає, NTP недосяжний (пристрій
        # сам — точка доступу), тож єдине джерело — цей ПК. Показуємо, що
        # пристрій думає, і даємо виправити одним рухом.
        bclk = ttk.LabelFrame(p_cfg, text="🕐 Дата пристрою  ·  в АКБ не пише", padding=8)
        bclk.pack(fill="x", pady=4)
        ttk.Label(bclk, text="З неї рахується наробіток там, де клієнта немає — у меню самого\n"
                             "приладу й у Майстрі з екрана. Зберігається й переживає перезавантаження.",
                  foreground="#b9bd86", justify="left").pack(anchor="w")
        self.lblClk = ttk.Label(bclk, text="пристрій вважає, що сьогодні: —", foreground="#c8b04a")
        self.lblClk.pack(anchor="w", pady=(2, 0))
        ttk.Button(bclk, text="🕐 Синхронізувати з цим ПК",
                   command=self.clock_sync).pack(anchor="w", pady=2)

        self._build_sound(p_cfg)

        b7 = ttk.LabelFrame(p_cfg, text="Пристрій", padding=8); b7.pack(fill="x", pady=4)
        ttk.Button(b7, text="🔁 Перезавантажити ESP32", command=self.reboot).pack(side="left", padx=3)

    # ---- налаштування звуку ---------------------------------------------
    #  Нічого не пише в акумулятор: це налаштування самого пристрою. Межі
    #  повзунків приходять із прошивки (SOUND -> limits) — власна копія тут
    #  розійшлася б із buzzCfgClamp() після наступної правки firmware.
    SND_FIELDS = [
        # (ключ у JSON, ключ команди, підпис, крок, як показати значення)
        ("volume",    "vol",   "Гучність",         1, lambda v: "%d / 255" % v),
        ("tempo",     "tempo", "Швидкість",        5, lambda v: "%d %%" % v),
        ("glide",     "glide", "Перетікання нот",  5, lambda v: "вимкн." if v == 0 else "%d %%" % v),
        ("attack",    "atk",   "Наростання",       2, lambda v: "%d мс" % v),
        ("release",   "rel",   "Згасання",         5, lambda v: "%d мс" % v),
        ("semitones", "st",    "Висота тону",      1,
         lambda v: "заводська" if v == 0 else "%+d пів." % v),
    ]

    def _build_sound(self, p):
        bs = ttk.LabelFrame(p, text="🔊 Налаштування звуку  ·  лише пристрій, в АКБ не пише",
                            padding=8)
        bs.pack(fill="x", pady=4)
        ttk.Label(bs, text="Зберігається в пам'яті пристрою й переживає перезавантаження. Кожен п'єзо\n"
                           "має свій резонанс: на одному звук м'який, на іншому ледь чутний. Крутіть\n"
                           "повзунок і одразу тисніть «прослухати».",
                  foreground="#b9bd86", justify="left").pack(anchor="w")

        sw = ttk.Frame(bs); sw.pack(anchor="w", pady=(6, 2))
        self.sndEn = tk.BooleanVar(value=True)
        self.sndClk = tk.BooleanVar(value=True)
        ttk.Checkbutton(sw, text="Звук увімкнено", variable=self.sndEn,
                        command=self._snd_push).pack(side="left", padx=(0, 14))
        ttk.Checkbutton(sw, text="Блiп при перегортанні меню", variable=self.sndClk,
                        command=self._snd_push).pack(side="left")

        grid = ttk.Frame(bs); grid.pack(fill="x", pady=2)
        grid.columnconfigure(1, weight=1)
        self.sndVar, self.sndScale, self.sndLbl = {}, {}, {}
        for i, (key, _ck, title, step, fmt) in enumerate(self.SND_FIELDS):
            ttk.Label(grid, text=title, width=18).grid(row=i, column=0, sticky="w", pady=1)
            var = tk.IntVar(value=0)
            # Повзунок Tk віддає float — округлюємо до кроку самі, інакше в
            # команду поїхало б «tempo=137.4999», і пристрій прочитав би 137.
            sc = tk.Scale(grid, from_=0, to=100, orient="horizontal", variable=var,
                          resolution=step, showvalue=False, sliderlength=18, width=11,
                          highlightthickness=0, bd=0, takefocus=1,
                          # tk.Scale не тематизується через ttk.Style — фарбуємо руками,
                          # інакше на темному тлі буде світло-сіра пляма.
                          bg=MIL["bg"], fg=MIL["fg"], troughcolor=MIL["field"],
                          activebackground=MIL["khaki"], relief="flat")
            sc.grid(row=i, column=1, sticky="we", padx=8)
            sc.configure(command=lambda _v, k=key: self._snd_show(k))
            sc.bind("<ButtonRelease-1>", lambda _e: self._snd_push())
            sc.bind("<KeyRelease>", lambda _e: self._snd_push())
            lbl = ttk.Label(grid, text="—", width=11, anchor="e",
                            font=fnt("Consolas", 10, "bold"))
            lbl.grid(row=i, column=2, sticky="e")
            self.sndVar[key], self.sndScale[key], self.sndLbl[key] = var, sc, lbl

        self.lblSndHint = ttk.Label(
            bs, foreground="#b9bd86", justify="left",
            text="Швидкість розтягує і ноти, і перетікання разом, тож повільна фраза лишається злитою.\n"
                 "Перетікання 0 % — ноти перемикаються стрибком, 200–300 % — довге ковзання. Висота\n"
                 "тону зсуває всю мелодію: якщо п'єзо на заводських частотах тихий, підніміть її на\n"
                 "кілька півтонів і знайдіть його резонанс на слух.")
        self.lblSndHint.pack(anchor="w", pady=(6, 2))

        self.frSndTest = ttk.Frame(bs); self.frSndTest.pack(fill="x", pady=2)
        ttk.Button(bs, text="↩️ Заводські значення",
                   command=self.sound_reset).pack(anchor="w", pady=(4, 0))

    def _build_hex(self):
        f = self.tabHex
        mono = fnt("Consolas", 10)
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
        # Ctrl+C/V/A ці ж обробники пропускають до буфера обміну (див. _hx_key).
        self.hxHex.bind("<Key>", self._hx_key)
        self.hxAsc.bind("<Key>", self._hx_asc_key)
        self._hxLast = self.hxHex
        for w in (self.hxHex, self.hxAsc):
            self._hx_menu(w)
            w.bind("<FocusIn>", lambda e, x=w: setattr(self, "_hxLast", x), add="+")
            w.bind("<Button-1>", lambda e, x=w: setattr(self, "_hxLast", x), add="+")

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
        # Ctrl-поєднання — ПЕРШИМИ. Інакше Ctrl+C приходить сюди як символ \x03,
        # не проходить перевірку «шістнадцяткова цифра» і блокується разом із
        # усім іншим — саме через це копіювання в редакторі не працювало.
        k = ctrl_combo(e)
        if k == "c": return self.hx_copy()
        if k == "v": return self.hx_paste()
        if k == "a": return self.hx_select_all()
        if k == "x": return "break"                       # «вирізати» тут не має сенсу
        if e.keysym in ("Left", "Right", "Up", "Down", "Home", "End",
                        "Prior", "Next", "Tab", "Shift_L", "Shift_R",
                        "Control_L", "Control_R"):
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
        k = ctrl_combo(e)                                 # див. _hx_key
        if k == "c": return self.hx_copy()
        if k == "v": return self.hx_paste()
        if k == "a": return self.hx_select_all()
        if k == "x": return "break"
        if e.keysym in ("Left", "Right", "Up", "Down", "Home", "End",
                        "Prior", "Next", "Tab", "Shift_L", "Shift_R",
                        "Control_L", "Control_R"):
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
        self.txLog = scrolledtext.ScrolledText(self.tabLog, font=fnt("Consolas", 8),
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
            self.cmd("PING", 3.0, cb=lambda _: (self.load_templates(), self.sound_load(),
                                                 self.clock_load(), self.clone_samples_load(),
                                                 self.refresh()))
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
        # Шкалу «заряд за напругою» називає ПРИСТРІЙ — підпис кнопки й текст
        # діалогу беремо звідти, щоб вони не розійшлися з прошивкою.
        if d.get("scaleTxt"):
            self.scaleTxt = d["scaleTxt"]
            if hasattr(self, "btnChgAuto"):
                self.btnChgAuto.config(text="⚡ За напругою (%s)" % d["scaleTxt"])
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
        # Ємність/знос і цикли беруться зі штатних полів Motorola — їх нижче
        # проставить _render_bms(). Тут лише запасний варіант на випадок, коли
        # блоки прошивки побиті й декодер нічого не дав.
        cap = d.get("capacity"); wear = d.get("wear")
        self.ovCap.config(text=(f"{cap}% / знос {wear}%" if isinstance(cap, int) and cap >= 0
                                else "— (див. «Штатні лічильники»)"))
        if d.get("ccaCycles") is not None:
            self.ovCyc.config(text=f"{d['ccaCycles']} зар. / {d['dcaCycles']} розр. (за CCA/DCA)")
        if "genuine" in d:
            self.ovAuth.config(text=("OK" if d["genuine"] else "РИЗИК (" + str(d.get("authReason", "")) + ")"))
        if "headerOk" in d:
            # Профіль моделі — друга 32-байтна сума (0x021–0x040, Σ≡0x00).
            # COPYRIGHT є не в усіх моделей: 4409A й APLI4810C його штатно не
            # мають, і рація такі пакети приймає, тож «—» тут не помилка.
            t = ("заголовок " + ("OK" if d["headerOk"] else "✗")
                 + " · дзеркало " + ("OK" if d.get("mirrorOk") else "✗"))
            if "profileOk" in d:
                t += " · профіль " + ("OK" if d["profileOk"] else "✗ побитий")
            if "copyright" in d:
                t += " · © " + {"ok": "OK", "broken": "✗ сума хибна",
                                "none": "немає (норма для моделі)"}.get(d["copyright"], "?")
            self.ovInteg.config(text=t)
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
        rs = d.get("rsense")
        self._rs_from_chip = bool(d.get("rsenseChip"))
        self.dRs.config(text=(f"{rs * 1000:.2f} мОм " +
                              ("(з чипа)" if d.get("rsenseChip") else "(з налаштувань — у чипі поля немає)"))
                        if isinstance(rs, (int, float)) else "—")
        self.dSerial33.config(text=d.get("serial33") or "—")
        self._render_bms(d.get("bms"))
        b = d.get("bms") or {}
        # Поточний знос — одразу в картку правки, щоб не звірятись з іншою вкладкою.
        if getattr(self, "lblHpNow", None) is not None:
            h = b.get("health") if b.get("haveKey") else None
            self.lblHpNow.config(text=("зараз у чипі: %d %% (≈%s мА·год)" % (h, b.get("potentialMah")))
                                 if h is not None else "зараз у чипі: — (ключ не визначено)")
        # Справжня дата з чипа має пріоритет над оцінкою «сьогодні − ETM».
        if b.get("firstUseDate"):
            self.dFirst.config(text=b["firstUseDate"] + " (з чипа)", foreground="")
        self._warn_foreign_2438(b, etm if isinstance(etm, int) else 0)
        if isinstance(cap, int) and cap >= 0:
            self._set_entry(self.eCap, str(cap))
        if d.get("icaMah") is not None:
            self._set_entry(self.eMah, str(d.get("icaMah")))
        self._set_text(self.tx33, self._hex_dump(d.get("hex33", "")))
        self._set_text(self.tx38, self._hex_dump(d.get("hex38", "")))

    # Пакет не міг ПРАЦЮВАТИ довше, ніж він ІСНУЄ. Якщо напрацювання ETM більше
    # за вік від дати виготовлення — DS2438 не від цього АКБ (типово: монітор не
    # перечитали після зміни пакета). Це не дрібниця: шунт у DS2438 свій у
    # кожного пакета, тож із чужого монітора струм, залишок і знос будуть хибні.
    # Допуск 180 діб: на 31 рідній парі з dumps/ ETM перевищував вік щонайбільше
    # на 44 доби — лічильник стартує до того, як у чип запишуть дату.
    ETM_AGE_SLACK_D = 180

    def _warn_foreign_2438(self, b, etm_sec):
        import datetime
        mfg = b.get("mfgDate")
        if not mfg or not etm_sec:
            self.bWarn.grid_remove(); return
        try:
            age = (datetime.date.today() - datetime.date(*map(int, mfg.split("-")))).days
        except ValueError:
            self.bWarn.grid_remove(); return
        etm_d = etm_sec // 86400
        if etm_d > age + self.ETM_AGE_SLACK_D:
            self.bWarn.config(text=("⚠️ Напрацювання ETM (%d діб) більше за вік пакета "
                                    "(%d діб від %s). Або монітор не від цього АКБ "
                                    "(перечитайте DS2438 — доти струм, залишок і знос "
                                    "неправильні), або пакет пройшов цикл на станції: ЗП "
                                    "переписує ETM своїм числом. Тому наробіток правлять "
                                    "ПІСЛЯ калібрування, а не до нього."
                                    % (etm_d, age, mfg)))
            self.bWarn.grid()
        else:
            self.bWarn.grid_remove()

    _rs_from_chip = False        # чи взято шунт із чипа (впливає на точність зносу)
    _disNowMv = 0                # поточна напруга — риска на графіку лінійки

    def _render_bms(self, b):
        """Штатні поля Motorola з відповіді INFO."""
        empty = [self.bCyc, self.bCycN, self.bPot, self.bHealth,
                 self.bCal, self.bMfg, self.bUse, self.bKey]
        if not b:
            for w in empty:
                w.config(text="—", foreground="")
            return
        cyc = b.get("cycles", -1)
        self.bCyc.config(text=str(cyc) if cyc >= 0 else "— (блок гістограми побитий)")
        self.bCycN.config(text=str(b.get("nonImpresCycles", "—")))
        if b.get("haveKey"):
            pot = b.get("potentialMah", 0); first = b.get("firstUseMah") or 0
            self.bPot.config(text=f"{pot} мА·год" + (f" (на початку {first})" if first else ""))
            h = b.get("health", 0)
            # Знос рахується через шунт. Якщо шунт не з чипа — це лише оцінка:
            # у різних моделей він відрізняється майже вдвічі.
            note = "" if self._rs_from_chip else "  (оцінка: шунт не з чипа)"
            self.bHealth.config(text=f"{h} %{note}",
                                foreground="#7ea24a" if h >= 80 else ("" if h >= 60 else "#c0392b"))
            self.bCal.config(text=str(b.get("calCycles", "—")))
            self.bMfg.config(text=b.get("mfgDate") or "—")
            self.bUse.config(text=b.get("firstUseDate") or "— (пакет ще не вмикався)")
            self.bKey.config(text="підібрано перебором (ROM чипа недоступний)"
                             if b.get("keyGuessed") else "з ROM-ID чипа DS2433")
            self.ovCap.config(text=f"{pot} мА·год / знос {h}%")
            self.ovCyc.config(text=f"{cyc} зар. IMPRES" +
                              (f" / +{b['nonImpresCycles']} звич. ЗП" if b.get("nonImpresCycles") else ""))
        else:
            for w in (self.bPot, self.bHealth, self.bCal, self.bMfg, self.bUse):
                w.config(text="—", foreground="")
            self.bKey.config(text="не визначено — зчитайте АКБ пристроєм (потрібен ROM-ID DS2433)")
            if cyc >= 0:
                self.ovCyc.config(text=f"{cyc} зар. IMPRES")

    def _set_entry(self, e, val):
        e.delete(0, "end"); e.insert(0, val)

    def _set_text(self, t, s):
        t.delete("1.0", "end"); t.insert("1.0", s)

    @staticmethod
    def _hex_dump(s):
        """Дамп у вигляді адреса + 16 байт + ASCII.

        Пристрій віддає один довгий рядок «8D F8 01 …». У Text він переноситься
        там, де влізе по ширині вікна, тож стовпці «пливуть» — на 512 байтах це
        видно одразу. Розкладаємо самі, по 16 байт у рядку.
        """
        b = [x for x in (s or "").split() if len(x) == 2]
        if not b:
            return ""
        rows = []
        for i in range(0, len(b), 16):
            row = b[i:i + 16]
            hx = " ".join(row).ljust(16 * 3 - 1)
            asc = "".join(chr(v) if 32 <= v < 127 else "." for v in (int(x, 16) for x in row))
            rows.append("%03X: %s  %s" % (i, hx, asc))
        return "\n".join(rows)

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
                                 self.cmd(f"INITBAT {model} {mah}", 25.0,
                                          cb=lambda r: self._after_init(r, model))))

    def _after_init(self, r, model):
        """identity=False означає, що ROM чипа не знайшли й у пам'яті лишилась
        шифровка донора — рація прочитає її як сміття. Мовчати про це не можна."""
        if isinstance(r, dict) and r.get("ok") and not r.get("identity", True):
            messagebox.showwarning(
                "Новий АКБ",
                "%s записано, але ROM-ID чипа DS2433 невідомий — ідентичність НЕ згенеровано.\n\n"
                "У чипі лишилась зашифрована частина донора: рація розшифрує її своїм ключем\n"
                "і побачить сміття («невідомий акумулятор»). Перечитайте АКБ і повторіть." % model)
            self._after_write(r, "⚠️ %s записано без згенерованої ідентичності" % model)
            return
        dt = _dnum((r or {}).get("mfgDate", 0)) if isinstance(r, dict) else ""
        self._after_write(r, "✅ Новий %s записано%s"
                          % (model, (", ідентичність із ROM, дата " + dt) if dt and dt != "—" else ""))

    # ---- керований розряд ---------------------------------------------
    def _dis_show(self, r):
        # Уся візуалізація — у DischargeMonitor; тут лише передаємо стан і
        # знімаємо ознаку «запит у польоті».
        self._disBusy = False
        d = (r or {}).get("discharge") if isinstance(r, dict) else None
        # Межі лінійки і цілі — з пристрою: він їх і застосовує, тож клієнт не
        # має права мати власну думку.
        if isinstance(d, dict) and d.get("rampHiMv"):
            for k in ("rampHiMv", "maHi", "maLo", "tgtMinMv", "tgtMaxMv", "tgtDefMv"):
                if d.get(k) is not None:
                    self.disRamp[k] = d[k]
            self._dis_ramp_note()
        if isinstance(d, dict) and d.get("state") == "run":
            self._disNowMv = d.get("mv", 0)
            self._dis_ramp_draw(self._dis_target_mv())
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

    # Межі лінійки приходять від пристрою (див. dischargeJson); поки не
    # опитали — типові з settings.h.
    disRamp = {"rampHiMv": 8250, "maHi": 1000, "maLo": 300,
               "tgtMinMv": 6800, "tgtMaxMv": 8000, "tgtDefMv": 7200}

    def _dis_target_mv(self):
        """Ціль: поле «або, мВ» має пріоритет над кнопками; затиснута в межі."""
        raw = (self.eDisTarget.get() or "").strip() if hasattr(self, "eDisTarget") else ""
        try:
            mv = int(raw) if raw else int(self.disTarget.get())
        except (ValueError, tk.TclError):
            mv = self.disRamp["tgtDefMv"]
        return max(self.disRamp["tgtMinMv"], min(self.disRamp["tgtMaxMv"], mv))

    def _dis_setpoint_ma(self, mv, target):
        """Та сама формула, що в discharge.h."""
        c = self.disRamp
        if target >= c["rampHiMv"]:
            return c["maLo"]
        if mv >= c["rampHiMv"]:
            return c["maHi"]
        if mv <= target:
            return c["maLo"]
        return int(round(c["maLo"] + (mv - target) * (c["maHi"] - c["maLo"])
                         / (c["rampHiMv"] - target)))

    def _dis_ramp_note(self, *_):
        if not hasattr(self, "lblDisRamp"):
            return
        mv, c = self._dis_target_mv(), self.disRamp
        self.lblDisRamp.config(
            text="Струм веде за напругою: %d мА на %.2f В → %d мА рівно на цілі %.2f В.\n"
                 "Малий струм у кінці дає чесніший вимір ємності й щадить банки."
                 % (c["maHi"], c["rampHiMv"] / 1000.0, c["maLo"], mv / 1000.0))
        self._dis_ramp_draw(mv)

    def _dis_ramp_draw(self, target):
        cv = getattr(self, "cvDisRamp", None)
        if cv is None:
            return
        cv.delete("all")
        c = self.disRamp
        W, H = int(cv["width"]), int(cv["height"])
        x0, x1 = target, max(c["rampHiMv"], target + 1)
        X = lambda mv: 6 + (mv - x0) / (x1 - x0) * (W - 12)
        Y = lambda ma: H - 22 - ma / max(1, c["maHi"]) * (H - 40)
        pts = []
        for i in range(61):
            mv = x0 + (x1 - x0) * i / 60.0
            pts += [X(mv), Y(self._dis_setpoint_ma(int(round(mv)), target))]
        cv.create_polygon([6, H - 22] + pts + [W - 6, H - 22],
                          fill="#2a3320", outline="")
        cv.create_line(*pts, fill=MIL["khaki"], width=2)
        f = fnt("Segoe UI", 7)
        cv.create_text(8, 9, text="%d мА" % c["maHi"], anchor="w", fill="#8b9166", font=f)
        cv.create_text(8, H - 8, text="ціль %.2f В" % (target / 1000.0),
                       anchor="w", fill="#8b9166", font=f)
        cv.create_text(W - 8, H - 8, text="%.2f В" % (c["rampHiMv"] / 1000.0),
                       anchor="e", fill="#8b9166", font=f)
        cv.create_text(W - 8, 9, text="%d мА" % c["maLo"], anchor="e", fill="#8b9166", font=f)
        # поточна напруга під час розряду — вертикальна риска
        mv = getattr(self, "_disNowMv", 0)
        if x0 <= mv <= x1:
            x = X(mv)
            cv.create_line(x, 4, x, H - 20, fill="#6f8f3a")

    def discharge_start(self):
        if not self.need_conn():
            return
        mv, c = self._dis_target_mv(), self.disRamp
        if not messagebox.askyesno("Розряд",
                "Почати розряд до %.2f В?\n\n"
                "Струм обмежується ШІМом: %d мА на %.2f В, лінійно до %d мА на %.2f В.\n"
                "Навантаження буде увімкнено, резистор нагріється.\n"
                "Не лишайте пристрій без нагляду."
                % (mv / 1000.0, c["maHi"], c["rampHiMv"] / 1000.0, c["maLo"], mv / 1000.0)):
            return
        def done(r):
            if isinstance(r, dict) and r.get("ok"):
                self.status("Розряд почато"); self.monDis.reset_history(); self._dis_show(r)
            else:
                self._disBusy = False
                self.status("Помилка: " + str((r or {}).get("err", "")))
        self.maybe_auth(lambda: self.cmd("DISCHARGE %d" % mv, 15.0, cb=done))

    def discharge_stop(self):
        if not self.need_conn():
            return
        self.cmd("DISCHARGE STOP", 10.0, cb=lambda r: (self.status("Розряд зупинено"), self._dis_show(r)))

    # --- ЗАРЯД через DC/DC (CHARGE ?/CHARGE [%]/CHARGE STOP) -----------------
    # Ціль обирається у відсотках (50..100); профіль струму (точки перегину
    # 10/50/80/95 %) масштабується під неї на пристрої — тут лише вибір цілі.
    chgTargetBounds = {"min": 50, "max": 100, "def": 100}

    def _chg_target_pct(self):
        """Ціль: поле «або, %» має пріоритет над кнопками; затиснута в межі."""
        raw = (self.eChgTarget.get() or "").strip() if hasattr(self, "eChgTarget") else ""
        try:
            pct = int(raw) if raw else int(self.chgTarget.get())
        except (ValueError, tk.TclError):
            pct = self.chgTargetBounds["def"]
        return max(self.chgTargetBounds["min"], min(self.chgTargetBounds["max"], pct))

    def _chg_show(self, r):
        self._chgBusy = False
        d = (r or {}).get("charge") if isinstance(r, dict) else None
        self.monChg.update_state(d)
        self.psu_alert_update(d)          # смуга аварії живлення — над вкладками

    # ── АВАРІЯ ЖИВЛЕННЯ: смуга вгорі вікна ────────────────────────────────
    # Ті самі поля /api/charge, що малює екран пристрою й веб-інтерфейс, тож
    # розходження між трьома поверхнями неможливе.
    def psu_alert_update(self, d):
        try:
            bad = bool(d) and d.get("psuSensed") and not d.get("psuOk", True)
            if not bad:
                if self._psuShown:
                    self.psuBar.pack_forget()
                    self._psuShown = False
                return
            self.lblPsu.config(
                text="⛔ %s\n%s\nвиміряно %.2f В, потрібно %.1f…%.1f В\n"
                     "Заряд неможливий. Читання й правка пам'яті пакета працюють "
                     "без блока живлення."
                     % (d.get("psuHead") or d.get("psuText", "живлення поза допуском"),
                        d.get("psuText", ""),
                        d.get("psuMv", 0) / 1000.0,
                        d.get("psuMinMv", 0) / 1000.0, d.get("psuMaxMv", 0) / 1000.0))
            if not self._psuShown:
                # before=вкладки, щоб смуга сіла НАД ними, а не під низом вікна.
                self.psuBar.pack(fill="x", padx=6, pady=(0, 4), before=self.nb)
                self._psuShown = True
        except tk.TclError:
            pass                      # вікно закрилось

    def _psu_blink(self):
        """Блимання смуги. Статичну червону смугу за хвилину перестаєш
        помічати — рухома лишається видимою боковим зором; це та сама причина,
        з якої аварійна індикація в техніці завжди рухома."""
        try:
            if self._psuShown:
                self._psuBlink = not self._psuBlink
                # ⚠️ Блимає ПЛАШКА, а не текст: колір напису підбирається під
                # обидві фази, щоб слова читались завжди. Інакше виходить те
                # саме, що спершу вийшло на екрані пристрою, — півперіоду
                # кольорова смуга без жодного слова про причину.
                c = MIL["maroon"] if self._psuBlink else MIL["bg_dark"]
                self.psuBar.config(bg=c)
                self.lblPsu.config(bg=c, fg="#ffffff")
            self.root.after(500, self._psu_blink)
        except tk.TclError:
            pass

    def _chg_tick(self):
        """Автоопитування стану заряду — той самий принцип, що й _dis_tick:
        раз на 3 с під час заряду, раз на 30 с у спокої (щоб побачити заряд,
        запущений кнопкою на самому пристрої)."""
        try:
            d = self.monChg.d
            run = bool(d) and d.get("state") == "run"
            if self.connected and not self._chgBusy:
                self._chgBusy = True
                self.cmd("CHARGE ?", 8.0, cb=self._chg_show)
            # Несправне живлення опитуємо частіше за звичайний спокій:
            # користувач саме зараз щось перемикає в блоці й чекає, коли смуга
            # зникне.
            badPsu = bool(d) and d.get("psuSensed") and not d.get("psuOk", True)
            self.root.after(3000 if (run or badPsu) else 30000, self._chg_tick)
        except tk.TclError:
            pass                      # вікно закрилось

    def charge_status(self):
        if not self.need_conn():
            return
        self._chgBusy = True
        self.cmd("CHARGE ?", 8.0, cb=self._chg_show)

    def charge_start(self):
        if not self.need_conn():
            return
        pct = self._chg_target_pct()
        if not messagebox.askyesno("Заряд",
                "Почати заряд через DC/DC до %d%%?\n\n"
                "Профіль струму (200→1500→100 мА) масштабується під цю ціль, тож заряд "
                "і так закінчиться плавним спадом струму перед самою ціллю.\n"
                "Переконайтесь, що керування ПЕРЕВІРЕНЕ мультиметром (enable=LOW безпечно).\n"
                "Не лишайте пристрій без нагляду." % pct):
            return
        def done(r):
            if isinstance(r, dict) and r.get("ok"):
                self.status("Заряд почато"); self._chg_show(r)
            else:
                self._chgBusy = False
                self.status("Помилка: " + str((r or {}).get("err", "")))
        self.maybe_auth(lambda: self.cmd("CHARGE %d" % pct, 15.0, cb=done))

    def charge_stop(self):
        if not self.need_conn():
            return
        self.cmd("CHARGE STOP", 10.0, cb=lambda r: (self.status("Заряд зупинено"), self._chg_show(r)))

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
                if r.get("plan"):
                    self._render_plan(r["plan"])
                n = len([f for f in r.get("plan", {}).get("fixes", []) if f.get("on")])
                self._after_write(r, f"✅ Еталон {model} записано"
                                     + (" (DS2433+DS2438)" if both else " (лише DS2433)")
                                     + (f", правок під пакет: {n}" if n else ""))
            else:
                self._after_write(r, "")
        arg = (f"RESTORE {model}" +
               (" VERBATIM" if verbatim
                else self._rp_fixes_arg() + " TAIL=" + self._tail_mode()))
        self.maybe_auth(lambda: (self.status("Відновлення еталона..."),
                                 self.cmd(arg, 25.0, cb=done)))

    def restore_battery_verbatim(self):
        self.restore_battery(verbatim=True)

    # ---- правки еталона під конкретний пакет -----------------------------
    def _rp_rated(self):
        """Вписана вручну ємність нових банок; порожньо/сміття -> 0."""
        e = getattr(self, "eRpRated", None)
        if e is None:
            return 0
        try:
            return max(0, int(e.get().strip() or 0))
        except (ValueError, tk.TclError):
            return 0

    def _rp_rsense(self):
        """Вписаний вручну шунт у «сирих» одиницях чипа (мОм×100); 0 — не вписано."""
        e = getattr(self, "eRpRs", None)
        if e is None:
            return 0
        try:
            return max(0, int(round(float((e.get() or "").strip().replace(",", ".") or 0) * 100)))
        except (ValueError, tk.TclError):
            return 0

    def _rp_rsmodel(self):
        """Модель, з еталона якої взяти шунт; порожньо — вручну / зі свого пакета."""
        c = getattr(self, "cbRpRs", None)
        v = c.get().strip() if c is not None else ""
        return "" if v in ("", RP_RS_MANUAL) else v.split(" ")[0]

    def _tail_mode(self):
        """FRESH або ERASE — режим навченого хвоста для RESTORE/WIZSTEP."""
        c = getattr(self, "cbTail", None)
        return "ERASE" if (c is not None and c.get() == TAIL_MODES[1]) else "FRESH"

    def _rp_mfg(self):
        """Вписана вручну дата виготовлення як YYYYMMDD; порожньо/сміття -> 0."""
        e = getattr(self, "eRpMfg", None)
        if e is None:
            return 0
        t = (e.get() or "").strip().replace(".", "-").replace("/", "-")
        if not t:
            return 0
        try:
            y, m, d = (int(x) for x in t.split("-"))
        except (ValueError, tk.TclError):
            return 0
        if not (2005 <= y <= 2035 and 1 <= m <= 12 and 1 <= d <= 31):
            return 0
        return y * 10000 + m * 100 + d

    def _rp_seed(self):
        """Заповнити поля правок тим, що ЗАРАЗ прочитано з пакета.

        Підставляємо лише справді відоме: якщо ключ вмісту не визначається,
        зашифровані числа — сміття, і записати його назад уже під правильним
        ключем було б гірше, ніж лишити поле порожнім. Лічильники циклів ключа
        не потребують, тож ідуть завжди."""
        p = self.rpPlan or {}
        def put(attr, val):
            e = getattr(self, attr, None)
            if e is None or val in ("", None):
                return 0
            e.delete(0, "end"); e.insert(0, str(val))
            return 1
        n = 0
        if p.get("cryptSrcOk"):
            if p.get("mfgReal"):        n += put("eRpMfg", _dnum(p["mfgReal"]))
            if p.get("useReal"):        n += put("eRpUse", _dnum(p["useReal"]))
            if p.get("hpReal", 0) > 0:  n += put("eRpHp",  p["hpReal"])
            if p.get("calReal", -1) >= 0: n += put("eRpCal", p["calReal"])
        c = getattr(self, "cbRpEtm", None)
        if p.get("useReal") and c is not None:
            c.set(RP_ETM_SRC[1])
        if p.get("cycNow", -1) >= 0: n += put("eRpCyc", p["cycNow"])
        if p.get("nonNow", -1) >= 0: n += put("eRpNon", p["nonNow"])
        self.status("Підставлено з акумулятора: %d пол." % n if n else "Підставляти нема чого")
        self._rp_recalc()

    def _rp_seed_clear(self):
        for attr in ("eRpMfg", "eRpUse", "eRpHp", "eRpCal", "eRpCyc", "eRpNon"):
            e = getattr(self, attr, None)
            if e is not None:
                e.delete(0, "end")
        self._rp_recalc()

    def _rp_entry(self, parent, width, value="", on=None):
        """Поле правки: створити, заповнити й підписати на перерахунок плану.

        on — що робити замість простого перерахунку (наприклад, ще й перевести
        наробіток на вписану дату). Другою прив'язкою це зробити не можна:
        вона спрацювала б уже ПІСЛЯ того, як план перерахували."""
        e = ttk.Entry(parent, width=width)
        e.pack(side="left", padx=4)
        if value:
            e.insert(0, value)
        h = on or self._rp_recalc
        e.bind("<Return>", lambda _e: h())
        e.bind("<FocusOut>", lambda _e: h())
        return e

    def _rp_int(self, attr):
        """Ціле з поля правки; -1 = не вписували (нуль тут — повноцінне число)."""
        e = getattr(self, attr, None)
        if e is None:
            return -1
        t = (e.get() or "").strip()
        if not t:
            return -1
        try:
            v = int(t)
        except (ValueError, tk.TclError):
            return -1
        return v if 0 <= v <= 65535 else -1

    def _rp_use(self):
        """Дата першого запуску як YYYYMMDD; 0 — не вписували."""
        e = getattr(self, "eRpUse", None)
        if e is None:
            return 0
        t = (e.get() or "").strip().replace(".", "-").replace("/", "-")
        if not t:
            return 0
        try:
            y, m, d = (int(x) for x in t.split("-"))
        except (ValueError, tk.TclError):
            return 0
        return y * 10000 + m * 100 + d if (2005 <= y <= 2035 and 1 <= m <= 12 and 1 <= d <= 31) else 0

    def _rp_health(self):
        """Вписаний вручну знос, %; порожньо/сміття -> 0."""
        e = getattr(self, "eRpHp", None)
        if e is None:
            return 0
        try:
            v = int((e.get() or "").strip() or 0)
        except (ValueError, tk.TclError):
            return 0
        return v if 1 <= v <= 100 else 0

    def _rp_today(self):
        """Сьогоднішня дата як YYYYMMDD. Годинника в пристрої немає — шле клієнт."""
        d = datetime.date.today()
        return d.year * 10000 + d.month * 100 + d.day

    def _rp_use_typed(self):
        """Дату запуску вписали — наробіток рахуємо з неї (див. рядок «наробіток»)."""
        c = getattr(self, "cbRpEtm", None)
        if c is not None and self._rp_use():
            c.set(RP_ETM_SRC[1])
        self._rp_recalc()

    def _rp_etm_src(self):
        """USE — рахувати наробіток із дати запуску, PACK — лишити свій; "" — не чіпати."""
        c = getattr(self, "cbRpEtm", None)
        if c is None:
            return ""
        return "USE" if c.get() == RP_ETM_SRC[1] else "PACK"

    def _rp_keys(self):
        k = [key for key, v in self.rpVars.items() if v.get()]
        # Вписана ємність — це вже згода її записати, галочки для неї може ще
        # не бути (рядок був недоступним, поки поле стояло порожнім).
        if self._rp_rated() and "rated" not in k:
            k.append("rated")
        if (self._rp_rsense() or self._rp_rsmodel()) and "rsense" not in k:
            k.append("rsense")
        if (self._rp_mfg() or self._rp_health() or self._rp_use()
                or self._rp_int("eRpCal") >= 0) and "crypt" not in k:
            k.append("crypt")
        if (self._rp_int("eRpCyc") >= 0 or self._rp_int("eRpNon") >= 0) and "hist" not in k:
            k.append("hist")
        if self._rp_etm_src() == "USE" and "etm" not in k:
            k.append("etm")
        return ",".join(k)

    def _rp_args(self):
        r = self._rp_rated()
        # Модель із бібліотеки й ручне число взаємно виключні — так само, як у полях.
        rsm, rs = self._rp_rsmodel(), self._rp_rsense()
        mfg = self._rp_mfg()
        return (" FIXES=" + self._rp_keys() + (" RATED=%d" % r if r else "") +
                (" RSMODEL=%s" % rsm if rsm else (" RSENSE=%d" % rs if rs else "")) +
                (" MFG=%d" % mfg if mfg else "") +
                (" HEALTH=%d" % self._rp_health() if self._rp_health() else "") +
                (" USE=%d" % self._rp_use() if self._rp_use() else "") +
                (" CAL=%d" % self._rp_int("eRpCal") if self._rp_int("eRpCal") >= 0 else "") +
                (" CYC=%d" % self._rp_int("eRpCyc") if self._rp_int("eRpCyc") >= 0 else "") +
                (" NONIMP=%d" % self._rp_int("eRpNon") if self._rp_int("eRpNon") >= 0 else "") +
                (" TODAY=%d" % self._rp_today()) +
                (" ETMSRC=%s" % self._rp_etm_src() if self._rp_etm_src() else ""))

    def _rp_fixes_arg(self):
        if self.rpPlan is None:
            return ""
        return self._rp_args()

    def rp_write(self):
        """Записати ЛИШЕ обрані правки, не чіпаючи еталон."""
        if not self.need_conn():
            return
        model = self.cbRest.get()
        if not model or self.rpPlan is None:
            messagebox.showwarning("Правки", "Спершу перечитайте акумулятор"); return
        on = [f for f in self.rpPlan.get("fixes", []) if f.get("on")]
        if not on:
            messagebox.showwarning("Правки", "Не обрано жодної правки"); return
        q = ("Записати лише правки?\n"
             + "\n".join("  • %s: %s" % (f.get("title"), f.get("use")) for f in on)
             + "\n\nЕталон, навчена калібровка й лічильники НЕ чіпаються.")
        if not messagebox.askyesno("Записати правки", q):
            return
        def done(r):
            if isinstance(r, dict) and r.get("ok"):
                if r.get("plan"):
                    self._render_plan(r["plan"])
                self._after_write(r, "✅ Правки записано (%d)" % len(on))
            else:
                self._after_write(r, "")
        self.maybe_auth(lambda: (self.status("Запис правок..."),
                                 self.cmd("FIXES " + model + self._rp_args(), 20.0, cb=done)))

    def rp_load(self, reread=False):
        """Запитати план у пристрою. reread=False — порахувати на вже
        прочитаних даних: клієнт смикає це на кожну галочку, і сіпати 1-Wire
        щоразу не варто."""
        model = self.cbRest.get()
        if not model or not self.connected:
            self.rpPlan = None
            self.frRp.pack_forget()
            return
        # TODAY шлемо навіть на «чистому» читанні: без сьогоднішньої дати
        # наробіток із дати першого запуску порахувати нема з чого.
        arg = (f"RESTOREPLAN {model}" + ("" if reread else " NOREAD") +
               " TODAY=%d" % self._rp_today())
        self.cmd(arg, 20.0, cb=self._on_plan)

    def _rp_rs_pick(self):
        """Обрано модель — ручне число гасимо: писати можна тільки щось одне."""
        if getattr(self, "eRpRs", None) is not None:
            self.eRpRs.delete(0, "end")
        self._rp_recalc()

    def _rp_rs_typed(self):
        if getattr(self, "cbRpRs", None) is not None and self._rp_rsense():
            self.cbRpRs.set(RP_RS_MANUAL)
        self._rp_recalc()

    def _rp_recalc(self):
        # Перерахунок робить ПРИСТРІЙ: паливомір залежить від того, який шунт
        # опиниться в чипі, і рахувати це вдруге тут означало б завести другу
        # формулу, яка одного дня розійдеться з прошивкою.
        model = self.cbRest.get()
        if not model or not self.connected:
            return
        self.cmd(f"RESTOREPLAN {model} NOREAD" + self._rp_args(), 20.0, cb=self._on_plan)

    def _on_plan(self, r):
        if isinstance(r, dict) and r.get("ok") and r.get("plan"):
            self._render_plan(r["plan"])
        else:
            self.rpPlan = None
            self.frRp.pack_forget()

    def _render_plan(self, p):
        self.rpPlan = p
        for w in self.rpGrid.winfo_children():
            w.destroy()
        self.rpVars = {}
        # Віджети щойно знищено, а посилання лишились: звертатись до них — це
        # TclError. Рядок наробітку є не завжди, тож гасимо посилання явно.
        self.cbRpEtm = None
        hdr = ("", "Що саме", "Еталон (донор)", "Цей пакет", "Буде записано")
        for c, t in enumerate(hdr):
            ttk.Label(self.rpGrid, text=t, foreground="#9a9c82").grid(row=0, column=c, sticky="w", padx=4, pady=(0, 3))
        self.rpGrid.columnconfigure(1, weight=1)
        mono = fnt("Consolas", 9)
        for i, f in enumerate(p.get("fixes", []), start=1):
            avail = bool(f.get("avail"))
            if avail:
                var = tk.BooleanVar(value=bool(f.get("on")))
                self.rpVars[f["key"]] = var
                ttk.Checkbutton(self.rpGrid, variable=var,
                                command=self._rp_recalc).grid(row=i, column=0, sticky="w")
            else:
                ttk.Label(self.rpGrid, text="—", foreground="#6b6f58").grid(row=i, column=0, sticky="w")
            title = f.get("title", "") + "  ·  пише в " + f.get("chipsText", "")
            if not avail and f.get("key") not in ("rated", "rsense", "crypt", "hist"):
                title += "  (джерела немає)"
            # wraplength — щоб довга назва переносилась, а не заповзала під
            # стовпець значень і не обрізалась на вузькому вікні.
            cell = ttk.Frame(self.rpGrid)
            cell.grid(row=i, column=1, sticky="we", padx=4)
            ttk.Label(cell, text=title, wraplength=int(round(330 * self.zoom)), justify="left",
                      foreground="#e7e3d2" if avail else "#6b6f58").pack(anchor="w")
            # Ємність — єдина правка, яку можна не лише взяти з пакета, а й
            # вписати руками: після заміни банок нової ємності взяти нізвідки.
            if f.get("key") == "rated":
                sub = ttk.Frame(cell); sub.pack(anchor="w", pady=(2, 0))
                ttk.Label(sub, text="нові банки, мА·год:", foreground="#b9bd86").pack(side="left")
                self.eRpRated = ttk.Entry(sub, width=8)
                self.eRpRated.pack(side="left", padx=4)
                if p.get("ratedUser", 0) > 0:
                    self.eRpRated.insert(0, str(p["ratedUser"]))
                self.eRpRated.bind("<Return>", lambda _e: self._rp_recalc())
                self.eRpRated.bind("<FocusOut>", lambda _e: self._rp_recalc())
                ttk.Label(sub, text="(порожньо — лишити як є, крок %d)" % p.get("ratedStep", 25),
                          foreground="#6b6f58").pack(side="left")
            # Шунт — так само правиться руками: у «чистому» моніторі його немає
            # зовсім, і без нього струм, залишок і знос не рахуються. Другий
            # шлях — узяти з еталона моделі: плата в усіх екземплярів однакова.
            if f.get("key") == "rsense":
                sub = ttk.Frame(cell); sub.pack(anchor="w", pady=(2, 0))
                ttk.Label(sub, text="з еталона:", foreground="#b9bd86").pack(side="left")
                lib = [RP_RS_MANUAL] + ["%s — %.2f мОм" % (m.get("model", ""), m.get("raw", 0) / 100.0)
                                        for m in p.get("rsLib", [])]
                self.cbRpRs = ttk.Combobox(sub, values=lib, state="readonly", width=22)
                self.cbRpRs.set(RP_RS_MANUAL)
                for v in lib[1:]:
                    if v.split(" ")[0] == p.get("rsSrc", ""):
                        self.cbRpRs.set(v)
                self.cbRpRs.pack(side="left", padx=4)
                self.cbRpRs.bind("<<ComboboxSelected>>", lambda _e: self._rp_rs_pick())
                ttk.Label(sub, text="або вручну, мОм:", foreground="#b9bd86").pack(side="left")
                self.eRpRs = ttk.Entry(sub, width=8)
                self.eRpRs.pack(side="left", padx=4)
                # Ручне поле заповнюємо ЛИШЕ коли значення вписали руками:
                # інакше після вибору моделі заповнені обидва, і незрозуміло,
                # що саме піде в чип.
                if p.get("rsUser", 0) > 0 and not p.get("rsSrc"):
                    self.eRpRs.insert(0, "%.2f" % (p["rsUser"] / 100.0))
                self.eRpRs.bind("<Return>", lambda _e: self._rp_rs_typed())
                self.eRpRs.bind("<FocusOut>", lambda _e: self._rp_rs_typed())
                if not p.get("rsPack"):
                    ttk.Label(sub, text="(у пакеті шунта немає!)",
                              foreground="#d08a3a").pack(side="left")
            # Дати й лічильники зашифровані ключем із ROM самого чипа. Еталон
            # знято з чужого пакета, тож рація прочитає його поля СВОЇМ ключем
            # і побачить сміття. Дату виготовлення, якої на «свіжому» хвості
            # просто немає, можна вписати руками.
            if f.get("key") == "crypt":
                sub = ttk.Frame(cell); sub.pack(anchor="w", pady=(2, 0))
                if not p.get("haveRom"):
                    ttk.Label(sub, text="ROM чипа невідомий — правка можлива лише на живому чипі",
                              foreground="#d08a3a").pack(side="left")
                else:
                    if p.get("cryptWrong"):
                        ttk.Label(sub, text="⚠ рація читає %s, насправді %s"
                                  % (_dnum(p.get("mfgSeen", 0)), _dnum(p.get("mfgReal", 0))),
                                  foreground="#d08a3a").pack(side="left")
                    elif p.get("cryptUnknown"):
                        # Вміст ні з чим не узгоджений: перенести нічого, але
                        # дату вписати можна — вона піде ключем цього чипа.
                        ttk.Label(sub, text="⚠ рація читає %s; вміст неузгоджений — впишіть дату"
                                  % _dnum(p.get("mfgSeen", 0)),
                                  foreground="#d08a3a").pack(side="left")
                    ttk.Label(sub, text="  дата вигот. вручну (РРРР-ММ-ДД):",
                              foreground="#b9bd86").pack(side="left")
                    self.eRpMfg = ttk.Entry(sub, width=11)
                    self.eRpMfg.pack(side="left", padx=4)
                    if p.get("mfgUser", 0) > 0:
                        self.eRpMfg.insert(0, _dnum(p["mfgUser"]))
                    self.eRpMfg.bind("<Return>", lambda _e: self._rp_recalc())
                    self.eRpMfg.bind("<FocusOut>", lambda _e: self._rp_recalc())
                    ttk.Label(sub, text="знос, %:", foreground="#b9bd86").pack(side="left")
                    self.eRpHp = ttk.Entry(sub, width=5)
                    self.eRpHp.pack(side="left", padx=4)
                    if p.get("hpUser", 0) > 0:
                        self.eRpHp.insert(0, str(p["hpUser"]))
                    self.eRpHp.bind("<Return>", lambda _e: self._rp_recalc())
                    self.eRpHp.bind("<FocusOut>", lambda _e: self._rp_recalc())
                    sub2 = ttk.Frame(cell); sub2.pack(anchor="w", pady=(2, 0))
                    ttk.Label(sub2, text="дата запуску (РРРР-ММ-ДД):",
                              foreground="#b9bd86").pack(side="left")
                    # Вписали дату запуску — одразу переводимо на неї й наробіток:
                    # інакше рація показала б свою дату (зі старого наробітку
                    # монітора), а фірмове ПЗ — нашу.
                    self.eRpUse = self._rp_entry(sub2, 11, _dnum(p.get("useUser", 0))
                                                 if p.get("useUser") else "",
                                                 on=self._rp_use_typed)
                    ttk.Label(sub2, text="калібрувань:", foreground="#b9bd86").pack(side="left")
                    self.eRpCal = self._rp_entry(sub2, 6,
                                                 str(p["calUser"]) if p.get("calUser", -1) >= 0 else "")
                    ttk.Label(cell, foreground="#6b6f58", justify="left", wraplength=460,
                              text="рація зараз читає запуск %s, калібрувань %s"
                                   % (_dnum(p.get("useSeen", 0)),
                                      p.get("calSeen") if p.get("calSeen", -1) >= 0 else "—")
                              ).pack(anchor="w")
                    bar = ttk.Frame(cell); bar.pack(anchor="w", pady=(3, 0))
                    ttk.Button(bar, text="📥 Узяти з акумулятора",
                               command=self._rp_seed).pack(side="left", padx=2)
                    ttk.Button(bar, text="✖ Очистити поля",
                               command=self._rp_seed_clear).pack(side="left", padx=2)
                    ttk.Label(cell, foreground="#6b6f58", justify="left", wraplength=460,
                              text=("рація зараз читає знос %s; буде записано CTS = %d"
                                    % (("%d %%" % p["hpSeen"]) if p.get("hpSeen") else "—",
                                       p.get("ctsUse", 0)))).pack(anchor="w")
            # Наробіток. Рація показує «дату першого користування» як «зараз
            # мінус наробіток» — числа з DS2433 для цього рядка вона не читає.
            # Тому дату треба продублювати наробітком, інакше рація покаже свою
            # дату, а фірмове ПЗ — нашу. Рахує пристрій; «сьогодні» шле ПК, бо
            # годинника в приладі немає.
            if f.get("key") == "etm" and avail:
                sub = ttk.Frame(cell); sub.pack(anchor="w", pady=(2, 0))
                ttk.Label(sub, text="звідки брати:", foreground="#b9bd86").pack(side="left")
                self.cbRpEtm = ttk.Combobox(sub, values=list(RP_ETM_SRC), state="readonly", width=34)
                self.cbRpEtm.set(RP_ETM_SRC[1] if p.get("etmFromUse") else RP_ETM_SRC[0])
                self.cbRpEtm.pack(side="left", padx=4)
                self.cbRpEtm.bind("<<ComboboxSelected>>", lambda _e: self._rp_recalc())
                dd = lambda v: int(round(v / 86400.0))
                if p.get("etmUseDate"):
                    t = ("від дати запуску %s до сьогодні (%s) — %d діб"
                         % (_dnum(p["etmUseDate"]), _dnum(p.get("today", 0)),
                            dd(p.get("etmCalc", 0))))
                else:
                    t = "порахувати нема з чого: невідома дата першого запуску"
                ttk.Label(cell, foreground="#6b6f58", justify="left", wraplength=460,
                          text="у пакеті зараз %s  ·  %s"
                               % (("%d діб" % dd(p["etmPack"])) if p.get("etmPack") else "немає", t)
                          ).pack(anchor="w")
            # Цикли ключа не потребують — окремий рядок із двома полями.
            if f.get("key") == "hist":
                sub = ttk.Frame(cell); sub.pack(anchor="w", pady=(2, 0))
                ttk.Label(sub, text="циклів IMPRES:", foreground="#b9bd86").pack(side="left")
                self.eRpCyc = self._rp_entry(sub, 7,
                                             str(p["cycUser"]) if p.get("cycUser", -1) >= 0 else "")
                ttk.Label(sub, text="не-IMPRES:", foreground="#b9bd86").pack(side="left")
                self.eRpNon = self._rp_entry(sub, 7,
                                             str(p["nonUser"]) if p.get("nonUser", -1) >= 0 else "")
                ttk.Label(cell, foreground="#6b6f58", justify="left", wraplength=460,
                          text="зараз у чипі: IMPRES %s, не-IMPRES %s  ·  0 — «як новий»"
                               % (p.get("cycNow") if p.get("cycNow", -1) >= 0 else "—",
                                  p.get("nonNow") if p.get("nonNow", -1) >= 0 else "—")
                          ).pack(anchor="w")

            for c, key, col in ((2, "tpl", "#9a9c82"), (3, "pack", "#e7e3d2"), (4, "use", "#c8b04a")):
                ttk.Label(self.rpGrid, text=f.get(key, "—"), font=mono,
                          foreground=col if avail else "#6b6f58").grid(row=i, column=c, sticky="e", padx=4)
        if p.get("pack38"):
            self.lblRpSum.config(
                text="Виміряно зараз: %.2f В → заряд %d %% (паливомір ICA %d, паспортна ємність %d мА·год%s).\n"
                     "В еталоні зашито %.2f В — це стан ДОНОРА, не вашого пакета."
                     % (p.get("packMv", 0) / 1000.0, p.get("packPct", -1), p.get("icaUse", 0),
                        p.get("ratedMah", 0),
                        " — вписана вручну" if p.get("ratedUser", 0) > 0 else "",
                        p.get("tplMv", 0) / 1000.0))
        else:
            self.lblRpSum.config(text="Монітор DS2438 не прочитано, тож брати реальні значення нема звідки — "
                                      "піде те, що в еталоні.")
        w = ""
        if not p.get("pack38"):
            w = ("⚠️ DS2438 не читається. Якщо мікросхема жива — натисніть «Перечитати акумулятор»: "
                 "без неї в пакет піде рівень заряду й шунт донора.")
        elif not p.get("tpl38"):
            w = ("ℹ️ Для цієї моделі еталона монітора немає — DS2438 пакета лишається своїм, "
                 "правиться лише паливомір.")
        if w:
            self.lblRpWarn.config(text=w)
            self.lblRpWarn.pack(anchor="w", pady=(2, 0), before=self.rpGrid)
        else:
            self.lblRpWarn.pack_forget()
        self.frRp.pack(fill="x", pady=(6, 2))

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
        scale = getattr(self, "scaleTxt", "") or "шкала пристрою"
        if not messagebox.askyesno("Заряд", "Виставити рівень заряду з поточної напруги?\n"
                                            "(%s; зарядка потім уточнить)" % scale):
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

    CLK_SRC = {"client": "від клієнта (точна)",
               "saved":  "відновлена після перезавантаження — відстає",
               "none":   "не заведено"}

    def clock_load(self):
        """Спитати, яку дату пристрій вважає сьогоднішньою."""
        if not self.connected:
            return
        self.cmd("CLOCK", 8.0, cb=self._clock_show)

    def _clock_show(self, r):
        if not isinstance(r, dict) or not r.get("ok"):
            return
        src = self.CLK_SRC.get(r.get("src", ""), r.get("src", "—"))
        self.lblClk.config(text="пристрій вважає, що сьогодні: %s  ·  %s"
                                % (_dnum(r.get("today", 0)), src))

    def clock_sync(self):
        if not self.need_conn():
            return
        self.cmd("CLOCK %d" % self._rp_today(), 8.0,
                 cb=lambda r: (self.status("✅ Дату пристрою синхронізовано"), self.clock_load()))

    def clone_samples_load(self):
        """Вбудовані зразки моніторів копій — щоб не носити файл із собою."""
        if not self.connected:
            return
        self.cmd("SAMPLES", 8.0, cb=self._clone_samples_show)

    def _clone_samples_show(self, r):
        if not isinstance(r, dict) or not r.get("ok"):
            return
        self._clSamples = r.get("samples", []) or []
        self.cbClSample["values"] = ["— свій файл —"] + [s.get("name", "?") for s in self._clSamples]

    def _clone_sample_pick(self):
        i = self.cbClSample.current() - 1        # 0 = «свій файл»
        subs = getattr(self, "_clSamples", [])
        if i < 0 or i >= len(subs):
            return
        self._cloneHex = subs[i].get("hex", "")
        self.lblClone.config(text="%s · %d мА·год" % (subs[i].get("note", ""), subs[i].get("rated", 0)))

    def hdr_fix(self):
        """Добудова заголовка DS2433, яку почала (але не завершила) станція
        WPLN4226A: дзеркало з DS2438 уже на місці, суму вона не виправила."""
        if not self.need_conn():
            return
        if not messagebox.askyesno("Добудова заголовка",
                "Добудувати заголовок із дзеркала DS2438?\n\n"
                "Профіль і модель цим не відновлюються — лише заголовок стане структурно валідним."):
            return
        self.maybe_auth(lambda: (self.status("Добудова..."),
                                 self.cmd("HDRFIX", 15.0, cb=self._hdrfix_show)))

    def _hdrfix_show(self, r):
        ok = isinstance(r, dict) and r.get("ok")
        note = (r or {}).get("note", "")
        self.status(("✅ " + note) if ok and note else ("✅ Готово" if ok else "Помилка: " + note), ok)
        if not ok:
            messagebox.showerror("Добудова заголовка", note or "Збій запису")
        self.refresh()

    def clone_pick(self):
        """Обрати дамп DS2438 копії — рівно 64 байти."""
        p = filedialog.askopenfilename(title="Дамп DS2438 копії (64 Б)",
                                       filetypes=[("Дамп", "*.bin"), ("Усі файли", "*.*")])
        if not p:
            return
        try:
            with open(p, "rb") as f:
                d = f.read()
        except OSError as e:
            messagebox.showerror("Дамп", str(e)); return
        if len(d) != 64:
            messagebox.showwarning("Дамп", "Дамп DS2438 має бути рівно 64 байти (зараз %d)" % len(d))
            return
        self._cloneHex = d.hex()
        self.lblClone.config(text="%s (64 Б)" % os.path.basename(p))

    def clone_restore(self):
        if not self.need_conn():
            return
        hx = getattr(self, "_cloneHex", "")
        if len(hx) != 128:
            messagebox.showwarning("Режим копії", "Спершу оберіть дамп DS2438 копії (64 байти)")
            return
        id33 = self.vClId33.get()
        if not messagebox.askyesno("Режим копії",
                "Відновити за зразком копії?\n\n"
                "DS2438 — зі зразка, лічильники в нуль, паливомір із поточної напруги.\n"
                "DS2433 %s\n\nЦе крайній засіб. Продовжити?"
                % ("отримає ЕКСПЕРИМЕНТАЛЬНУ ідентичність на каркасі 4409."
                   if id33 else "буде СТЕРТО.")):
            return
        def dn(e):
            t = (e.get() or "").strip().replace(".", "-").replace("/", "-")
            try:
                y, m, d = (int(x) for x in t.split("-"))
                return y * 10000 + m * 100 + d
            except (ValueError, tk.TclError):
                return 0
        rs = self.cbClRs.get()
        rsv = "".join(ch for ch in rs.split(" ")[0] if ch.isdigit()) if rs != CLONE_SHUNTS[0] else ""
        a = " " + hx
        if (self.eClRated.get() or "").strip(): a += " RATED=" + self.eClRated.get().strip()
        if rsv:                                 a += " RSENSE=" + rsv
        if (self.eClModel.get() or "").strip(): a += " MODEL=" + self.eClModel.get().strip().upper()
        if dn(self.eClMfg):                     a += " MFG=%d" % dn(self.eClMfg)
        if dn(self.eClUse):                     a += " USE=%d" % dn(self.eClUse)
        if (self.eClHp.get() or "").strip():    a += " HEALTH=" + self.eClHp.get().strip()
        if id33:                                a += " ID33=1"
        if not self.vClZero.get():              a += " ZERO=0"
        if not self.vClRecheck.get():           a += " RECHECK=0"
        self.maybe_auth(lambda: (self.status("Режим копії..."),
                                 self.cmd("CLONE" + a, 30.0,
                                          cb=lambda r: self._after_write(r, "✅ Відновлено за зразком копії"))))

    def set_health(self):
        """Знос окремою дією. Рахує ПРИСТРІЙ (той самий restore_plan.h, що й у
        плані правок): друга формула тут одного дня розійшлася б із прошивкою."""
        if not self.need_conn():
            return
        try:
            v = int((self.eHp.get() or "").strip())
        except ValueError:
            messagebox.showwarning("Знос", "Вкажіть 1..100"); return
        if not (1 <= v <= 100):
            messagebox.showwarning("Знос", "Знос має бути 1..100 %"); return
        if not messagebox.askyesno("Знос", "Записати знос %d %%?\n\n"
                                           "Пишеться поле CTS у блок RECOND (DS2433)." % v):
            return
        self.maybe_auth(lambda: self.cmd("SETHEALTH %d" % v, 20.0,
            cb=lambda r: self._after_write(r, "✅ Знос %d %%" % v)))

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

    # ---- налаштування звуку ---------------------------------------------
    def sound_load(self, *_):
        if self.connected:
            self.cmd("SOUND", 5.0, cb=self._apply_sound)

    def _apply_sound(self, r):
        if not r.get("ok") or "sound" not in r:
            return
        s, lim = r["sound"], r.get("limits", {})
        self._sndQuiet = True                    # заповнення не має слати команду назад
        try:
            self.sndEn.set(bool(s.get("enabled", True)))
            self.sndClk.set(bool(s.get("click", True)))
            for key, _ck, _t, _st, _f in self.SND_FIELDS:
                lo, hi = lim.get(key, (0, 255))
                self.sndScale[key].configure(from_=lo, to=hi)
                self.sndVar[key].set(int(s.get(key, lo)))
                self._snd_show(key)
        finally:
            self._sndQuiet = False
        # Кнопки «прослухати» будуємо з переліку пристрою: сигнали живуть у
        # buzzer.h, і клієнт не має тримати їхню другу копію.
        for w in self.frSndTest.winfo_children():
            w.destroy()
        # У два стовпці, а не в один ряд: у Tk немає перенесення, і в один ряд
        # п'ять кнопок виїжджали б за край картки, обрізавши підписи.
        for i, sig in enumerate(r.get("signals", [])):
            ttk.Button(self.frSndTest,
                       text="▶ %s (%d мс)" % (sig.get("title", sig.get("key", "?")), sig.get("ms", 0)),
                       command=lambda k=sig.get("key"): self.sound_test(k)
                       ).grid(row=i // 2, column=i % 2, sticky="we", padx=2, pady=2)
        for c in range(2):
            self.frSndTest.columnconfigure(c, weight=1)
        if r.get("hasBuzzer") is False:
            self.lblSndHint.config(
                text="⚠️ У цій збірці буззер не підключено (BUZZER_PIN у settings.h не задано) —\n"
                     "налаштування зберігаються, але звучати нема чому.")

    def _snd_show(self, key):
        for k, _ck, _t, _st, fmt in self.SND_FIELDS:
            if k == key:
                self.sndLbl[k].config(text=fmt(int(self.sndVar[k].get())))
                return

    # Повзунок шлють після відпускання, а не на кожен піксель: інакше кожен рух
    # мишею — окрема команда й запис у SPIFFS, ресурс перезапису якої скінченний.
    def _snd_push(self, test=None):
        if getattr(self, "_sndQuiet", False) or not self.connected:
            return
        a = "SET en=%d clk=%d" % (1 if self.sndEn.get() else 0, 1 if self.sndClk.get() else 0)
        for key, ck, _t, _st, _f in self.SND_FIELDS:
            a += " %s=%d" % (ck, int(self.sndVar[key].get()))
        if test:
            a += " test=" + test
        self.cmd("SOUND " + a, 6.0, cb=self._apply_sound)

    # «Прослухати» несе й поточні значення повзунків: користувач має почути те,
    # що бачить на екрані, а не те, що встигло долетіти минулою командою.
    def sound_test(self, name):
        if not self.need_conn():
            return
        self._snd_push(test=name)

    def sound_reset(self):
        if not self.need_conn():
            return
        def done(r):
            self._apply_sound(r)
            if r.get("ok"):
                self.cmd("SOUND TEST ok", 4.0)
                self.status("🔊 Повернуто заводські значення", True)
        self.cmd("SOUND RESET", 6.0, cb=done)

    # ---- шаблони / файли -----------------------------------------------
    def load_templates(self, *_):
        self.cmd("TEMPLATES", 5.0, cb=self._apply_templates)

    def _apply_templates(self, r):
        models = r.get("models", []) if r.get("ok") else []
        for cb in (self.cbInit, self.cbRest, self.cbWiz):
            cb["values"] = models
            if models and not cb.get():
                cb.current(0)
        # Зміна моделі — інший еталон, отже інші числа донора: план треба
        # перебудувати, інакше галочки описували б попередню модель.
        if not getattr(self, "_rpBound", False):
            self._rpBound = True
            self.cbRest.bind("<<ComboboxSelected>>", lambda _e: self.rp_load(False))
        if models:
            self.rp_load(False)

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
