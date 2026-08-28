#ifndef DISPLAY_H
#define DISPLAY_H

#include "settings.h"
#include "impres_format.h"
#include "impres_bms.h"   // штатний декодер Motorola (цикли, знос, дати)
#include "battery_reader.h"
#include "templates.h"    // BATTERY_TEMPLATES/COUNT — для дій «Новий АКБ» у меню
#include "operations.h"   // ЄДИНИЙ каталог операцій (порядок/назви/небезпека)
#include "textwrap.h"     // txtFit(): назва пункту меню не лізе на сусідній рядок
#include "combo.h"        // приховані жести (чиста логіка, спільна з display_color.h)
#include "discharge.h"    // стан керованого розряду для сторінки моніторингу
#include "charge.h"       // стан керованого заряду для сторінки моніторингу

// Стан, яке відображаємо (заповнюється з .ino і обробників веб-сервера).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump[DUMP_SIZE];
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern uint8_t chipSN2438[8];
extern bool hasSN2438;
extern uint8_t chipSN2433[8];
extern bool hasSN2433;

// ── СТОРІНКИ ЕКРАНА ────────────────────────────────────────────────────────
//  ⚑ ДВА РІЗНІ РОДИ СТОРІНОК, І ЦЕ ГОЛОВНЕ В РОЗКЛАДЦІ.
//
//  Раніше всі вісім сторінок лежали в ОДНОМУ кільці, яке гортається по колу:
//  показання, потім два hex-дампи, потім «Дії», потім «Майстер». Через це
//  щоденні показання доводилось шукати між сторінками, куди заглядають раз на
//  місяць, а сама сторінка «Дії» була ще одним кільцем усередині першого — з
//  трьома десятками пунктів, які гортались ЛИШЕ ВПЕРЕД.
//
//  Тепер:
//    • КІЛЬЦЕ ПОКАЗАНЬ (0..NUM_STATUS_PAGES-1) — те, на що дивляться постійно.
//      Гортається «‹»/«›» по колу, як і раніше;
//    • МЕНЮ (PAGE_MENU) — один список УСЬОГО, що пристрій уміє: і операції, і
//      переходи на службові сторінки. Відкривається кнопкою OK з будь-якої
//      сторінки показань. Склад і порядок — у operations.h;
//    • СЛУЖБОВІ СТОРІНКИ (дампи, Майстер) — у кільце НЕ входять, їх відкриває
//      меню, а «‹» з них повертає назад у меню.
#define PAGE_MAIN         0   // головна (заряд + статус)
#define PAGE_MODEL        1   // модель і серійний номер
#define PAGE_TECH         2   // технічні дані DS2438
#define PAGE_HEALTH       3   // здоров'я: ємність / знос / цикли
#define NUM_STATUS_PAGES  4   // розмір кільця показань
#define PAGE_MENU         4   // список усіх функцій
#define PAGE_RAW38        5   // сирий дамп DS2438 (hex)
#define PAGE_RAW33        6   // сирий дамп DS2433 (hex)
#define PAGE_WIZARD       7   // Майстер відновлення (аналіз/проблеми/крок)
#define NUM_DISPLAY_PAGES 8   // усього сторінок (кільце + меню + службові)

// «Сторінковий» пункт меню -> номер сторінки екрана. Переклад потрібен саме
// тому, що operations.h НЕ ЗНАЄ про екран: там свої MPG_*, тут свої PAGE_*, і
// зв'язок між ними — рівно ця функція, а не збіг чисел (який одного дня
// перестав би бути збігом).
inline int menuPageToDisplayPage(int mpg) {
    switch (mpg) {
        case MPG_HOME:   return PAGE_MAIN;
        case MPG_RAW38:  return PAGE_RAW38;
        case MPG_RAW33:  return PAGE_RAW33;
        case MPG_WIZARD: return PAGE_WIZARD;
        default:         return PAGE_MAIN;
    }
}

// ========================================================================
// Кольоровий TFT (ST7789VW 240x240 / ST7789V3 240x280) — окрема реалізація
// в display_color.h (Adafruit_ST7789 + U8g2_for_Adafruit_GFX). Монохромний
// u8g2-шлях нижче лишається без змін і збирається, коли ST7789 НЕ вибраний.
//
// Кольоровий режим вмикається, якщо задано DISPLAY_ST7789_SPI АБО будь-який
// пресет розміру ST7789 (щоб не ловити помилку, коли забули головний перемикач).
// ========================================================================
#if defined(DISPLAY_ST7789_SPI)    || defined(DISPLAY_ST7789_240X240) || \
    defined(DISPLAY_ST7789_240X280) || defined(DISPLAY_ST7789_240X320) || \
    defined(DISPLAY_ST7789_135X240) || defined(DISPLAY_ST7789_170X320) || \
    defined(DISPLAY_ST7789_172X320) || (defined(DISPLAY_ST7789_W) && defined(DISPLAY_ST7789_H))
  #ifndef DISPLAY_ST7789_SPI
    #define DISPLAY_ST7789_SPI       // авто-вмикання за наявності пресету розміру
  #endif
#include "display_color.h"
#else
#include <U8g2lib.h>
#include <Wire.h>

// Адреса I2C дисплея. Дефолт 0x3C, якщо користувач закоментував/не задав —
// щоб монохромна збірка не падала «DISPLAY_I2C_ADDR was not declared».
#ifndef DISPLAY_I2C_ADDR
  #define DISPLAY_I2C_ADDR 0x3C
#endif

// Об'єкт дисплея вибирається по моделі з settings.h (повний буфер _F_).
// DISP_W/DISP_H — роздільність; DISPLAY_USES_I2C — ознака шини I2C.
//
// HW I2C: піни SCL/SDA передаються прямо в конструктор — U8g2 сама викликає
// Wire.begin(SDA, SCL). При увімкненому DISPLAY_HW_I2C додатково створюється
// запасний програмний (SW) об'єкт: якщо апаратна шина не відповідає (немає
// ACK — слабкі підтяжки, довгі проводи), displayInit автоматично
// перемикається на програмний I2C на тех же пінах. Кадровий буфер у пари
// об'єктів спільний (статичний в U8g2), зайвої пам'яті це майже не ест.
// ── CS для монохромних SPI-панелей ────────────────────────────────────────
//  На ST7567/PCD8544 контакт CS є практично завжди, але константа спільна з
//  кольоровою гілкою (ST7789 240x240 його не має взагалі), тож і тут вона
//  може бути не задана. U8g2 для такого випадку має U8X8_PIN_NONE — той самий
//  зміст, що й -1 у Adafruit: пін не чіпаємо, панель вибрана постійно.
#ifdef DISPLAY_CS_PIN
  #define U8G2_CS_ARG (DISPLAY_CS_PIN)
#else
  #define U8G2_CS_ARG U8X8_PIN_NONE
#endif

#if defined(DISPLAY_ST7567_SPI)
  #define DISP_W 128
  #define DISP_H 64
  // ST7567 (Open-Smart 1.8"), апаратний SPI (SCK=18, MOSI=23).
  // Варіант ENH_DG128064: коректна електрика (bias 1/9, помірний
  // контраст — варіант OS12864 з bias 1/7 дає темний засвічений екран).
  // Зсув картинки +4px робиться через x_offset в displayInit
  // (DISPLAY_ST7567_XOFF в settings.h).
  U8G2_ST7567_ENH_DG128064_F_4W_HW_SPI u8g2_spi(U8G2_R0, U8G2_CS_ARG, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
  static U8G2 *g_u8g2p = &u8g2_spi;
#elif defined(DISPLAY_PCD8544_SPI)
  #define DISP_W 84
  #define DISP_H 48
  // Nokia 5110 (PCD8544), 84x48, апаратний SPI.
  U8G2_PCD8544_84X48_F_4W_HW_SPI u8g2_spi(U8G2_R0, U8G2_CS_ARG, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
  static U8G2 *g_u8g2p = &u8g2_spi;
#elif defined(DISPLAY_SH1107_128_I2C)
  #define DISPLAY_USES_I2C 1
  #define DISP_W 128
  #define DISP_H 128
  U8G2_SH1107_128X128_F_SW_I2C u8g2_sw(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
  #if defined(DISPLAY_HW_I2C)
    U8G2_SH1107_128X128_F_HW_I2C u8g2_hw(U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);
    static U8G2 *g_u8g2p = &u8g2_hw;
  #else
    static U8G2 *g_u8g2p = &u8g2_sw;
  #endif
#elif defined(DISPLAY_SSD1327_128_I2C)
  #define DISPLAY_USES_I2C 1
  #define DISP_W 128
  #define DISP_H 128
  // Варіант "розводки" SSD1327 (см. settings.h). за замовчуванням MIDAS.
  #if   defined(SSD1327_WS)
    #define SSD1327_SW U8G2_SSD1327_WS_128X128_F_SW_I2C
    #define SSD1327_HW U8G2_SSD1327_WS_128X128_F_HW_I2C
  #elif defined(SSD1327_EA)
    #define SSD1327_SW U8G2_SSD1327_EA_W128128_F_SW_I2C
    #define SSD1327_HW U8G2_SSD1327_EA_W128128_F_HW_I2C
  #elif defined(SSD1327_ZJY)
    #define SSD1327_SW U8G2_SSD1327_ZJY_128X128_F_SW_I2C
    #define SSD1327_HW U8G2_SSD1327_ZJY_128X128_F_HW_I2C
  #else
    #define SSD1327_SW U8G2_SSD1327_MIDAS_128X128_F_SW_I2C
    #define SSD1327_HW U8G2_SSD1327_MIDAS_128X128_F_HW_I2C
  #endif
  SSD1327_SW u8g2_sw(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
  #if defined(DISPLAY_HW_I2C)
    SSD1327_HW u8g2_hw(U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);
    static U8G2 *g_u8g2p = &u8g2_hw;
  #else
    static U8G2 *g_u8g2p = &u8g2_sw;
  #endif
#elif defined(DISPLAY_SH1106_I2C)
  #define DISPLAY_USES_I2C 1
  #define DISP_W 128
  #define DISP_H 64
  U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2_sw(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
  #if defined(DISPLAY_HW_I2C)
    U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_hw(U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);
    static U8G2 *g_u8g2p = &u8g2_hw;
  #else
    static U8G2 *g_u8g2p = &u8g2_sw;
  #endif
#else   // DISPLAY_SSD1306_I2C — за замовчуванням
  #define DISPLAY_USES_I2C 1
  #define DISP_W 128
  #define DISP_H 64
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2_sw(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
  #if defined(DISPLAY_HW_I2C)
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_hw(U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);
    static U8G2 *g_u8g2p = &u8g2_hw;
  #else
    static U8G2 *g_u8g2p = &u8g2_sw;
  #endif
#endif

// Весь код нижче працює через вказівник: звернення "u8g2." ідуть до активному
// об'єкту (апаратному або запасному програмному після fallback).
#define u8g2 (*g_u8g2p)

// Дозволена частота шини I2C: ручна (KHZ>0) або авто-безпечна з драйвера
// (SSD1327 — 100 кГц, решта — 400 кГц). Викликається Завжди (як в робочій
// версії), а не пропускається — так надійніше і для HW, і як no-op для SW.
#if DISPLAY_I2C_KHZ > 0
  #define DISPLAY_CLK_HZ (DISPLAY_I2C_KHZ * 1000UL)
#elif defined(DISPLAY_SSD1327_128_I2C)
  #define DISPLAY_CLK_HZ 100000UL   // межа SSD1327
#else
  #define DISPLAY_CLK_HZ 400000UL
#endif
#define DISPLAY_PROBE_HZ DISPLAY_CLK_HZ

// SSD1327 (відтінки сірого) на деяких панелях стартує тьмяно/порожньо —
// задаємо помітний контраст за замовчуванням, якщо користувач не вказав свій.
#if defined(DISPLAY_SSD1327_128_I2C) && !defined(DISPLAY_CONTRAST)
  #define DISPLAY_CONTRAST 0x7F
#endif

// Розмітка меню под роздільність екрана: шрифти, шапка, рядки тела (ROW),
// нижня рядок статусу. На 128x128 — більший шрифт і більше рядків.
#if DISP_H >= 128            // 128x128 (GME128128-02 і т.п.)
  #define HEAD_FONT  u8g2_font_6x12_t_cyrillic
  #define HEAD_Y     10                // базова лінія заголовка
  #define HEAD_LINE  13                // Y роздільної лінії шапки
  #define BODY_FONT  u8g2_font_6x12_t_cyrillic
  #define BODY_Y0    27                // базова лінія першої рядки тела
  #define ROW_H      14                // крок рядків
  #define FOOT_HL    (DISP_H - 15)     // лінія над статусом
  #define FOOT_Y     (DISP_H - 3)      // базова лінія статусу
#elif DISP_H >= 64           // 128x64 (SSD1306/SH1106/ST7567)
  #define HEAD_FONT  u8g2_font_5x8_t_cyrillic
  #define HEAD_Y     7
  #define HEAD_LINE  9
  #define BODY_FONT  u8g2_font_5x8_t_cyrillic
  #define BODY_Y0    18
  #define ROW_H      9
  #define FOOT_HL    53
  #define FOOT_Y     62
#else                        // 84x48 (Nokia 5110)
  #define HEAD_FONT  u8g2_font_5x8_t_cyrillic
  #define HEAD_Y     7
  #define HEAD_LINE  9
  #define BODY_FONT  u8g2_font_5x8_t_cyrillic
  #define BODY_Y0    16
  #define ROW_H      8
  #define FOOT_HL    (DISP_H - 9)
  #define FOOT_Y     (DISP_H - 2)
#endif
// Базова лінія n-й рядки тела сторінки.
#define ROW(n) (BODY_Y0 + (n) * ROW_H)

static char g_displayStatus[36] = "ЗАПУСК";  // нижня рядок статусу (UTF-8)
static int  g_displayPage = 0;             // поточна сторінка меню
// Стан прихованих жестів і повноекранного повідомлення: на них дивиться і
// обробник кнопок, і рендер.
static ComboHold  g_hold;
static ComboSeq   g_seq;
static ComboFlash g_flash;
inline bool displayFlashActive() { return comboFlashActive(g_flash, millis()); }
static bool g_readRequested = false;       // запит повторного читання після циклу
// Сторінка «Дії»: вибір операції (BTN2 коротко) + виконання (BTN2 довго).
static int  g_menuSel = 1;                 // курсор у списку (0 — «‹ Показання»)
static int  g_actionRequested = -1;        // -1 нема; інакше — КОД операції для .ino

// Екранний Майстер відновлення. Рендер (тут) читає ці глобали; заповнює їх
// wizDeviceRefresh() з recovery.h, а .ino оркеструє (читання/крок).
static int  g_wizReq      = 0;             // запит для .ino: 0 нема, 1 аналіз, 2 крок
static int  g_wizProblems = -1;            // -1 ще не аналізовано; інакше к-сть проблем
static bool g_wizHealthy  = false;         // усе гаразд, відновлення не треба
static int  g_wizProg = 0, g_wizTotal = 0; // прогрес плану
static bool g_wizAwait = false;            // чекаємо повернення з зарядної станції
static bool g_wizBusy  = false;            // виконується крок (анімація)
static char g_wizTop[48]  = "";            // топ-проблема (людською)
static char g_wizNext[48] = "";            // назва наступного кроку

// Анімація батареї на головній сторінці: фаза «блику» + збережений прямокутник
// іконки (щоб оновлювати ЛИШЕ його область, не перемальовуючи весь екран —
// критично для повільного SSD1327).
static uint8_t g_animPhase = 0;
static int g_battX = 0, g_battY = 0, g_battW = 0, g_battH = 0;

inline void displayRender(); // визначення нижче

// Емблема Національної Гвардії України для стартової заставки (1-біт XBM).
#define NGU_W 64
#define NGU_H 64
static const unsigned char ngu_xbm[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0, 0x09, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xDC, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE,
  0x3F, 0x03, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xFE, 0xBF, 0x07, 0x00, 0x00,
  0x00, 0x00, 0xE0, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xFF,
  0xFF, 0x07, 0x01, 0x00, 0x00, 0x00, 0xFC, 0xFD, 0xFF, 0x07, 0x03, 0x00,
  0x00, 0x00, 0xEF, 0xFF, 0xFF, 0xB7, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xFF,
  0xFF, 0xFF, 0x07, 0x00, 0x00, 0xF8, 0xEF, 0xFF, 0xFF, 0xFF, 0x03, 0x00,
  0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0xFA, 0xFF, 0xFF,
  0xFF, 0xFF, 0x3F, 0x00, 0x00, 0xFA, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00,
  0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0xFE, 0xFF, 0xFF,
  0xFF, 0xFF, 0x0F, 0x00, 0x00, 0xFC, 0xFF, 0xBF, 0xFF, 0xFF, 0x7F, 0x00,
  0x80, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x80, 0xFF, 0xCF, 0xFF,
  0xFF, 0xFF, 0xFF, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xEF, 0xFF, 0xFF, 0x01,
  0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0xF8, 0xFF, 0xF8,
  0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xF8, 0x07, 0x00,
  0x00, 0xC0, 0x23, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0xFF,
  0xFF, 0x00, 0x30, 0x00, 0x00, 0xFC, 0x00, 0xFC, 0x3F, 0x00, 0x3F, 0x00,
  0x00, 0xF8, 0x1F, 0xFE, 0x3F, 0xF8, 0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF,
  0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xF8, 0xFF, 0x0F, 0xC0, 0xFF, 0x0F, 0x00,
  0x00, 0xF0, 0xFF, 0x00, 0x00, 0xFE, 0x0F, 0x00, 0x00, 0xF0, 0x3F, 0x00,
  0x00, 0xF8, 0x0F, 0x00, 0x00, 0xE0, 0x1F, 0x80, 0x01, 0xF0, 0x07, 0x00,
  0x00, 0xE0, 0x07, 0x81, 0x81, 0xC1, 0x07, 0x00, 0x00, 0xC0, 0x03, 0x83,
  0xC1, 0x80, 0x03, 0x00, 0x00, 0xE0, 0x01, 0x85, 0xA1, 0x81, 0x03, 0x00,
  0x00, 0xF0, 0x00, 0x8D, 0xB1, 0x01, 0x0F, 0x00, 0x00, 0xFE, 0x00, 0x89,
  0x91, 0x01, 0x7F, 0x00, 0x80, 0x7F, 0x00, 0x89, 0x91, 0x01, 0xFE, 0x01,
  0xE0, 0x7F, 0x00, 0x89, 0x91, 0x01, 0xFE, 0x07, 0xFC, 0x7F, 0x00, 0x99,
  0x91, 0x01, 0xFE, 0x3F, 0xFE, 0x7F, 0x00, 0x9D, 0x99, 0x01, 0xFE, 0x7F,
  0xE0, 0x7F, 0x00, 0xC7, 0xE2, 0x00, 0xFE, 0x07, 0x80, 0x7F, 0x00, 0x65,
  0xA6, 0x01, 0xFE, 0x01, 0x00, 0x7C, 0x00, 0x79, 0x9E, 0x01, 0x7F, 0x00,
  0x00, 0xF0, 0x00, 0xB1, 0x89, 0x01, 0x1F, 0x00, 0x00, 0xC0, 0x00, 0xFF,
  0xFF, 0x81, 0x07, 0x00, 0x00, 0xE0, 0x01, 0xB0, 0x0D, 0xC0, 0x03, 0x00,
  0x00, 0xE0, 0x03, 0xA0, 0x05, 0xC0, 0x03, 0x00, 0x00, 0xE0, 0x07, 0xC0,
  0x07, 0xF0, 0x07, 0x00, 0x00, 0xF0, 0x1F, 0x80, 0x01, 0xF8, 0x07, 0x00,
  0x00, 0xF0, 0x7F, 0x00, 0x00, 0xFE, 0x0F, 0x00, 0x00, 0xF8, 0xFF, 0x03,
  0xC0, 0xFF, 0x0F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0x17, 0x00,
  0x00, 0xF8, 0x1F, 0xFE, 0xFF, 0xF8, 0x1F, 0x00, 0x00, 0xFC, 0x00, 0xFC,
  0x7F, 0x00, 0x3D, 0x00, 0x00, 0x0C, 0x00, 0xFC, 0x3F, 0x00, 0x30, 0x00,
  0x00, 0x00, 0x00, 0xF0, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0,
  0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x05, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
  0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Перерахунок контрольної суми TLV-записи DS2433: сума усіх байт == 0x5A.
inline void fixRecordChecksum(uint8_t *buf, int start, int len) {
    int s = 0;
    for (int k = 0; k < len - 1; k++) s += buf[start + k];
    buf[start + len - 1] = (0x5A - s) & 0xFF;
}

// ---------- базова налаштування ----------

inline void displayInit() {
#if defined(DISPLAY_USES_I2C) && defined(DISPLAY_HW_I2C)
    // Проба апаратної шини НА Робочій Частоті до u8g2.begin(). Повторний
    // Wire.begin теми же пінами всередині U8g2 безвреден ("already started").
    // Якщо дисплей не відповідає (немає ACK: слабкі підтяжки, довгі проводи,
    // примхливий контролер) — освобождаем піни і автоматично переходимо
    // на запасний програмний I2C: дисплей працює завжди, лише медленнее.
    Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, DISPLAY_PROBE_HZ);
    Wire.setTimeOut(50);
    bool hwOk = true;
    for (int i = 0; i < 3 && hwOk; i++) {
        Wire.beginTransmission(DISPLAY_I2C_ADDR);
        hwOk = (Wire.endTransmission() == 0);
    }
    if (!hwOk) {
        Wire.end();                 // отдать піни біт-бенгу
        g_u8g2p = &u8g2_sw;         // запасний програмний I2C
        Serial.printf("DISPLAY: HW I2C no ACK at 0x%02X (SDA=%d SCL=%d) -> SW I2C fallback\n",
                      DISPLAY_I2C_ADDR, DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    } else {
        Serial.println("DISPLAY: HW I2C OK");
    }
#endif
#if defined(DISPLAY_USES_I2C)
    u8g2.setI2CAddress(DISPLAY_I2C_ADDR << 1);
    u8g2.setBusClock(DISPLAY_CLK_HZ);   // завжди (для HW важливо, для SW no-op)
#endif
    u8g2.begin();
#if defined(DISPLAY_ST7567_SPI)
    // Панель Open-Smart: RAM ST7567 на 132 колонки, скло показує 4..131.
    u8g2.getU8x8()->x_offset = DISPLAY_ST7567_XOFF;
#endif
    // Стартуємо ЗАТЕМНЕНО й з чорним екраном, щоб артефакти ініціалізації не
    // блимали до заставки; яскравість плавно піднімемо у displayIntro().
    u8g2.setContrast(0);
    u8g2.clearDisplay();
    u8g2.setFont(BODY_FONT);
    Serial.printf("DISPLAY: %dx%d, I2C clock=%lu Hz\n", (int)DISP_W, (int)DISP_H, (unsigned long)DISPLAY_CLK_HZ);
}

// Цільова яскравість (контраст) у робочому режимі.
#if defined(DISPLAY_CONTRAST)
  #define DISP_BRIGHT DISPLAY_CONTRAST
#else
  #define DISP_BRIGHT 255
#endif

// Плавне керування яскравістю (для моно — контраст контролера).
inline void displaySetBrightness(uint8_t v) { u8g2.setContrast(v); }

// Стартова заставка: тризуб + "Національна Гвардія України".
inline void displaySplash() {
    u8g2.clearBuffer();
#if DISP_H >= 128
    // 128x128: емблема по центру зверху, текст под ній.
    u8g2.drawXBM((DISP_W - NGU_W) / 2, 6, NGU_W, NGU_H, ngu_xbm);
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    u8g2.drawUTF8(7,  88,  "Національна Гвардія");   // 19 зн. x 6px = 114
    u8g2.drawUTF8(43, 103, "України");                //  7 зн. x 6px = 42
    u8g2.drawUTF8(31, 121, "IMPRES tool");            // 11 зн. x 6px = 66
#elif (DISP_H >= 64) && (DISP_W >= 110)
    // 128x64: емблема зліва + текст справа.
    u8g2.drawXBM(0, 0, NGU_W, NGU_H, ngu_xbm);
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(66, 18, "Національна");
    u8g2.drawUTF8(66, 34, "Гвардія");
    u8g2.drawUTF8(66, 50, "України");
#else
    // Дрібні екрани (Nokia 5110): лише текст.
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(0, 12, "Нац. Гвардія");
    u8g2.drawUTF8(0, 24, "України");
    u8g2.drawUTF8(0, 40, "IMPRES tool");
#endif
    u8g2.sendBuffer();
}

// Плавна поява/зникнення заставки. Викликати ОДИН раз у setup() замість
// displaySplash()+delay(): екран лишається чорним до появи, потім заставка
// плавно проявляється, тримається і плавно згасає (жодних артефактів/блимань).
inline void displayIntro() {
    displaySplash();                         // у буфер (контраст 0 -> ще не видно)
    for (int c = 0; c <= DISP_BRIGHT; c += 8) { u8g2.setContrast(c); delay(18); }
    u8g2.setContrast(DISP_BRIGHT);
    delay(1300);                             // тримаємо заставку
    for (int c = DISP_BRIGHT; c >= 0; c -= 8) { u8g2.setContrast(c < 0 ? 0 : c); delay(12); }
    u8g2.setContrast(0);
    u8g2.clearDisplay();                     // чорно перед входом у меню
}

// Плавний вхід у головне меню (наприкінці setup()): малюємо головну сторінку
// затемнено й плавно піднімаємо яскравість.
inline void displayFadeInMain() {
    g_displayPage = 0;
    displayRender();                         // контраст 0 -> малюнок ще невидимий
    for (int c = 0; c <= DISP_BRIGHT; c += 8) { u8g2.setContrast(c); delay(16); }
    u8g2.setContrast(DISP_BRIGHT);
}

inline void displaySetStatus(const char *s) {
    strncpy(g_displayStatus, s, sizeof(g_displayStatus) - 1);
    g_displayStatus[sizeof(g_displayStatus) - 1] = '\0';
}

inline void displayShow(const char *s) {
    displaySetStatus(s);
    displayRender();
}

// Червоний «світлофільтр» — лише для кольорових панелей. На монохромному екрані
// кольору немає, тож заглушка (текст «...ЗБІЙ» і так показує помилку).
inline void displaySetErrorTint(bool) {}

// ---------- допоміжні елементи відмальовування ----------

// counter — що показати праворуч замість номера сторінки (напр. «12/36» у
// меню). nullptr/порожній = номер сторінки в кільці показань.
inline void drawHeader(const char *title, const char *counter = nullptr) {
    char h[16];
    u8g2.setFont(HEAD_FONT);
    u8g2.drawUTF8(0, HEAD_Y, title);
    // ⚑ «!» перед номером сторінки — ознака несправності ЖИВЛЕННЯ. Вона тут, у
    // спільній шапці, а не на окремій сторінці, саме тому, що видима мусить
    // бути з БУДЬ-ЯКОЇ сторінки: без блока живлення заряд не піде, хай що
    // користувач зараз гортає. Розшифровка — на сторінці заряду й у вебі.
    // Лічильник — тільки по кільцю показань: службові сторінки в нього не
    // входять, і «6/8» на них обіцяло б гортання, якого немає.
    if (counter && *counter)
        snprintf(h, sizeof(h), "%s%s", chargePsuFault() ? "!" : "", counter);
    else if (g_displayPage < NUM_STATUS_PAGES)
        snprintf(h, sizeof(h), "%s%d/%d", chargePsuFault() ? "!" : "",
                 g_displayPage + 1, NUM_STATUS_PAGES);
    else
        snprintf(h, sizeof(h), "%s", chargePsuFault() ? "!" : "");
    u8g2.drawStr(DISP_W - u8g2.getStrWidth(h) - 1, HEAD_Y, h);
    u8g2.drawHLine(0, HEAD_LINE, DISP_W);
}

inline void drawFooter() {
    char f[42];
    u8g2.setFont(BODY_FONT);
    u8g2.drawHLine(0, FOOT_HL, DISP_W);
    snprintf(f, sizeof(f), ">%s", g_displayStatus);
    u8g2.drawUTF8(0, FOOT_Y, f);
}

// Іконка батареї зі шкалою заповнення; pct<0 — даних немає.
inline void drawBatteryIcon(int x, int y, int w, int h, int pct) {
    g_battX = x; g_battY = y; g_battW = w; g_battH = h;   // для displayAnimTick()
    u8g2.drawFrame(x, y, w, h);
    u8g2.drawBox(x + w, y + h / 3, 3, h - 2 * (h / 3)); // "плюсовий" вивід
    if (pct < 0) return;
    int fillw = (w - 4) * pct / 100;
    if (fillw < 0) fillw = 0;
    if (fillw > w - 4) fillw = w - 4;
    if (fillw > 0) u8g2.drawBox(x + 2, y + 2, fillw, h - 4);   // анімація — у displayAnimTick()
}

// Відсоток заряду. Пріоритет — ICA (якщо увімкнений облік струму IAD=1),
// інакше оцінка по напрузі. src отримує мітку джерела ("ICA"/"volt"/"--").
inline int batteryPercent(const char **src) {
    if (!hasDump2438) { *src = "--"; return -1; }

    // % за напругою — завжди доступна опора (VDD DS2438, стр.0 байти 3..4, LE).
    long vmv = (long)(((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3]) * 10;
    int vpct = (int)((vmv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
    if (vpct < 0) vpct = 0; if (vpct > 100) vpct = 100;

    uint8_t config = batteryDump2438[0];        // стр.0 байт0 = Status/Config
    if (config & 0x01) {                         // IAD=1 -> ICA підтримується
        // Шкала ICA АПАРАТНА: одиниця = 0.4882 мВ·год / Rsense, а не «255 =
        // повний пакет». Тому відсоток рахуємо через мА·год і паспортну
        // ємність — інакше повний пакет показувався б як ~79 % (2150 мА·год
        // при шунті 0.0459 Ом — це 202 одиниці, а не 255).
        char pm[16] = "";
        if (hasDump) impresModelName(batteryDump, pm, sizeof(pm));
        int ica = impresPercentFromIca(batteryDump2438[12],
                                       impresRatedMahFor(hasDump ? batteryDump : nullptr, pm),
                                       impresBmsRsense(batteryDump2438));
        // Паливомір «завис»/не відкалібрований: ICA каже майже порожньо, а напруга
        // — суттєво більше (напр. після заміни банок/стирання, до калібрування на
        // ЗП). Тоді показуємо реальний рівень за НАПРУГОЮ, а джерело позначаємо "U!"
        // (щоб не збивати з пантелику 0% на фактично повному АКБ).
        if (ica + 25 < vpct) { *src = "U!"; return vpct; }
        *src = "ICA";
        return ica;
    }

    *src = "volt";                               // ICA не підтримується — по напрузі
    return vpct;
}

// Знайти модель (part number, напр. "PMNN4409A"/"PT4409A") в дампі DS2433.
// Пріоритет — авторитетна запис моделі: тег 0x0B, потім ASCII-назва,
// що починається з 'P' (перевірено на усіх дампах: 0B 50 4D 4E 4E ... = "PMNN...";
// 0B 50 54 ... = "PT..."). Запасний шлях — компактна алфавітно-цифрова
// рядок 7..11 з цифрою (відсікає "COPYRIGHT...MOTOROLASOLUTIONS").
inline bool decodeModel(char *out, size_t n) {
    if (!hasDump) return false;

    // 1) Валідний запис моделі: довжина 0x0B, 9 символів [A-Z0-9 ], Σ≡0x5A
    //    (штатне місце 0x148). Перевірка суми прибирає хибні влучання
    //    «0x0B + літера» на частково стертих чипах.
    if (impresModelName(batteryDump, out, n)) return true;

    // 2) Запасний варіант: найбільш довга компактна рядок [A-Z0-9] з цифрою.
    int best = -1, bestLen = 0, i = 0;
    while (i < (int)DUMP_SIZE) {
        uint8_t c = batteryDump[i];
        if (c >= 'A' && c <= 'Z') {
            int j = i + 1;
            bool hasDigit = false;
            while (j < (int)DUMP_SIZE) {
                uint8_t d = batteryDump[j];
                if (d >= '0' && d <= '9') { hasDigit = true; j++; }
                else if (d >= 'A' && d <= 'Z') j++;
                else break;
            }
            int len = j - i;
            if (hasDigit && len >= 7 && len <= 11 && len > bestLen) { bestLen = len; best = i; }
            i = j;
        } else i++;
    }
    if (best < 0) return false;
    int len = bestLen;
    if ((size_t)len >= n) len = n - 1;
    memcpy(out, batteryDump + best, len);
    out[len] = '\0';
    return true;
}

// Здоров'я / строк служби, %.
//
// ⚠️ РАНІШЕ повертали байт зі зсуву +21 у першому записі довжини 0x17.
// Це неправильно: перший байт запису — ДОВЖИНА, а не тег, і сам запис @0x129 —
// ЗАВОДСЬКА таблиця моделі (у dumps/ вона побайтово однакова в усіх 19
// екземплярів PMNN4409A і всіх 8 екземплярів PMNN4409B, байт +21 завжди 0x64).
// Тобто програма ЗАВЖДИ показувала «100% / знос 0%» — саме про цю розбіжність
// із показаннями станції й писав власник.
//
// Перебір усіх зсувів і кодувань по 7 АКБ із відомими показаннями рації збігу
// не дав: строк служби в прошивці НЕ зберігається, його рахує рація. Тому
// чесно повертаємо «невідомо».
inline bool decodeCapacity(int *capPct, int *wearPct) {
    (void)capPct; (void)wearPct;
    return false;
}

// Евристика справжності / цілісності ПРОШИВКИ (не «чи живі елементи»).
// МОДЕЛЬНО-ЗАЛЕЖНА: блок автентифікації "MOTOROLA" і калібрувальний підпис
// 0x1B-0x1E є лише у частини моделей (PMNN4488A/4493A, формат 2017). У
// PMNN4409A (формат 2014) їх штатно НЕМАЄ — перевірено на РОБОЧОМУ 4409A, який
// рація приймає. Тому відсутність MOTOROLA-блоку — НЕ ознака підробки.
// Червоні прапори (для будь-якої моделі): побитий заголовок (Σ≠0x41),
// відсутній запис моделі (порожній/стертий чіп), переповнений CCA (0xFFFF).
// Додатково, ЛИШЕ якщо MOTOROLA-блок присутній, вимагаємо непорожній підпис
// 0x1B-0x1E (його стирання — ознака побитого 2017-калібрування).
// Повертає true, якщо прошивка виглядає цілісною; в reason — стисла причина.
inline bool batteryGenuine(const char **reason) {
    if (!hasDump) { *reason = "нема дампу"; return false; }

    // Заголовок DS2433: сума 0x00..0x1F має бути ≡0x41 (інакше стертий/побитий).
    int hs = 0; for (int i = 0; i <= 0x1F; i++) hs += batteryDump[i];
    if ((hs & 0xFF) != 0x41) { *reason = "хибний заголовок"; return false; }

    // Має бути запис моделі 0x0B (ідентичність). Немає — чіп без прошивки.
    // Валідний запис моделі (довжина 0x0B, 9 символів, Σ≡0x5A). Раніше тут
    // вистачало «байт 0x0B, за яким літера», через що сміттєвий чіп міг
    // вважатися таким, що має модель.
    bool hasModel = impresFindModel(batteryDump) >= 0;
    if (!hasModel) { *reason = "нема моделі"; return false; }

    // Переповнений лічильник заряду CCA (0xFFFF) в DS2438 — типова ознака
    // збитої/«залоченої» АКБ (часто після заміни елементів).
    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) { *reason = "CCA перепов."; return false; }
    }

    // Блок автентифікації "MOTOROLA" — лише у моделей формату 2017.
    bool auth = false;
    static const char pat[] = "MOTOROLA";
    const int plen = 8;
    for (int i = 0; i + plen <= (int)DUMP_SIZE && !auth; i++) {
        int k = 0;
        while (k < plen && batteryDump[i + k] == (uint8_t)pat[k]) k++;
        if (k == plen) auth = true;
    }
    if (auth) {
        // Формат 2017: додатково вимагаємо непорожній калібрувальний підпис.
        if (batteryDump[0x1B] == 0xFF && batteryDump[0x1C] == 0xFF &&
            batteryDump[0x1D] == 0xFF && batteryDump[0x1E] == 0xFF) {
            *reason = "стерте калібр."; return false;
        }
        *reason = "OK"; return true;
    }

    // Немає MOTOROLA-блоку — формат без автентифікації (напр. 4409A). Заголовок
    // і модель у нормі, CCA не переповнений => прошивка ціла. Не підробка.
    *reason = "OK (ф.2014)";
    return true;
}

// ---------- сторінки меню ----------

// залишкова ємність (заряд) в мА·ч з регістра ICA (DS2438 байт 12).
// Залишок, мА·год: паливомір ICA перерахований через ПАСПОРТНУ ємність моделі
// (див. impres_format.h). Раніше множили на DS2438_MAH_PER_LSB, і повна шкала
// виходила ~4978 мА·год — більше за сам пакет.
inline int batteryRemainingMah() {
    if (!hasDump2438) return -1;
    char m[16] = "";
    if (hasDump) impresModelName(batteryDump, m, sizeof(m));
    return impresIcaToMahRs(batteryDump2438[12],
                            impresRatedMahFor(hasDump ? batteryDump : nullptr, m),
                            impresBmsRsense(batteryDump2438));
}

inline void drawPageMain() {
    char buf[40];
    const char *src;
    int pct = batteryPercent(&src);             // рівень заряду, % (головний показник)
    int mah = batteryRemainingMah();            // залишок у мА·год (додатково)

    drawHeader("Moto IMPRES");

#if DISP_H >= 128
    // 128x128: великим — заряд %, нижче — мА·год, напруга, IP.
    drawBatteryIcon(0, 22, 56, 22, pct);
    u8g2.setFont(u8g2_font_10x20_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(62, 40, buf);
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(62, 54, src);                 // джерело заряду (ICA/напруга)
    if (mah >= 0) snprintf(buf, sizeof(buf), "залишок %d мА·год", mah);
    else          snprintf(buf, sizeof(buf), "залишок: --");
    u8g2.drawUTF8(0, 70, buf);
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        // Струм із вбудованого датчика DS2438 (резистор усередині пакета,
        // послідовно з банками). Від'ємний = розряд.
        int16_t iraw = (int16_t)(((uint16_t)batteryDump2438[6] << 8) | batteryDump2438[5]);
        int     i_mA = (int)((float)iraw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f);
        snprintf(buf, sizeof(buf), "%.2fV %dmA %.1fC", vraw * 0.01f, i_mA, traw * 0.03125f);
    } else snprintf(buf, sizeof(buf), "DS2438: немає даних");
    u8g2.drawUTF8(0, 84, buf);
    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);        u8g2.drawUTF8(0, 96, buf);
    // Точка доступу й пароль — поруч з IP: щоб під'єднатися з телефона,
    // потрібні всі три, а шукати їх у settings.h саме тоді, коли пристрій в
    // руках, — найгірший момент. Дрібним шрифтом: місця на 128x128 обмаль.
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    snprintf(buf, sizeof(buf), "Wi-Fi: %s", AP_SSID);    u8g2.drawUTF8(0, 105, buf);
    snprintf(buf, sizeof(buf), "Пароль: %s", AP_PASSWORD); u8g2.drawUTF8(0, 113, buf);
    // Підказка по кнопках — нижче й ТИМ САМИМ шрифтом, що показання (раніше
    // ділила рядок із рештою і читалась гірше за все на екрані).
    u8g2.setFont(BODY_FONT);
#ifdef MENU_BTN3_PIN
    u8g2.drawUTF8(0, FOOT_HL - 2, "[<][>] меню  [OK] чит.");
#else
    u8g2.drawUTF8(0, FOOT_HL - 2, "[>] довго - зчитати");
#endif
#elif DISP_W < 100
    // Nokia 84x48: компактно.
    drawBatteryIcon(0, 12, 34, 12, pct);
    u8g2.setFont(u8g2_font_6x12_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(40, 22, buf);
    u8g2.setFont(BODY_FONT);
    if (mah >= 0) snprintf(buf, sizeof(buf), "%d мА·год", mah); else snprintf(buf, sizeof(buf), "залишок --");
    u8g2.drawUTF8(0, 31, buf);
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        snprintf(buf, sizeof(buf), "%.2fV  %s", vraw * 0.01f, ESP_IP);
    } else snprintf(buf, sizeof(buf), "%s", ESP_IP);
    u8g2.drawUTF8(0, 39, buf);
#else
    // 128x64: великим — заряд %, справа джерело, нижче мА·год/напруга, IP.
    drawBatteryIcon(0, 13, 52, 14, pct);
    u8g2.setFont(u8g2_font_10x20_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(58, 27, buf);
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(104, 20, src);
    if (mah >= 0) snprintf(buf, sizeof(buf), "залишок %d мА·год", mah);
    else          snprintf(buf, sizeof(buf), "залишок: --");
    u8g2.drawUTF8(0, 39, buf);
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2fV %.1fC %s", vraw * 0.01f, traw * 0.03125f, ESP_IP);
    } else snprintf(buf, sizeof(buf), "DS2438 нема  %s", ESP_IP);
    u8g2.drawUTF8(0, 48, buf);
    // 128x64: цілого рядка під точку доступу немає, тож SSID і пароль ідуть
    // одним дрібним рядком під IP. Довгий SSID обріжеться — це чесніше, ніж
    // не показати пароль узагалі.
    u8g2.setFont(u8g2_font_4x6_t_cyrillic);
    snprintf(buf, sizeof(buf), "%s / %s", AP_SSID, AP_PASSWORD);
    u8g2.drawUTF8(0, 55, buf);
#endif

    drawFooter();
}

inline void drawPageModel() {
    drawHeader("Модель / Серійний");

    char model[24];
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(0, ROW(0), "Модель:");
    if (decodeModel(model, sizeof(model))) {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.drawStr(6, ROW(1) + 4, model);
    } else {
        u8g2.setFont(BODY_FONT);
        u8g2.drawUTF8(48, ROW(0), hasDump ? "(невідомо)" : "(зчитайте)");
    }

    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(0, ROW(3), "Серійний (DS2438):");
    if (hasSN2438) {
        char sn[20];
        int p = 0;
        for (int i = 0; i < 8; i++) p += snprintf(sn + p, sizeof(sn) - p, "%02X", chipSN2438[i]);
        u8g2.drawStr(6, ROW(4), sn);
    } else {
        u8g2.drawUTF8(6, ROW(4), "(зчитайте АКБ)");
    }
#if DISP_H >= 128
    drawFooter();   // на малих екранах не вміщається — там сторінка без статусу
#endif
}

inline void drawPageTech() {
    char buf[40];
    drawHeader("Дані батареї");

    if (!hasDump2438) {
        u8g2.setFont(BODY_FONT);
        u8g2.drawUTF8(0, ROW(0), "Немає даних DS2438.");
        u8g2.drawUTF8(0, ROW(1), "Спочатку зчитайте АКБ.");
        drawFooter();
        return;
    }

    uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
    int16_t  traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
    int16_t  iraw = (int16_t)(((uint16_t)batteryDump2438[6] << 8) | batteryDump2438[5]);
    float    i_mA = (float)iraw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f;
    uint8_t  rem  = batteryDump2438[12];                                             // ICA

    int remMah = batteryRemainingMah();            // залишок в мА*ч (за моделлю)

    u8g2.setFont(BODY_FONT);
    snprintf(buf, sizeof(buf), "Напруга:  %.2f V", vraw * 0.01f);      u8g2.drawUTF8(0, ROW(0), buf);
    snprintf(buf, sizeof(buf), "Струм:    %.0f mA", i_mA);             u8g2.drawUTF8(0, ROW(1), buf);
    snprintf(buf, sizeof(buf), "Темп:     %.1f C", traw * 0.03125f);   u8g2.drawUTF8(0, ROW(2), buf);
    snprintf(buf, sizeof(buf), "Залишок: ~%d mAh", remMah);            u8g2.drawUTF8(0, ROW(3), buf);

    drawFooter();
}

// Сторінка «Стан АКБ».
//
//  Тут раніше стояло «Ємність: (зчитайте)» — і воно НЕ зникало після зчитування,
//  бо decodeCapacity() принципово повертає false: строк служби в прошивці не
//  зберігається, його рахує рація. Напис штовхав шукати неіснуючу дію.
//
//  Тепер показуємо те, що прошивка справді знає: ПАСПОРТНУ ємність за моделлю і
//  ЗАЛИШОК за паливоміром DS2438. Рядки йдуть за спаданням важливості: на 128x64
//  їх влазить лише чотири, тож зауваження про знос дістається великим екранам.
inline void drawPageHealth() {
    char buf[48];
    drawHeader("Стан АКБ");
    u8g2.setFont(BODY_FONT);

    int r = 0;
    auto row = [&](const char *txt) {
        if (ROW(r) >= FOOT_HL) return;
        u8g2.drawUTF8(0, ROW(r), txt); r++;
    };

    if (!hasDump && !hasDump2438) {
        row("Ємність: зчитайте АКБ");
    } else {
        char m[16] = "";
        if (hasDump) impresModelName(batteryDump, m, sizeof(m));
        snprintf(buf, sizeof(buf), "Ємність: %d мА*год",
                 impresRatedMahFor(hasDump ? batteryDump : nullptr, m));
        row(buf);
        int rem = batteryRemainingMah();
        if (rem >= 0) {
            const char *src; int pct = batteryPercent(&src);
            snprintf(buf, sizeof(buf), "Залишок: %d (%d%%)", rem, pct < 0 ? 0 : pct);
            row(buf);
        }
    }

    // Вирок про справжність — ТРЕТІМ рядком, до лічильника циклів: на 84x48
    // влазить лише три рядки, і там він потрібніший за кількість циклів.
    // Поки нічого не зчитано, вироку немає: «РИЗИК: нема дампу» лише лякав би.
    if (hasDump || hasDump2438) {
        const char *reason;
        if (batteryGenuine(&reason)) {
            row("Справжня: ТАК");
        } else {
            snprintf(buf, sizeof(buf), "РИЗИК: %s", reason);
            row(buf);
        }
    }

    // ── штатні поля Motorola (impres_bms.h) ────────────────────────────────
    //  Раніше тут стояло «Знос: рахує рація», а цикли оцінювались із CCA.
    //  Насправді і знос, і справжній лічильник циклів лежать у самому чипі:
    //  цикли — у гістограмі (без ключа), знос — у зашифрованому CTS.
    const ImpresBms &bms = impresBmsOf(hasDump ? batteryDump : nullptr,
                                       hasDump2438 ? batteryDump2438 : nullptr,
                                       hasSN2433 ? chipSN2433 : nullptr,
                                       DS2438_RSENSE_OHM);
    if (bms.ok && bms.cycles >= 0) {
        snprintf(buf, sizeof(buf), "Циклів: %d", bms.cycles);
        row(buf);
    }
    if (bms.ok && bms.haveKey) {
        snprintf(buf, sizeof(buf), "Знос: %d%% (%d мА*год)", bms.health, bms.potentialMah);
        row(buf);
    } else if (hasDump || hasDump2438) {
        row("Знос: нема ключа");
    }

    drawFooter();
}

// Загальна відмальовування сирого дампа (hex), шрифт 4x6. На вузьких екранах (Nokia)
// по 6 байт в рядку, інакше по 8; глибина — скільки вміщається по висоті.
inline void drawRawPage(const char *title, const uint8_t *data, bool has, int count) {
    drawHeader(title);
    if (!has) {
        u8g2.setFont(BODY_FONT);
        u8g2.drawUTF8(0, ROW(0), "немає даних (зчитайте)");
        return;
    }
    u8g2.setFont(u8g2_font_4x6_tr);
    char buf[36];
    const int perRow = (DISP_W >= 100) ? 8 : 6;
    int y = HEAD_LINE + 7;
    for (int off = 0; off < count; off += perRow) {
        int n = snprintf(buf, sizeof(buf), "%02X:", off);
        for (int c = 0; c < perRow && off + c < count; c++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02X ", data[off + c]);
        u8g2.drawStr(0, y, buf);
        y += 7;
        if (y > DISP_H) break;
    }
}

// На 128x128 вміщається вдвічі більше дампа DS2433.
#define RAW2433_COUNT ((DISP_H >= 128) ? 128 : 64)
inline void drawPageRaw2438() { drawRawPage("DS2438 дамп 0-63", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawPage((DISP_H >= 128) ? "DS2433 дамп 0-127" : "DS2433 дамп 0-63",
                                            batteryDump, hasDump, RAW2433_COUNT); }

// ⚑ Тут була numActions() = opCount(). Вона рахувала довжину КІЛЬЦЯ дій, якого
// більше немає: екран показує СПИСОК, а його довжину рахує menuCount() — і
// вона більша за opCount(), бо в списку є ще й переходи на сторінки.

// Сторінка «Дії»: показуємо ОДНУ обрану операцію крупно + опис + попередження.
// [<] коротко — наступна операція; [<] утримати (0.8с) — ВИКОНАТИ; [>] — вихід.
// Сторінка МОНІТОРИНГУ РОЗРЯДУ (монохром). Показується автоматично, поки
// навантаження увімкнене, і має пріоритет над гортанням меню.
// ═══════ СПІЛЬНИЙ КАРКАС ЕКРАНІВ ОПЕРАЦІЙ (заряд / розряд / пробудження) ══
//  Той самий каркас, що й на кольоровій панелі, лише рядків менше — на
//  128 пікселів їх влазить чотири. Порядок однаковий у всіх трьох режимах:
//      шапка з відсотком -> напруга й струм -> уставка/ШІМ -> ємність ->
//      час і СКІЛЬКИ ЩЕ ЛИШИЛОСЬ.
static int g_monLine = 0;

inline void opMonHead(const char *title, uint16_t mv, int16_t ma) {
    drawHeader(title);
    char b[48];
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    snprintf(b, sizeof(b), "%u.%02u В %d мА", mv / 1000, (mv % 1000) / 10, ma);
    u8g2.drawUTF8(0, HEAD_LINE + 13, b);
    const char *csrc; int pct = batteryPercent(&csrc);
    drawBatteryIcon(DISP_W - 27, HEAD_LINE + 4, 22, 10, pct);
    u8g2.setFont(BODY_FONT);
    g_monLine = 0;
}

inline void opMonRow(const char *txt) {
    int y = HEAD_LINE + 24 + g_monLine * 9;
    if (y > FOOT_HL - 2) return;          // під підвал не пишемо
    u8g2.drawUTF8(0, y, txt);
    g_monLine++;
}

// «Скільки ще лишилось» — компактно; 0 = оцінити не можна, і так і пишемо.
inline void opMonEtaText(uint32_t etaS, char *out, size_t n) {
    if (!etaS)       { snprintf(out, n, "-"); return; }
    if (etaS < 60)   { snprintf(out, n, "<1хв"); return; }
    if (etaS < 3600) { snprintf(out, n, "%luхв", (unsigned long)(etaS / 60)); return; }
    snprintf(out, n, "%luг%02luхв", (unsigned long)(etaS / 3600),
             (unsigned long)((etaS % 3600) / 60));
}

inline void opMonTime(uint32_t elapsedS, uint32_t etaS) {
    char b[48], e[16];
    opMonEtaText(etaS, e, sizeof(e));
    unsigned long el = elapsedS;
    snprintf(b, sizeof(b), "%lu:%02lu:%02lu ще %s%s",
             el / 3600, (el / 60) % 60, el % 60, etaS ? "~" : "", e);
    opMonRow(b);
}

inline void opMonFoot(bool running, const char *text) {
    u8g2.drawHLine(0, FOOT_HL, DISP_W);
    u8g2.drawUTF8(0, FOOT_Y, running ? "трим=ЗУПИНИТИ" : text);
}

inline void drawPageDischarge() {
    const DischargeState &d = g_dis;
    char b[48];
    snprintf(b, sizeof(b), "РОЗРЯД %d%%",
             (d.startMv > d.targetMv)
                 ? (int)(((long)d.startMv - d.lastMv) * 100 / ((long)d.startMv - d.targetMv))
                 : 0);
    opMonHead(b, d.lastMv, d.lastMa);

    // Уставка й шпаруватість: розряд іде не «скільки дасть резистор», а на
    // заданому струмі. Поруч — режим: без нього не видно, звідки взялась
    // уставка (лінійка за напругою, 0.2C від ємності чи ручне число).
    if (dischargePwmOk())
        snprintf(b, sizeof(b), "уст%uмА ШІМ%u%% %s", d.setMa, d.dutyPct,
                 dischargeManualMa() ? "руч" : dischargeProfileShort(dischargeProfile()));
    else
        snprintf(b, sizeof(b), "БЕЗ ШІМ! пік%u", d.peakMa);
    opMonRow(b);

    snprintf(b, sizeof(b), "%lu мА·год (DCA %lu)",
             (unsigned long)dischargeMah(), (unsigned long)dischargeDcaMah());
    opMonRow(b);

    snprintf(b, sizeof(b), "ICA %u  %d.%dC  %s", d.lastIca,
             d.lastTempC10 / 10, abs(d.lastTempC10 % 10),
             dischargePhaseShort(d.phase));
    opMonRow(b);

    opMonTime(d.elapsedS,
              dischargeEtaS(impresPercentFromMv(d.lastMv),
                            impresPercentFromMv(d.targetMv),
                            dischargeRatedMah(),
                            (uint16_t)(d.lastMa < 0 ? -d.lastMa : d.lastMa)));

    opMonFoot(d.state == DIS_RUN,
              d.state == DIS_DONE ? "ГОТОВО -> на ЗП" : dischargeReasonText(d.reason));
}

// ⚑ ПРОБУДЖЕННЯ — ОКРЕМА СТОРІНКА, А НЕ ГІЛКИ ВСЕРЕДИНІ ЗАРЯДУ. Відсоток, ICA,
//  CCA й температура беруться з DS2438, а він мовчить — у цьому вся суть
//  режиму, і показувати замість них нулі означало б видавати відсутність
//  даних за дані. Каркас спільний, тож виглядає воно однаково із зарядом.
inline void drawPageWake() {
    const ChargeState &c = g_chg;
    char b[48];
    snprintf(b, sizeof(b), "ПРОБУДЖ. %lus", (unsigned long)c.elapsedS);
    opMonHead(b, c.lastMv, c.lastMa);

    if (chargePwmOk())
        snprintf(b, sizeof(b), "трим%u.%02uВ ШІМ%u%%", c.targetMv / 1000,
                 (c.targetMv % 1000) / 10, chargeDutyPct());
    else
        snprintf(b, sizeof(b), "БЕЗ КЕРУВАННЯ!");
    opMonRow(b);

    snprintf(b, sizeof(b), "%lu з %u мА·год  %uмА",
             (unsigned long)chargeMah(), (unsigned)CHARGE_WAKE_MAH_MAX,
             (unsigned)CHARGE_WAKE_MA);
    opMonRow(b);

    snprintf(b, sizeof(b), "проб %u  %s", c.wakeProbes,
             chargeWakeGoalShort(c.wakeGoal, c.reason));
    opMonRow(b);

    // Залишок ТОЧНИЙ: режим обмежений часом жорстко.
    uint32_t left = (c.elapsedS < (uint32_t)CHARGE_WAKE_MAX_S)
                  ? ((uint32_t)CHARGE_WAKE_MAX_S - c.elapsedS) : 0;
    opMonTime(c.elapsedS, (c.state == CHG_RUN) ? left : 0);

    opMonFoot(c.state == CHG_RUN,
              c.state == CHG_DONE ? "ГОТОВО" : chargeReasonText(c.reason));
}

// Сторінка МОНІТОРИНГУ ЗАРЯДУ (монохром) — той самий каркас, що й розряд.
inline void drawPageCharge() {
    if (chargeWakeShown()) { drawPageWake(); return; }
    const ChargeState &c = g_chg;
    char b[48];
    snprintf(b, sizeof(b), "ЗАРЯД %d%%", c.lastPct);
    opMonHead(b, c.lastMv, c.lastMa);

    if (!chargePwmOk())
        snprintf(b, sizeof(b), "БЕЗ КЕРУВАННЯ!");
    else
        snprintf(b, sizeof(b), "уст%uмА ШІМ%u%% %s", c.setMa, chargeDutyPct(),
                 chargeManualMa() ? "руч" : chargeProfileShort(chargeProfile()));
    opMonRow(b);

    // Якщо несправне ЖИВЛЕННЯ — на цьому рядку саме воно: без блока заряд не
    // піде взагалі, і показувати натомість мА·год означало б ховати причину.
    if (chargePsuFault())
        snprintf(b, sizeof(b), "БЖ %u.%02u В — %s", chargePsuMv() / 1000,
                 (chargePsuMv() % 1000) / 10,
                 chargePsuState() == PSU_ABSENT ? "НЕМАЄ" :
                 chargePsuState() == PSU_LOW    ? "ЗАНИЖЕНО" : "ЗАВИЩЕНО");
    else
        snprintf(b, sizeof(b), "%lu мА·год (CCA %lu)",
                 (unsigned long)chargeMah(), (unsigned long)chargeCcaMah());
    opMonRow(b);

    snprintf(b, sizeof(b), "ICA %u  %d.%dC  %s", c.lastIca,
             c.lastTempC10 / 10, abs(c.lastTempC10 % 10),
             chargePhaseShort(c.phase));
    opMonRow(b);

    opMonTime(c.elapsedS,
              chargeEtaS(c.phase, c.lastPct, c.targetPct, chargeRatedMah(), c.lastMa));

    opMonFoot(c.state == CHG_RUN,
              c.state == CHG_DONE ? "ГОТОВО" : chargeReasonText(c.reason));
}

// ── СТОРІНКА ПОМИЛКИ ЖИВЛЕННЯ (монохромна) ────────────────────────────────
//  ⚠️ ТУТ БУЛА ТА САМА ПОМИЛКА, ЩО Й НА КОЛЬОРОВОМУ ЕКРАНІ: блимав САМ ТЕКСТ,
//  тобто півперіоду на екрані не було жодного слова про причину. Аварійне
//  повідомлення, яке пів часу нічого не повідомляє, гірше за статичне.
//  Правильно навпаки: блимає ПЛАШКА (заливка інвертується), а текст усередині
//  стоїть нерухомо й читається в обидві фази — у прямій як чорний на білому,
//  у зворотній як білий на чорному.
static bool g_psuBlinkOn = true;

inline void drawPagePsuFault() {
    uint8_t st = chargePsuState();
    uint16_t mv = chargePsuMv();
    char b[40];

    drawHeader("ЖИВЛЕННЯ");

    // Плашка на всю ширину: у прямій фазі — залита, у зворотній — лише рамка.
    const int py = HEAD_LINE + 2, ph = 24;
    if (g_psuBlinkOn) u8g2.drawBox(0, py, DISP_W, ph);
    else              u8g2.drawFrame(0, py, DISP_W, ph);
    // Текст усередині плашки — інверсний до її заливки, тож видимий завжди.
    u8g2.setDrawColor(g_psuBlinkOn ? 0 : 1);

    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    const char *big = (st == PSU_ABSENT) ? "НЕМАЄ ЖИВЛЕННЯ"
                    : (st == PSU_LOW)    ? "НАПРУГА ЗАНИЖЕНА"
                                         : "НАПРУГА ЗАВИЩЕНА";
    u8g2.drawUTF8((DISP_W - u8g2.getUTF8Width(big)) / 2, py + 11, big);

    u8g2.setFont(BODY_FONT);
    const char *sub = (st == PSU_ABSENT) ? "блок не під'єднано"
                    : (st == PSU_LOW)    ? "блок просів або не той"
                                         : "блок не той (19 В?)";
    u8g2.drawUTF8((DISP_W - u8g2.getUTF8Width(sub)) / 2, py + 21, sub);

    u8g2.setDrawColor(1);                     // назад у звичайний режим

    // ⚑ Головне число — НОМІНАЛ («треба 14 В»): допуск пояснює, ЧОМУ блок
    // відхилено, а користувачеві потрібна відповідь на «який тоді треба».
    // Тому номінал у першому рядку поруч із виміром, допуск — окремо нижче.
    char nom[8];
    snprintf(b, sizeof(b), "є %u.%02u В, треба %s В",
             mv / 1000, (mv % 1000) / 10,
             chargeMvShort(CHARGE_SUPPLY_MV, nom, sizeof(nom)));
    u8g2.drawUTF8(0, py + ph + 11, b);
    snprintf(b, sizeof(b), "допуск %u.%u-%u.%u В  ЗАРЯД НІ",
             CHARGE_PSU_MIN_MV / 1000, (CHARGE_PSU_MIN_MV % 1000) / 100,
             CHARGE_PSU_MAX_MV / 1000, (CHARGE_PSU_MAX_MV % 1000) / 100);
    u8g2.drawUTF8(0, py + ph + 20, b);

    u8g2.drawHLine(0, FOOT_HL, DISP_W);
    u8g2.drawUTF8(0, FOOT_Y, "кнопка — сховати");
}

// ── МЕНЮ: ТОЙ САМИЙ СПИСОК, ЩО Й НА КОЛЬОРОВІЙ ПАНЕЛІ ─────────────────────
//  Склад, порядок і групи — з operations.h, спільні з кольоровим екраном,
//  вебом і USB-клієнтом. Різниця лише в тому, скільки рядків влазить: на
//  84x48 їх три й опису немає взагалі, на 128x128 — п'ять і два рядки опису.
//  Саме тому геометрія рахується, а не зашита числами.
#define MENU_GLYPH_W (DISP_H >= 128 ? 6 : 5)

inline void drawPageMenu() {
    int total = menuCount();
    if (total <= 0) return;
    if (g_menuSel < 0) g_menuSel = 0;
    if (g_menuSel >= total) g_menuSel = total - 1;

    char nbuf[OP_NAME_BUF]; const char *name, *l1, *l2; uint8_t danger, chips;
    menuInfo(g_menuSel, &name, &l1, &l2, &danger, &chips, nbuf, sizeof(nbuf));
    uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
    menuRow(g_menuSel, &kind, &code, &group);

    char cnt[12]; snprintf(cnt, sizeof(cnt), "%d/%d", g_menuSel + 1, total);
    const char *gname = menuGroupName(group);
    char gfit[24];
    // Назва групи теж може не влізти поруч із лічильником — обрізаємо чесно.
    txtFit(gfit, sizeof(gfit), (gname && *gname) ? gname : "МЕНЮ",
           (DISP_W - (int)strlen(cnt) * MENU_GLYPH_W - 4) / MENU_GLYPH_W);
    drawHeader(gfit, cnt);

    const int descLines = (DISP_H >= 128) ? 2 : (DISP_H >= 64 ? 1 : 0);
    int listBot = FOOT_HL - descLines * ROW_H;
    int rows = (listBot - HEAD_LINE - 2) / ROW_H;
    if (rows < 2) rows = 2;
    if (rows > total) rows = total;

    int first = g_menuSel - rows / 2;
    if (first > total - rows) first = total - rows;
    if (first < 0) first = 0;

    int maxG = DISP_W / MENU_GLYPH_W - 2;      // 2 гліфи під позначку небезпеки
    u8g2.setFont(BODY_FONT);
    for (int r = 0; r < rows; r++) {
        int i = first + r;
        int y = HEAD_LINE + 2 + (r + 1) * ROW_H - 2;      // базова лінія рядка
        const char *n2, *a, *b; uint8_t d2, c2; char b2[OP_NAME_BUF];
        menuInfo(i, &n2, &a, &b, &d2, &c2, b2, sizeof(b2));
        char fit[OP_NAME_BUF + 4], line[OP_NAME_BUF + 8];
        txtFit(fit, sizeof(fit), n2, maxG);
        // Небезпечне видно ще В СПИСКУ, а не лише коли на нього наведешся:
        // «!» перед назвою — єдина позначка, яка є на монохромній панелі.
        snprintf(line, sizeof(line), "%s%s", (d2 == OPD_WIPE) ? "! " : "  ", fit);
        if (i == g_menuSel) {
            // Курсор — інверсією рядка: на монохромі це єдиний спосіб
            // показати вибір, який видно з відстані.
            u8g2.drawBox(0, y - ROW_H + 2, DISP_W, ROW_H);
            u8g2.setDrawColor(0);
            u8g2.drawUTF8(0, y, line);
            u8g2.setDrawColor(1);
        } else {
            u8g2.drawUTF8(0, y, line);
        }
    }

    if (descLines > 0) {
        int dy = listBot + ROW_H - 2;
        char fit[40];
        // Перший рядок опису — про НАСЛІДОК: що зробить натискання й куди
        // пише. На вузькій панелі це важливіше за розгорнуте пояснення.
        char tail[40];
        if (danger == OPD_WIPE)      snprintf(tail, sizeof(tail), "НЕЗВОРОТНЬО! трим.OK");
        else if (kind == MI_PAGE)    snprintf(tail, sizeof(tail), "OK - відкрити");
        else if (danger == OPD_SAFE) snprintf(tail, sizeof(tail), "OK - перемкнути");
        else                         snprintf(tail, sizeof(tail), "трим.OK=ПУСК %s",
                                              opChipsShort(chips));
        txtFit(fit, sizeof(fit), tail, DISP_W / MENU_GLYPH_W);
        u8g2.drawUTF8(0, dy, fit);
        if (descLines > 1) {
            txtFit(fit, sizeof(fit), l1, DISP_W / MENU_GLYPH_W);
            u8g2.drawUTF8(0, dy + ROW_H, fit);
        }
    }
    drawFooter();
}

// Сторінка Майстра відновлення: аналіз стану -> проблеми -> наступний крок.
// Дані готує wizDeviceRefresh() (recovery.h); тут лише малюємо.
inline void drawPageWizard() {
    drawHeader("Майстер");
    u8g2.setFont(BODY_FONT);
    int y = HEAD_LINE + 12;
    if (g_wizBusy) {
        u8g2.drawUTF8(0, y, "Виконую...");
    } else if (g_wizProblems < 0) {
        u8g2.drawUTF8(0, y,      "Аналіз стану АКБ.");
#ifdef MENU_BTN3_PIN
        u8g2.drawUTF8(0, y + 11, "[OK] аналіз");
#else
        u8g2.drawUTF8(0, y + 11, "[<] коротко = аналіз");
#endif
    } else if (g_wizHealthy) {
        u8g2.setFont(u8g2_font_6x12_t_cyrillic);
        u8g2.drawUTF8(0, y + 2,  "OK: справна");
        u8g2.setFont(BODY_FONT);
        u8g2.drawUTF8(0, y + 16, "Відновлення не треба");
    } else {
        char b[28]; snprintf(b, sizeof(b), "Проблем: %d", g_wizProblems);
        u8g2.drawUTF8(0, y, b);
        u8g2.drawUTF8(0, y + 10, g_wizTop);
        if (g_wizAwait) {
            u8g2.drawUTF8(0, y + 22, "Чекаю ЗП. Поверніть");
#ifdef MENU_BTN3_PIN
            u8g2.drawUTF8(0, y + 32, "АКБ, [OK] трим.");
#else
            u8g2.drawUTF8(0, y + 32, "АКБ, [<] трим.");
#endif
        } else {
            char n[44]; snprintf(n, sizeof(n), "Далі: %s", g_wizNext);
            u8g2.drawUTF8(0, y + 22, n);
            char p[20]; snprintf(p, sizeof(p), "Крок %d/%d", g_wizProg + 1, g_wizTotal);
            u8g2.drawUTF8(0, y + 32, p);
        }
    }
    u8g2.drawHLine(0, FOOT_HL, DISP_W);
#ifdef MENU_BTN3_PIN
    u8g2.drawUTF8(0, FOOT_Y, "[OK]аналіз трим=крок");
#else
    u8g2.drawUTF8(0, FOOT_Y, "[<]кор=аналіз трим=крок");
#endif
}

// ---------- рендер і кнопка ----------

// Оновлення екрана під час РОЗРЯДУ. У монохромі кадр завжди збирається в буфер
// цілком, тож окремого «легкого» режиму не треба — параметр лише для сумісності
// з кольоровою реалізацією.
inline void displayRender();
inline void displayDischargeRefresh(bool /*full*/) { displayRender(); }
inline void displayChargeRefresh(bool /*full*/) { displayRender(); }

// ── БЛИМАННЯ НАПИСУ ПРО ЖИВЛЕННЯ ──────────────────────────────────────────
//  Кличеться часто з loop(). Тут перемальовується ВЕСЬ буфер — на монохромному
//  екрані це дешево (одна посилка кадру), на відміну від кольорового, де
//  блимає лише смуга напису.
// Зняти повноекранне повідомлення, коли його час вийшов.
inline void displayFlashTask() {
    if (comboFlashExpired(g_flash, millis())) displayRender();
}

inline void displayPsuBlinkTask() {
    static unsigned long lastMs = 0;
    if (!chargePsuScreenActive()) { g_psuBlinkOn = true; return; }
    unsigned long now = millis();
    if (now - lastMs < DISPLAY_PSU_BLINK_MS) return;
    lastMs = now;
    g_psuBlinkOn = !g_psuBlinkOn;
    displayRender();
}

// Повноекранне повідомлення по комбінації: тримається COMBO_FLASH_MS і
// знімається саме або першою ж кнопкою.
inline void drawPageFlash() {
    const char *s = "Ляшко ЛОХ";
    u8g2.setFont(BODY_FONT);
    int w = u8g2.getUTF8Width(s);
    int x = (DISP_W - w) / 2; if (x < 0) x = 0;
    u8g2.drawUTF8(x, DISP_H / 2 + 4, s);
}

inline void displayRender() {
    u8g2.clearBuffer();
    if (comboFlashActive(g_flash, millis())) {
        drawPageFlash();
        u8g2.sendBuffer();
        return;
    }
    // Поки навантаження/заряд увімкнені — примусово моніторинг, хоч би яку
    // сторінку було обрано: довга операція із запобіжниками має бути видима.
    // Заряд і розряд не можуть іти одночасно (взаємно перевіряють одне одного
    // при старті), тож порядок цих двох перевірок не має значення.
    if (dischargeScreenActive()) {
        drawPageDischarge();
        u8g2.sendBuffer();
        return;
    }
    if (chargeScreenActive()) {
        drawPageCharge();
        u8g2.sendBuffer();
        return;
    }
    // Помилка живлення — НИЖЧЕ за операції, що йдуть: розряд блока живлення не
    // потребує, а зупинений через живлення заряд і так показує ту саму причину.
    if (chargePsuScreenActive()) {
        drawPagePsuFault();
        u8g2.sendBuffer();
        return;
    }
    switch (g_displayPage) {
        case PAGE_MAIN:   drawPageMain();     break;
        case PAGE_MODEL:  drawPageModel();    break;
        case PAGE_TECH:   drawPageTech();     break;
        case PAGE_HEALTH: drawPageHealth();   break;
        case PAGE_MENU:   drawPageMenu();     break;
        case PAGE_RAW38:  drawPageRaw2438();  break;
        case PAGE_RAW33:  drawPageRaw2433();  break;
        case PAGE_WIZARD: drawPageWizard();   break;
        default:          drawPageMain();     break;
    }
    u8g2.sendBuffer();
}

// Тік анімації батареї (з loop() ~10 разів/с). ПУЛЬСАЦІЯ заповнення: край рівня
// заряду плавно «дихає» (±2 px). Оновлює ЛИШЕ область іконки батареї — дешево
// навіть для повільного SSD1327, решту екрана й шину не чіпає.
inline void displayAnimTick() {
    // Головна сторінка + сторінки РОЗРЯДУ/ЗАРЯДУ: там показник заряду такий
    // самий, і статична шкала під час довгої операції виглядала б як «завис».
    if (!(g_displayPage == 0 || dischargeScreenActive() || chargeScreenActive()) || g_battW == 0) return;
    const char *src; int pct = batteryPercent(&src);
    if (pct < 0) return;
    g_animPhase++;
    int p = g_animPhase & 15;                       // період 16 кадрів (~1.8 с)
    int tri = (p < 8) ? p : (16 - p);               // 0..8..0 (трикутна хвиля)
    int shrink = (8 - tri) / 3;                     // 0..2 px «дихання» краю

    u8g2.setDrawColor(0);
    u8g2.drawBox(g_battX, g_battY, g_battW + 4, g_battH);   // очистити область іконки
    u8g2.setDrawColor(1);
    drawBatteryIcon(g_battX, g_battY, g_battW, g_battH, pct);
    if (shrink > 0) {                               // прибрати кілька px біля краю заповнення
        int fillw = (g_battW - 4) * pct / 100;
        if (fillw > g_battW - 4) fillw = g_battW - 4;
        int edge = g_battX + 2 + fillw;
        u8g2.setDrawColor(0);
        for (int i = 1; i <= shrink; i++) if (edge - i >= g_battX + 2) u8g2.drawVLine(edge - i, g_battY + 2, g_battH - 4);
        u8g2.setDrawColor(1);
    }
    int tx = g_battX / 8, ty = g_battY / 8;
    int tw = (g_battX + g_battW + 4 + 7) / 8 - tx;
    int th = (g_battY + g_battH + 7) / 8 - ty;
    u8g2.updateDisplayArea(tx, ty, tw, th);
}

// Повертає запит Майстра для .ino один раз: 0 нема, 1 аналіз, 2 наступний крок.
inline int displayConsumeWizRequest() { int r = g_wizReq; g_wizReq = 0; return r; }

// Плавний перехід між сторінками меню: короткий «дип» яскравості (crossfade
// старий->новий вміст). Деградує коректно, якщо контраст не підтримується.
inline void displayFlip() {
    int lo = DISP_BRIGHT / 3;
    for (int c = DISP_BRIGHT; c > lo; c -= 24) u8g2.setContrast(c < 0 ? 0 : c), delay(5);
    displayRender();
    for (int c = lo; c < DISP_BRIGHT; c += 24) u8g2.setContrast(c), delay(5);
    u8g2.setContrast(DISP_BRIGHT);
    // ⚑ Звук — В КІНЦІ, після всієї блокуючої роботи. Фразу веде buzzTask() із
    // loop(); поки триває «дип» контрасту й displayRender(), loop() стоїть і
    // тік не приходить жодного разу — бліп, запущений ДО переходу, протікав
    // повз, і перший тік після повернення просто гасив вихід.
    buzzClick();
}

// Натискання «OK» на пункті меню — та сама логіка, що й на кольоровій панелі:
// що зробить кнопка, залежить від ПУНКТА (сторінка / безпечне / запис), а не
// від сторінки. Довге натискання лишається бар'єром рівно там, де є наслідок.
inline void menuActivate(bool longPress) {
    uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
    if (!menuRow(g_menuSel, &kind, &code, &group)) return;
    if (kind == MI_PAGE) {
        if (longPress) return;
        g_displayPage = menuPageToDisplayPage(code);
        displayFlip();
        return;
    }
    char nb[OP_NAME_BUF]; const char *n, *a, *b2; uint8_t d, c;
    opInfo(code, &n, &a, &b2, &d, nb, sizeof(nb), &c);
    if (d == OPD_SAFE) {
        if (longPress) return;
        g_actionRequested = code;
        return;
    }
    if (!longPress) { displayShow("тримайте OK = ПУСК"); return; }
    g_actionRequested = code;
    displayShow("ВИКОНУЮ...");
}

inline void displayButtonSetup() {
#ifdef MENU_BTN_ADC_PIN
    // Аналогова драбинка: підтяжка ЗОВНІШНЯ (резистор на платі), внутрішню
    // підтяжку ESP32 НЕ вмикаємо — вона спотворить розрахунок порогів.
    pinMode(MENU_BTN_ADC_PIN, INPUT);
#else
    pinMode(MENU_BTN_PIN, INPUT_PULLUP);
    pinMode(MENU_BTN2_PIN, INPUT_PULLUP);
  #ifdef MENU_BTN3_PIN
    pinMode(MENU_BTN3_PIN, INPUT_PULLUP);
  #endif
#endif
}

// Стан однієї кнопки для антидребезгу + розпізнавання довгого натискання.
struct BtnState {
    bool stable = HIGH;        // стійкий (антидребезг) рівень
    bool lastRaw = HIGH;
    unsigned long tChange = 0; // час останнього зміни сирого рівня
    unsigned long tPress = 0;  // час натискання
    bool longFired = false;
};

// Опитування кнопки за вже зчитаним сирим станом (true = натиснуто). Та сама
// логіка антидребезгу/довгого натискання, що й раніше — лише відокремлена
// від того, ЯК саме зчитується «натиснуто», щоб працювати і з окремими GPIO,
// і з аналоговою драбинкою (див. btn1Raw/btn2Raw/btn3Raw нижче).
// Повертає: 0 — нічого, 1 — коротке (по відпусканню), 2 — довге (при
// утриманні longMs). longMs=0 вимикає довге натискання.
inline int pollButtonRaw(bool pressed, BtnState &b, unsigned long longMs) {
    bool raw = pressed ? LOW : HIGH;   // та сама полярність, що й INPUT_PULLUP
    unsigned long now = millis();
    if (raw != b.lastRaw) { b.lastRaw = raw; b.tChange = now; }

    int ev = 0;
    if (now - b.tChange > 25 && raw != b.stable) {   // стійке зміна
        b.stable = raw;
        if (b.stable == LOW) {                        // натискання
            b.tPress = now;
            b.longFired = false;
        } else {                                      // відпускання
            if (!b.longFired) ev = 1;                 // коротке (якщо не було довгого)
        }
    }
    if (b.stable == LOW && longMs && !b.longFired && now - b.tPress >= longMs) {
        b.longFired = true;
        ev = 2;                                        // довге (один раз)
    }
    return ev;
}

#ifdef MENU_BTN_ADC_PIN
// Три кнопки на одному ADC-піні (див. settings.h: MENU_BTN_ADC_*). Один
// зчит на весь прохід displayHandleButton() — не по разу на кожну логічну
// кнопку: три окремі зчитування могли б потрапити на різні миттєві значення
// шуму й дати суперечливий («і праворуч, і ввід одночасно») результат біля
// порогу, хай навіть це малоймовірно з таким запасом (500+ мВ).
//
// ⚠️ НЕ analogReadMilliVolts(): вона апаратно калібрується через eFuse
// (Two-Point/Vref), а на частині безіменних/клон-плат ця калібровка не
// прошита — ESP-IDF тоді валить у Serial «adc_cali: ... default vref
// didn't set» НА КОЖНОМУ виклику (тобто на кожен прохід loop()) і повертає
// невалідні мВ. Це саме й давало «кнопки не працюють» і самовільне
// перемикання сторінок при старті. analogRead() калібрування не потребує
// взагалі — рахуємо мВ лінійно самі (0..4095 -> 0..3300); похибка від
// нелінійності ADC — десятки мВ, а між порогами — сотні мВ запасу.
static bool g_btnAdcEnter = false, g_btnAdcLeft = false, g_btnAdcRight = false;
inline void btnAdcRefresh() {
    int mv = analogRead(MENU_BTN_ADC_PIN) * 3300 / 4095;
    bool newEnter = mv < MENU_BTN_ADC_TH_ENTER;
    bool newLeft  = !newEnter && mv < MENU_BTN_ADC_TH_LEFT;
    bool newRight = !newEnter && !newLeft && mv < MENU_BTN_ADC_TH_RIGHT;
    // Друк ЛИШЕ на зміну класифікації (не щоцикл loop()) — щоб побачити, яку
    // РЕАЛЬНУ напругу дає кожна фізична кнопка, і порівняти з порогами.
    // Діагностика для скарг «кнопка Х спрацьовує як Y» — це майже завжди
    // невідповідність РЕАЛЬНОГО номіналу резистора кнопки закодованому
    // MENU_BTN_ADC_LEFT_OHM/RIGHT_OHM у settings.h, а не баг прошивки.
    static int8_t lastCls = -1;   // -1=ще не друкували, 0=none, 1=enter, 2=left, 3=right
    int8_t cls = newEnter ? 1 : newLeft ? 2 : newRight ? 3 : 0;
    if (cls != lastCls) {
        lastCls = cls;
        Serial.printf("BTN_ADC: %d мВ -> %s (пороги: ENTER<%d, LEFT<%d, RIGHT<%d, none>=%d)\n",
                      mv, cls == 1 ? "ENTER" : cls == 2 ? "LEFT" : cls == 3 ? "RIGHT" : "відпущено",
                      (int)MENU_BTN_ADC_TH_ENTER, (int)MENU_BTN_ADC_TH_LEFT,
                      (int)MENU_BTN_ADC_TH_RIGHT, (int)MENU_BTN_ADC_TH_RIGHT);
    }
    g_btnAdcEnter = newEnter;
    g_btnAdcLeft  = newLeft;
    g_btnAdcRight = newRight;
}
// BTN1="Вперед"->Вправо, BTN2="Назад"->Вліво, BTN3="OK/Дія"->Ввід — той самий
// розподіл ролей, що й був на трьох окремих GPIO.
inline bool btn1Raw() { return g_btnAdcRight; }
inline bool btn2Raw() { return g_btnAdcLeft; }
inline bool btn3Raw() { return g_btnAdcEnter; }
#else
inline bool btn1Raw() { return digitalRead(MENU_BTN_PIN) == LOW; }
inline bool btn2Raw() { return digitalRead(MENU_BTN2_PIN) == LOW; }
  #ifdef MENU_BTN3_PIN
inline bool btn3Raw() { return digitalRead(MENU_BTN3_PIN) == LOW; }
  #endif
#endif

// true один раз після того, як кнопка провернула меню на повний коло.
inline bool displayConsumeReadRequest() {
    if (g_readRequested) { g_readRequested = false; return true; }
    return false;
}

// Повертає обрану дію (0=Скидання 1=Ремонт 2=Очистка) один раз після
// підтвердження в меню, інакше -1.
inline int displayConsumeActionRequest() {
    int a = g_actionRequested; g_actionRequested = -1; return a;
}

// Опитування кнопок.
//  BTN1: коротке — наступна сторінка; довге (0.8с) — повторне читання АКБ.
//  BTN2: на сторінці «Дії» — коротке = вибір операції, довге (0.8с) = ВИКОНАТИ;
//        на інших сторінках — коротке = попередня сторінка.
// Скільки триває «довге» натискання. Одне число на всі гілки; саме воно живе
// в combo.h разом із порогами жестів — усі три є сходинками однієї шкали.
#define BTN_LONG_MS COMBO_LONG_MS

// ── ПРИХОВАНІ ЖЕСТИ (те саме правило, що й на кольоровій панелі) ───────────
#ifdef MENU_BTN3_PIN
  #define HOLD_BTN_ST    b3
#else
  #define HOLD_BTN_ST    b2
#endif

// Що зробить довге натискання на поточному екрані; nullptr — нічого, і тоді
// обіцяти щось було б гірше за мовчання.
inline const char *holdLongHint() {
    if (g_displayPage == PAGE_MENU) {
        uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
        if (!menuRow(g_menuSel, &kind, &code, &group)) return nullptr;
        if (kind == MI_PAGE) return nullptr;
        char nb[OP_NAME_BUF]; const char *n, *a, *b2; uint8_t d, c;
        opInfo(code, &n, &a, &b2, &d, nb, sizeof(nb), &c);
        return (d == OPD_SAFE) ? nullptr : "відпустіть = ПУСК";
    }
    if (g_displayPage == PAGE_WIZARD)     return "відпустіть = ВИКОНАТИ";
    if (g_displayPage < NUM_STATUS_PAGES) return "відпустіть = МЕНЮ";
    return nullptr;
}

inline void displayToggleChargeMode() {
    bool off = chargeSetOffByUser(!chargeOffByUser());
    g_menuSel = menuClampSel(g_menuSel);
    displaySetStatus(off ? "ЗАРЯД ВИМКНЕНО" : "ЗАРЯД УВІМКНЕНО");
    displayRender();
    if (off) buzzErr(); else buzzOk();
}

inline void displayHandleButton() {
    static BtnState b1, b2, b3;
#ifdef MENU_BTN_ADC_PIN
    btnAdcRefresh();   // один аналоговий зчит на весь прохід нижче
#endif

    // ⚑ ОПИТУЄМО КНОПКИ ОДИН РАЗ НА ВСІ ГІЛКИ — див. пояснення в
    //  display_color.h: жест — це ТРИВАЛІСТЬ, і виміряти її чотирма
    //  незалежними детекторами (розряд / заряд / помилка живлення / меню)
    //  неможливо — перехід між сторінками починав би відлік заново.
    // Під час операції утримання належить АВАРІЙНІЙ ЗУПИНЦІ — вона мусить
    // спрацювати на порозі, а не на відпусканні. Тому там жестів немає.
    const bool holdLive = !dischargeScreenActive() && !chargeScreenActive();
    const unsigned long holdLongMs = holdLive ? 0 : BTN_LONG_MS;

    int e1 = pollButtonRaw(btn1Raw(), b1, BTN_LONG_MS);     // «›» далі
#ifdef MENU_BTN3_PIN
    int e2 = pollButtonRaw(btn2Raw(), b2, BTN_LONG_MS);     // «‹» назад
    int e3 = pollButtonRaw(btn3Raw(), b3, holdLongMs);      // «OK» — на ньому жести
#else
    int e2 = pollButtonRaw(btn2Raw(), b2, holdLongMs);      // «‹» — на ньому жести
    int e3 = 0;
#endif

    // «Довге» на цій кнопці вирішується НА ВІДПУСКАННІ (тому їй і передано
    // holdLongMs = 0): інакше дорога до вимикача заряду проходила б через уже
    // виконану операцію — на 0.8 с скидання, і аж потім, на 5 с, вимикач.
    uint8_t hev = CHOLD_NONE;
    if (holdLive) {
        hev = comboHoldFeed(g_hold, millis(), HOLD_BTN_ST.stable == LOW);
        int he = (hev == CHOLD_SHORT) ? 1 : (hev == CHOLD_LONG) ? 2 : 0;
#ifdef MENU_BTN3_PIN
        e3 = he;
#else
        e2 = he;
#endif
    } else {
        comboHoldReset(g_hold);
    }

    if (displayFlashActive()) {
        if (e1 || e2 || e3 || hev != CHOLD_NONE) { g_flash.until = 0; displayRender(); }
        return;
    }

    if (hev == CHOLD_CHARGE) { displayToggleChargeMode(); return; }
    if (hev == CHOLD_ARM_CHARGE) {
        displayShow(chargeOffByUser() ? "відпустіть = ЗАРЯД УВІМК"
                                      : "відпустіть = ЗАРЯД ВИМК");
        return;
    }
    if (hev == CHOLD_ARM_LONG) {
        const char *h = holdLongHint();
        if (h) displayShow(h);
        return;
    }

    // ── ПРИХОВАНИЙ ЖЕСТ: ПОСЛІДОВНІСТЬ КЛАВІШ ─────────────────────────────
    //  ⚑ ЖИВЕ ПОРУЧ ІЗ ЗВИЧАЙНОЮ НАВІГАЦІЄЮ, А НЕ ЗАМІСТЬ НЕЇ. Проміжні
    //  клавіші жесту роблять те, що робили завжди (гортають сторінки, водять
    //  курсор) — інакше набір було б видно з екрана, і жест перестав би бути
    //  прихованим. А от ОСТАННЮ клавішу, ту, якою жест завершено, звичайна
    //  обробка вже не бачить: інакше разом із жестом спрацювало б іще й
    //  «зчитати» або пункт меню під курсором.
    //
    //  ⚑ ДОВГЕ НАТИСКАННЯ РВЕ ЛАНЦЮЖОК. У жесті беруть участь лише короткі:
    //  довге — це вже інша команда (меню, стрибок групою, ПУСК), і зараховувати
    //  її в ланцюжок означало б рахувати як клавішу те, чим людина щойно
    //  зробила щось інше.
    if (holdLive) {
        comboSeqTick(g_seq, millis());
        if (e1 == 2 || e2 == 2 || e3 == 2) comboSeqReset(g_seq);
        uint8_t ck = (e1 == 1) ? CKEY_RIGHT : (e2 == 1) ? CKEY_LEFT
                   : (e3 == 1) ? CKEY_ENTER : CKEY_NONE;
        if (comboSeqFeed(g_seq, millis(), ck)) {
            comboFlashArm(g_flash, millis(), COMBO_FLASH_MS);
            displayRender();
            return;
        }
    } else {
        comboSeqReset(g_seq);
    }


    // ── РЕЖИМ РОЗРЯДУ ──────────────────────────────────────────────────────
    // Поки навантаження увімкнене, кнопки НЕ гортають меню: на екрані
    // моніторинг, а зміна сторінки «у фоні» лише збиває з пантелику.
    //   коротке натискання — оновити показання; довге — АВАРІЙНА ЗУПИНКА.
    if (dischargeScreenActive()) {
        int d1 = e1, d2 = e2, d3 = e3;
        if (!dischargeRunning()) {
            // Показано ПІДСУМОК завершеного розряду: будь-яке натискання прибирає
            // його й повертає звичайне меню.
            if (d1 || d2 || d3) { dischargeDismiss(); displayRender(); }
            return;
        }
        if (d1 == 2 || d2 == 2 || d3 == 2) {          // довге = АВАРІЙНА ЗУПИНКА
            dischargeStop(DISR_USER); displaySetStatus("РОЗРЯД СТОП"); displayRender();
        } else if (d1 == 1 || d2 == 1 || d3 == 1) {   // коротке = оновити показання
            displayDischargeRefresh(false);
        }
        return;
    }

    // ── РЕЖИМ ЗАРЯДУ — той самий принцип, що й розряд вище ─────────────────
    if (chargeScreenActive()) {
        int d1 = e1, d2 = e2, d3 = e3;
        if (!chargeRunning()) {
            if (d1 || d2 || d3) { chargeDismiss(); displayRender(); }
            return;
        }
        if (d1 == 2 || d2 == 2 || d3 == 2) {          // довге = АВАРІЙНА ЗУПИНКА
            chargeStop(CHGR_USER); displaySetStatus("ЗАРЯД СТОП"); displayRender();
        } else if (d1 == 1 || d2 == 1 || d3 == 1) {   // коротке = оновити показання
            displayChargeRefresh(false);
        }
        return;
    }

    // ── СТОРІНКА ПОМИЛКИ ЖИВЛЕННЯ ─────────────────────────────────────────
    // Будь-яке натискання прибирає її. Сама несправність лишається: «!» у
    // шапці, код на світлодіоді, і при зміні стану сторінка з'явиться знову.
    if (chargePsuScreenActive()) {
        if (e1 || e2 || e3) { chargePsuDismiss(); displayRender(); }
        return;
    }

    // ── ОДНЕ ПРАВИЛО НА ВСІ ЕКРАНИ (те саме, що на кольоровій панелі) ──────
    //  «‹»/«›» — рух: сторінка в кільці показань або пункт у меню; довге —
    //  стрибок (додому / на сусідню групу). «OK» — увійти чи виконати.
    //  Раніше «OK» означав п'ять різних речей залежно від сторінки, а на
    //  сторінці «Дії» взагалі не був вибором — він гортав список уперед.
#ifdef MENU_BTN3_PIN
    if (g_displayPage == PAGE_MENU) {
        int total = menuCount();
        if      (e1 == 1) { g_menuSel = (g_menuSel + 1) % total;         displayRender(); }
        else if (e1 == 2) { g_menuSel = menuNextGroup(g_menuSel);        displayRender(); }
        else if (e2 == 1) { g_menuSel = (g_menuSel - 1 + total) % total; displayRender(); }
        else if (e2 == 2) { g_menuSel = menuPrevGroup(g_menuSel);        displayRender(); }
        else if (e3)      { menuActivate(e3 == 2); }
        return;
    }
    if (g_displayPage >= NUM_STATUS_PAGES) {
        if      (e2 == 1) { g_displayPage = PAGE_MENU; displayFlip(); }
        else if (e2 == 2) { g_displayPage = PAGE_MAIN; displayFlip(); }
        else if (e1 == 1 && (g_displayPage == PAGE_RAW38 || g_displayPage == PAGE_RAW33)) {
            g_displayPage = (g_displayPage == PAGE_RAW38) ? PAGE_RAW33 : PAGE_RAW38;
            displayFlip();
        } else if (g_displayPage == PAGE_WIZARD) {
            if      (e3 == 1) { g_wizReq = 1; g_wizBusy = true; displaySetStatus("АНАЛІЗ..."); displayRender(); }
            else if (e3 == 2) { g_wizReq = 2; g_wizBusy = true; displaySetStatus("ВИКОНУЮ..."); displayRender(); }
        }
        return;
    }
    if      (e1 == 1) { g_displayPage = (g_displayPage + 1) % NUM_STATUS_PAGES; displayFlip(); }
    else if (e2 == 1) { g_displayPage = (g_displayPage - 1 + NUM_STATUS_PAGES) % NUM_STATUS_PAGES; displayFlip(); }
    else if (e2 == 2) { g_displayPage = PAGE_MAIN; displayFlip(); }
    else if (e3 == 1) { g_readRequested = true; displaySetStatus("ЗЧИТУВАННЯ..."); displayRender(); }
    else if (e3 == 2) { g_displayPage = PAGE_MENU; displayFlip(); }
#else
    // ДВІ кнопки: «›» — рух, «‹» — вибір/виконання.
    if (g_displayPage == PAGE_MENU) {
        int total = menuCount();
        if      (e1 == 1) { g_menuSel = (g_menuSel + 1) % total;  displayRender(); }
        else if (e1 == 2) { g_menuSel = menuNextGroup(g_menuSel); displayRender(); }
        else if (e2)      { menuActivate(e2 == 2); }
        return;
    }
    if (g_displayPage >= NUM_STATUS_PAGES) {
        if      (e1 == 1 && (g_displayPage == PAGE_RAW38 || g_displayPage == PAGE_RAW33)) {
            g_displayPage = (g_displayPage == PAGE_RAW38) ? PAGE_RAW33 : PAGE_RAW38;
            displayFlip();
        }
        else if (e1 == 2) { g_displayPage = PAGE_MENU; displayFlip(); }
        else if (g_displayPage == PAGE_WIZARD) {
            if      (e2 == 1) { g_wizReq = 1; g_wizBusy = true; displaySetStatus("АНАЛІЗ..."); displayRender(); }
            else if (e2 == 2) { g_wizReq = 2; g_wizBusy = true; displaySetStatus("ВИКОНУЮ..."); displayRender(); }
        } else if (e2 == 1) { g_displayPage = PAGE_MENU; displayFlip(); }
        return;
    }
    if      (e1 == 1) { g_displayPage = (g_displayPage + 1) % NUM_STATUS_PAGES; displayFlip(); }
    else if (e2 == 1) { g_readRequested = true; displaySetStatus("ЗЧИТУВАННЯ..."); displayRender(); }
    else if (e2 == 2) { g_displayPage = PAGE_MENU; displayFlip(); }
#endif
}

#endif  // DISPLAY_ST7789_SPI (кольоровий) / u8g2 (монохромний)

#endif  // DISPLAY_H
