#ifndef DISPLAY_COLOR_H
#define DISPLAY_COLOR_H
// ===========================================================================
//  Кольоровий TFT-дисплей на контролері ST7789 (SPI):
//    • ST7789VW — 240x240
//    • ST7789V3 — 240x280
//  Реалізує ТОЙ САМИЙ публічний інтерфейс, що й монохромний display.h
//  (displayInit/Splash/Show/Render/HandleButton/Consume*), а також спільні
//  логічні функції (decodeModel/decodeCapacity/batteryGenuine/batteryPercent/
//  fixRecordChecksum), які використовує web_server.h.
//
//  Потрібні бібліотеки (Менеджер бібліотек Arduino):
//    - Adafruit GFX Library
//    - Adafruit ST7735 and ST7789 Library
//    - Adafruit BusIO
//    - U8g2_for_Adafruit_GFX   (дає кириличні шрифти u8g2 на кольоровому екрані)
//  Вибір і піни — у settings.h (блок «ДИСПЛЕЙ», варіант DISPLAY_ST7789_SPI).
// ===========================================================================

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "settings.h"
#include "impres_format.h"
#include "impres_bms.h"   // штатний декодер Motorola (цикли, знос, дати)
#include "battery_reader.h"
#include "templates.h"    // BATTERY_TEMPLATES/COUNT — дії «Новий АКБ»
#include "operations.h"   // ЄДИНИЙ каталог операцій (порядок/назви/небезпека)
#include "battbar.h"      // коли шкалу батареї треба перемальовувати, а коли ні
#include "combo.h"        // приховані жести (чиста логіка, спільна з display.h)
#include "textwrap.h"     // перенос по словах: текст не вилазить за плашку
#include "discharge.h"    // стан керованого розряду для сторінки моніторингу
#include "charge.h"       // стан керованого заряду для сторінки моніторингу

// Стан, яке відображаємо (визначене в .ino / web_server.h).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump[DUMP_SIZE];
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern uint8_t chipSN2438[8];
extern bool hasSN2438;
extern uint8_t chipSN2433[8];
extern bool hasSN2433;

// -------------------- Роздільність (будь-яка панель ST7789) --------------------
// Пресети для типових панелей АБО власний розмір DISPLAY_ST7789_W/H.
// PANEL_W/PANEL_H — рідні (портретні) розміри матриці; оффсети пам'яті
// (XOFF/YOFF) для панелей, де видима зона зсунута в RAM 240x320 контролера.
#if   defined(DISPLAY_ST7789_240X320)      // 2.0"/2.4" 240x320
  #define PANEL_W 240
  #define PANEL_H 320
#elif defined(DISPLAY_ST7789_240X280)      // 1.69" ST7789V3 240x280
  #define PANEL_W 240
  #define PANEL_H 280
  #define PANEL_XOFF 0
  #define PANEL_YOFF 20
#elif defined(DISPLAY_ST7789_240X240)      // 1.3"/1.54" ST7789VW 240x240
  #define PANEL_W 240
  #define PANEL_H 240
#elif defined(DISPLAY_ST7789_135X240)      // 1.14" 135x240
  #define PANEL_W 135
  #define PANEL_H 240
  #define PANEL_XOFF 52
  #define PANEL_YOFF 40
#elif defined(DISPLAY_ST7789_170X320)      // 1.9" 170x320
  #define PANEL_W 170
  #define PANEL_H 320
  #define PANEL_XOFF 35
  #define PANEL_YOFF 0
#elif defined(DISPLAY_ST7789_172X320)      // 1.47" 172x320
  #define PANEL_W 172
  #define PANEL_H 320
  #define PANEL_XOFF 34
  #define PANEL_YOFF 0
#elif defined(DISPLAY_ST7789_W) && defined(DISPLAY_ST7789_H)   // власний розмір
  #define PANEL_W DISPLAY_ST7789_W
  #define PANEL_H DISPLAY_ST7789_H
#else                                       // за замовчуванням 240x240
  #define PANEL_W 240
  #define PANEL_H 240
#endif

// Ручне перевизначення оффсетів пам'яті (мають пріоритет над пресетом).
#if defined(DISPLAY_ST7789_XOFF)
  #undef  PANEL_XOFF
  #define PANEL_XOFF DISPLAY_ST7789_XOFF
#endif
#if defined(DISPLAY_ST7789_YOFF)
  #undef  PANEL_YOFF
  #define PANEL_YOFF DISPLAY_ST7789_YOFF
#endif

#ifndef DISPLAY_ST7789_ROT
  #define DISPLAY_ST7789_ROT 0            // 0..3; 0/2 — портрет, 1/3 — ландшафт
#endif

// TFT_W/TFT_H — РОБОЧІ розміри екрана з урахуванням орієнтації (для верстки).
#if (DISPLAY_ST7789_ROT & 1)
  #define TFT_W PANEL_H
  #define TFT_H PANEL_W
#else
  #define TFT_W PANEL_W
  #define TFT_H PANEL_H
#endif

// Ручні оффсети пам'яті вмикаються, лише якщо користувач їх задав або явно
// попросив DISPLAY_ST7789_MANUAL_OFFSET. Інакше — покладаємось на init()
// бібліотеки Adafruit (вона знає стандартні панелі 240x240/240x320/135x240/
// 240x280 у свіжих версіях), і підклас із доступом до protected-полів навіть
// не компілюється — стандартний випадок максимально безпечний.
// ── CS: пін, якого на модулі може не бути ──────────────────────────────────
//  Модулі ST7789 240x240 (GMT130 і подібні 1.3"/1.54") виводять лише
//  GND/VCC/SCL/SDA/RES/DC/BLK — CS на платі припаяний до землі, тобто
//  контролер вибраний завжди. Adafruit_SPITFT це штатно підтримує: -1 означає
//  «CS немає», і бібліотека просто не смикає пін (ані pinMode, ані рівні в
//  startWrite()). Тому тут не заглушка, а документований режим драйвера.
//
//  ⚠️ Наслідок, який варто пам'ятати: шину SPI більше не можна ділити. Поки
//  дисплей на ній єдиний — а в цьому проєкті так і є, — це нічого не змінює.
#ifdef DISPLAY_CS_PIN
  #define ST7789_CS_ARG (DISPLAY_CS_PIN)
#else
  #define ST7789_CS_ARG (-1)
#endif

// ── НОМЕР РЕЖИМУ SPI -> КОНСТАНТА ПЛАТФОРМИ ────────────────────────────────
//  У settings.h режим задається числом 0..3 і НАВМИСНО не константою: макроси
//  SPI_MODE0..SPI_MODE3 на різних платформах мають різні значення (на AVR —
//  0x00/0x04/0x08/0x0C, на ESP32 — 0..3), тож «просто число» туди підставляти
//  не можна. Перетворення — тут, де вже підключено SPI.h разом з Adafruit.
#if   (DISPLAY_ST7789_SPI_MODE) == 3
  #define ST7789_SPI_MODE_CONST SPI_MODE3
#elif (DISPLAY_ST7789_SPI_MODE) == 2
  #define ST7789_SPI_MODE_CONST SPI_MODE2
#elif (DISPLAY_ST7789_SPI_MODE) == 1
  #define ST7789_SPI_MODE_CONST SPI_MODE1
#elif (DISPLAY_ST7789_SPI_MODE) == 0
  #define ST7789_SPI_MODE_CONST SPI_MODE0
#else
  #error "DISPLAY_ST7789_SPI_MODE має бути 0, 1, 2 або 3."
#endif

#if defined(DISPLAY_ST7789_MANUAL_OFFSET) || defined(DISPLAY_ST7789_XOFF) || defined(DISPLAY_ST7789_YOFF)
  #define ST7789_USE_OFFSET_CLASS 1
  class ST7789Panel : public Adafruit_ST7789 {
  public:
    ST7789Panel(int8_t cs, int8_t dc, int8_t rst) : Adafruit_ST7789(cs, dc, rst) {}
    void applyOffsets(uint8_t col, uint8_t row) {
      _colstart = col; _rowstart = row;
      _colstart2 = 0;  _rowstart2 = 0;
      setRotation(rotation);             // перерахувати _xstart/_ystart
    }
  };
  static ST7789Panel tft = ST7789Panel(ST7789_CS_ARG, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
#else
  static Adafruit_ST7789 tft = Adafruit_ST7789(ST7789_CS_ARG, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
#endif
static U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// -------------------- Червоний «світлофільтр» на час помилки --------------------
// Під час оповіщення про помилку УВЕСЬ екран стає червоного відтінку (не блимає).
// Реалізовано на рівні палітри: кожен колір C_* проходить через TC(), яка при
// g_errTint переводить його у червоний відтінок тієї ж яскравості. Тож звичайний
// перемальовок сторінки автоматично дає «червоний фільтр» — без окремого коду в
// кожній сторінці й без читання кадрового буфера (ST7789 по SPI його не віддає).
static bool g_errTint = false;

// Перевести колір у червоний відтінок за його яскравістю (немов червоний гель).
inline uint16_t redFilter(uint16_t c) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int y = r * 36 + g * 37 + b * 12;        // яскравість ~0..3819
    int rr = y / 123; if (rr > 31) rr = 31;  // 0..31 -> червоний канал
    int gg = rr >> 2;                        // трохи (теплий відтінок), синій = 0
    return (uint16_t)((rr << 11) | (gg << 5));
}
inline uint16_t TC(uint16_t c) { return g_errTint ? redFilter(c) : c; }

// -------------------- Палітра (RGB565) --------------------
// Кожен колір проходить через TC() -> при помилці весь екран червоний.
#define C_BG      TC(0x0000)     // чорний фон
#define C_CARD    TC(0x18E3)     // темно-сіра картка
#define C_HDRBG   TC(0x0208)     // темно-синій заголовок
#define C_TEXT    TC(0xFFFF)     // білий
#define C_MUTED   TC(0x8C71)     // приглушений сірий
#define C_BLUE    TC(0x02D6)     // синій (прапор UA)
#define C_YELLOW  TC(0xFEA0)     // жовтий (прапор UA)
#define C_GREEN   TC(0x07E6)     // добрий заряд
#define C_ORANGE  TC(0xFC60)     // середній
#define C_RED     TC(0xF800)     // низький / небезпека
// Темна фаза блимання аварійної плашки. Не чорний: плашка мусить лишатись
// ПЛАШКОЮ в обидві фази, інакше замість блимання виходить зникнення.
#define C_DARKRED TC(0x6000)     // темно-червоний — друга фаза блимання

// -------------------- Шрифти, адаптивно за шириною --------------------
// ВАЖЛИВО: беремо ЛИШЕ ті кириличні шрифти, що зашиті в U8g2_for_Adafruit_GFX
// (це підмножина u8g2: 4x6/5x8/6x12/7x13/8x13/9x15/10x20 *_t_cyrillic).
// Немає 9x18_t_cyrillic і fub* — тому великий % малюємо вбудованим шрифтом GFX.
// ⚑ ШИРИНА КОМІРКИ ЙДЕ ПОРУЧ ІЗ ІМЕНЕМ ШРИФТА, А НЕ ОКРЕМО. Усі ці шрифти
//  МОНОШИРИННІ, і число в їхній назві — це і є ширина гліфа в пікселях. Саме
//  вона дозволяє порахувати, скільки символів улізе в плашку, ДО того як
//  малювати (див. textwrap.h). Тримати ці два числа в різних місцях означало б
//  одного дня перемкнути шрифт і забути про ширину — і текст знову поповз би
//  за межі.
#if TFT_W < 200                                   // вузькі панелі (135/170/172)
  #define FONT_HDR    u8g2_font_7x13_t_cyrillic
  #define FONT_BODY   u8g2_font_6x12_t_cyrillic
  #define FONT_SMALL  u8g2_font_5x8_t_cyrillic
  #define FONT_MODEL  u8g2_font_8x13_t_cyrillic
  #define FONT_HDR_W    7
  #define FONT_BODY_W   6
  #define FONT_SMALL_W  5
  #define FONT_MODEL_W  8
  #define BIG_TSIZE   3                           // масштаб вбудованого шрифту GFX
#else                                             // 240-піксельні панелі
  #define FONT_HDR    u8g2_font_10x20_t_cyrillic
  #define FONT_BODY   u8g2_font_9x15_t_cyrillic
  #define FONT_SMALL  u8g2_font_6x12_t_cyrillic
  #define FONT_MODEL  u8g2_font_10x20_t_cyrillic
  #define FONT_HDR_W   10
  #define FONT_BODY_W   9
  #define FONT_SMALL_W  6
  #define FONT_MODEL_W 10
  #define BIG_TSIZE   4
#endif

// -------------------- Розмітка --------------------
#define HDR_H   30
#define FOOT_H  26
#define FOOT_Y  (TFT_H - FOOT_H)

// Заокруглені кути (ST7789V3 1.69" 240x280 зазвичай має скруглені кути) —
// безпечний горизонтальний відступ, щоб текст у кутах не обрізало дугою.
// Вмикається DISPLAY_ST7789_ROUND; радіус можна задати DISPLAY_ST7789_CORNER.
#if defined(DISPLAY_ST7789_ROUND)
  #ifndef DISPLAY_ST7789_CORNER
    #define DISPLAY_ST7789_CORNER 22
  #endif
  #define EDGE DISPLAY_ST7789_CORNER      // відступ біля кутів (шапка/статус/hex)
#else
  #define EDGE 6
#endif
#define CX (EDGE > 14 ? EDGE : 14)        // ліва межа основного контенту

static char g_displayStatus[36] = "ЗАПУСК";
static int  g_displayPage = 0;
// Стан прихованих жестів і повноекранного повідомлення — тут, бо на них
// дивиться і обробник кнопок, і рендер.
static ComboHold  g_hold;
static ComboFlash g_flash;
inline bool displayFlashActive() { return comboFlashActive(g_flash, millis()); }
static bool g_readRequested = false;
static int  g_menuSel = 1;                 // курсор у списку (0 — «‹ Показання»)
static int  g_actionRequested = -1;        // -1 нема; інакше — КОД операції для .ino

// Екранний Майстер відновлення (ті самі глобали, що й у моно-драйвері):
// рендер читає, wizDeviceRefresh() (recovery.h) заповнює, .ino оркеструє.
static int  g_wizReq      = 0;             // 0 нема, 1 аналіз, 2 крок
static int  g_wizProblems = -1;            // -1 ще не аналізовано
static bool g_wizHealthy  = false;
static int  g_wizProg = 0, g_wizTotal = 0;
static bool g_wizAwait = false;
static bool g_wizBusy  = false;
static char g_wizTop[48]  = "";
static char g_wizNext[48] = "";

// Анімація батареї (головна сторінка): фаза + прямокутник шкали + рамка цифр %.
// displayAnimTick() «дихає» яскравістю всього заповнення, але НЕ чіпає рамку
// цифр (g_pct*) — тож великий відсоток не блимає під час пульсації.
static uint8_t g_animPhase = 0;
static int g_battX = 0, g_battY = 0, g_battW = 0, g_battH = 0;
static int g_pctTx = 0, g_pctTy = 0, g_pctTw = 0, g_pctTh = 0;   // рамка цифр %

inline void displayRender();   // визначення нижче

// Емблема НГУ для заставки (1-біт XBM, 64x64). Дублює масив з display.h —
// монохромна й кольорова гілки ніколи не збираються разом, конфлікту немає.
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

// ===================== Спільна логіка (як у display.h) =====================

// Перерахунок контрольної суми TLV-записи DS2433: сума усіх байт == 0x5A.
inline void fixRecordChecksum(uint8_t *buf, int start, int len) {
    int s = 0;
    for (int k = 0; k < len - 1; k++) s += buf[start + k];
    buf[start + len - 1] = (0x5A - s) & 0xFF;
}

// Відсоток заряду. Пріоритет — ICA (якщо IAD=1), інакше по напрузі.
inline int batteryPercent(const char **src) {
    if (!hasDump2438) { *src = "--"; return -1; }
    long vmv = (long)(((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3]) * 10;
    // ⚑ ДРУГОЇ КОПІЇ ШКАЛИ ТУТ БІЛЬШЕ НЕМАЄ. Раніше відсоток рахувався прямо
    //  тут власною лінійною формулою — тобто шкала жила у двох місцях одразу.
    //  Поки обидві були лінійними, вони випадково збігались; щойно головна
    //  стала табличною кривою (soc.h), екран показував би зовсім інше число,
    //  ніж веб і USB-клієнт. Рахуємо тією самою функцією, що й усі.
    int vpct = impresPercentFromMv((int)vmv);
    uint8_t config = batteryDump2438[0];
    if (config & 0x01) {
        // Шкала ICA АПАРАТНА: одиниця = 0.4882 мВ·год / Rsense, а не «255 =
        // повний пакет». Тому відсоток рахуємо через мА·год і паспортну
        // ємність — інакше повний пакет показувався б як ~79 % (2150 мА·год
        // при шунті 0.0459 Ом — це 202 одиниці, а не 255).
        char pm[16] = "";
        if (hasDump) impresModelName(batteryDump, pm, sizeof(pm));
        int ica = impresPercentFromIca(batteryDump2438[12],
                                       impresRatedMahFor(hasDump ? batteryDump : nullptr, pm),
                                       impresBmsRsense(batteryDump2438));
        // Паливомір «завис»: ICA ~0%, а напруга каже «повний» -> показуємо за напругою.
        if (ica + 25 < vpct) { *src = "U!"; return vpct; }
        *src = "ICA";
        return ica;
    }
    *src = "volt";
    return vpct;
}

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

// Знайти модель (part number) в дампі DS2433.
// Основний шлях — валідний запис моделі (довжина 0x0B, 9 символів, Σ≡0x5A);
// запасний — найдовший ASCII-рядок, якщо запис зруйновано.
inline bool decodeModel(char *out, size_t n) {
    if (!hasDump) return false;
    if (impresModelName(batteryDump, out, n)) return true;
    int best = -1, bestLen = 0, i = 0;
    while (i < (int)DUMP_SIZE) {
        uint8_t c = batteryDump[i];
        if (c >= 'A' && c <= 'Z') {
            int j = i + 1; bool hasDigit = false;
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
// ⚠️ РАНІШЕ тут повертали байт зі зсуву +21 у першому записі довжини 0x17 і
// видавали його за «ємність/знос». Це неправильно з двох причин:
//   • запис шукали як «тег 0x17 + 0x00», хоча перший байт запису — ДОВЖИНА;
//   • сам запис @0x129 — ЗАВОДСЬКА таблиця моделі: у dumps/ вона побайтово
//     однакова в усіх 19 екземплярів PMNN4409A і всіх 8 екземплярів PMNN4409B,
//     а байт +21 у них завжди 0x64. Тобто програма ЗАВЖДИ показувала «100% /
//     знос 0%» — незалежно від реального АКБ. Саме про цю розбіжність із
//     показаннями станції й писав власник.
//
// Перебір усіх зсувів і кодувань по 7 АКБ із відомими показаннями рації
// (97/100/100/34/100/100/99 %) збігу не дав: строк служби в прошивці НЕ
// зберігається — його рахує рація з навчених даних. Тому чесно повертаємо
// «невідомо», а не вигадане число.
inline bool decodeCapacity(int *capPct, int *wearPct) {
    (void)capPct; (void)wearPct;
    return false;
}

// Заводська таблиця здоров'я моделі (запис @0x129) — для сторінки аналізу.
// Це НЕ стан конкретного АКБ, а константа моделі; показуємо як довідку.

// Евристика справжності/цілісності ПРОШИВКИ (модельно-залежна).
inline bool batteryGenuine(const char **reason) {
    if (!hasDump) { *reason = "нема дампу"; return false; }
    int hs = 0; for (int i = 0; i <= 0x1F; i++) hs += batteryDump[i];
    if ((hs & 0xFF) != 0x41) { *reason = "хибний заголовок"; return false; }
    // Валідний запис моделі (довжина 0x0B, 9 символів, Σ≡0x5A). Раніше тут
    // вистачало «байт 0x0B, за яким літера», через що сміттєвий чіп міг
    // вважатися таким, що має модель.
    bool hasModel = impresFindModel(batteryDump) >= 0;
    if (!hasModel) { *reason = "нема моделі"; return false; }
    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) { *reason = "CCA перепов."; return false; }
    }
    bool auth = false;
    static const char pat[] = "MOTOROLA";
    const int plen = 8;
    for (int i = 0; i + plen <= (int)DUMP_SIZE && !auth; i++) {
        int k = 0;
        while (k < plen && batteryDump[i + k] == (uint8_t)pat[k]) k++;
        if (k == plen) auth = true;
    }
    if (auth) {
        if (batteryDump[0x1B] == 0xFF && batteryDump[0x1C] == 0xFF &&
            batteryDump[0x1D] == 0xFF && batteryDump[0x1E] == 0xFF) {
            *reason = "стерте калібр."; return false;
        }
        *reason = "OK"; return true;
    }
    *reason = "OK (ф.2014)";
    return true;
}

// ⚑ Тут була numActions() = opCount() — довжина КІЛЬЦЯ дій, якого більше
// немає. Довжину СПИСКУ рахує menuCount() (operations.h), і вона більша за
// opCount(): у списку є ще й переходи на службові сторінки.

// ===================== Примітиви малювання (кольорові) =====================

// Малюємо текст у НЕПРОЗОРОМУ режимі (setFontMode(0)), задаючи фон гліфа
// РІВНИМ кольору ділянки. Так немає чорних ореолів навколо символів на
// кольорових смугах (шапка/статус). За замовчуванням фон = C_BG (чорний).
inline void tSet(const uint8_t *font, uint16_t fg, uint16_t bg = C_BG) {
    u8g2Fonts.setFont(font);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);
}
static bool g_tFooter = false;        // малюємо саму смугу статусу — їй униз можна
// Статус-смуга малюється ПОВЕРХ сторінки, тож усе, що заїхало під неї, однаково
// не видно — але встигає накластися на сусідні рядки. Тому нижче просто не
// пишемо: краще не показати рядок, ніж показати кашу.
inline void tPut(int x, int y, const char *s) {
    if (!g_tFooter && y > FOOT_Y - 2) return;
    u8g2Fonts.drawUTF8(x, y, s);
}
inline int  tWidth(const char *s) { return u8g2Fonts.getUTF8Width(s); }

// Текст ПО ЦЕНТРУ ділянки [x0 .. x0+w) з ПЕРЕНОСОМ по словах. Шрифт і кольори
// має виставити викликач (tSet) — сюди приходить лише геометрія.
//
//  ⚑ Навіщо власний центрувальник, коли поруч є tWidth(). Бо центрування
//  саме по собі нічого не гарантує: (TFT_W - ширина)/2 при задовгому рядку дає
//  ВІД'ЄМНИЙ x, і текст їде за лівий край, а хвостом — за правий. Саме це й
//  бачив власник у банері помилки живлення. Тут ширина спершу обмежується, а
//  вже потім центрується те, що точно влізе.
//
// cellW — ширина гліфа виставленого шрифту (FONT_*_W). Повертає y ПІСЛЯ
// останнього намальованого рядка.
inline int tPutWrapCenter(int x0, int w, int y, int lineH,
                          const char *s, int cellW, int maxLines) {
    if (!s || cellW <= 0 || w <= 0) return y;
    int maxG = w / cellW;
    if (maxG < 1) maxG = 1;
    TxtLine ln[4];
    if (maxLines > 4) maxLines = 4;
    int n = txtWrap(s, maxG, ln, maxLines);
    char buf[80];
    for (int i = 0; i < n; i++) {
        int len = ln[i].len;
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        memcpy(buf, s + ln[i].off, len);
        buf[len] = '\0';
        tPut(x0 + (w - tWidth(buf)) / 2, y, buf);
        y += lineH;
    }
    return y;
}

inline uint16_t chargeColor(int pct) {
    if (pct < 0)   return C_MUTED;
    if (pct >= 60) return C_GREEN;
    if (pct >= 30) return C_YELLOW;
    return C_RED;
}

// counter — що показати праворуч замість номера сторінки (напр. «12/36» у
// меню). nullptr/порожній рядок = номер сторінки в кільці показань.
inline void drawHeaderBar(const char *title, const char *counter = nullptr) {
    tft.fillRect(0, 0, TFT_W, HDR_H, C_HDRBG);
    tft.drawFastHLine(0, HDR_H - 1, TFT_W, C_BLUE);
    char h[16];
    // ⚑ «!» перед номером сторінки — ознака несправності ЖИВЛЕННЯ, видима з
    // БУДЬ-ЯКОЇ сторінки: без блока живлення заряд не піде, хай що користувач
    // зараз гортає. Розшифровка — на сторінці заряду й у вебі.
    bool psuBad = chargePsuFault();
    // Лічильник рахує ТІЛЬКИ кільце показань. Сторінки, відкриті з меню
    // (дампи, Майстер), у кільце не входять, і «5/8» на них означало б, що їх
    // можна догортати, — а їх не можна: вихід із них інший.
    if (counter && *counter)
        snprintf(h, sizeof(h), "%s%s", psuBad ? "!" : "", counter);
    else if (g_displayPage < NUM_STATUS_PAGES)
        snprintf(h, sizeof(h), "%s%d/%d", psuBad ? "!" : "",
                 g_displayPage + 1, NUM_STATUS_PAGES);
    else
        snprintf(h, sizeof(h), "%s", psuBad ? "!" : "");
    tSet(FONT_SMALL, psuBad ? C_RED : C_TEXT, C_HDRBG);
    int cx = TFT_W - tWidth(h) - EDGE;
    tPut(cx, 20, h);
    // Заголовок обрізаємо так, щоб він не заліз під лічильник сторінок: назви
    // на сторінці «Дії» довгі («Дія 12/27 2433+2438»), і на 240-піксельній
    // панелі вони налазили на «7/8».
    tSet(FONT_HDR, C_YELLOW, C_HDRBG);
    char t2[40];
    snprintf(t2, sizeof(t2), "%s", title);
    for (int n = (int)strlen(t2); n > 0 && EDGE + tWidth(t2) > cx - 6; ) {
        while (n > 0 && ((unsigned char)t2[n - 1] & 0xC0) == 0x80) n--;   // UTF-8
        if (n > 0) t2[--n] = 0;
    }
    tPut(EDGE, 21, t2);
}

inline void drawFooterBar() {
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    char f[42];
    snprintf(f, sizeof(f), "%s", g_displayStatus);
    tSet(FONT_BODY, C_GREEN, C_CARD);
    g_tFooter = true; tPut(EDGE, TFT_H - 8, f); g_tFooter = false;
}

// ── ШКАЛА БАТАРЕЇ: МАЛЮЄМО, ЛИШЕ ЯКЩО ЗМІНИВСЯ ГРАФІЧНИЙ РІВЕНЬ ───────────
//  Що вже намальовано на екрані, і «покоління» екрана (міняється щоразу, коли
//  екран чи тіло сторінки чистять повністю — інакше кеш брехав би після
//  fillScreen: стан «той самий», а пікселів уже немає).
static BattBarDrawn g_battDrawn;
static uint32_t     g_screenGen = 1;

// Кликати ПІСЛЯ кожного повного очищення екрана або тіла сторінки.
inline void displayScreenCleared() { g_screenGen++; }

// Іконка батареї зі шкалою заповнення; pct<0 — даних немає.
//
//  ⚑ ГОЛОВНЕ ТУТ — РАННІЙ ВИХІД. Під час заряду/розряду ця функція кличеться
//  на КОЖНОМУ опитуванні (раз на секунду), а displayAnimTick() тим часом ганяє
//  по заповненню градієнт ~9 к/с. Поки ми перемальовували шкалу беззастережно,
//  кожне опитування клало поверх градієнта РІВНУ заливку — і раз на секунду
//  було видно спалах, тобто «анімація скидається». Тепер, якщо графічний рівень
//  (ширина заповнення в пікселях), колір і геометрія ті самі, ми не чіпаємо
//  жодного пікселя, і градієнт біжить безперервно.
//
//  Порівнюється саме ШИРИНА В ПІКСЕЛЯХ, а не відсоток: шкала на 200 px має
//  100 різних положень, тож зміна на 1 % часто не рухає нічого. Порівняння за
//  відсотком лишило б той самий дефект, просто рідше.
inline void drawBatteryBar(int x, int y, int w, int h, int pct, uint16_t col) {
    g_battX = x; g_battY = y; g_battW = w; g_battH = h;   // для displayAnimTick()
    int fw = battFillW(w, pct, 6);

    if (!battBarChanged(g_battDrawn, x, y, w, h, fw, col, g_screenGen)) return;

    // Чистимо ЛИШЕ власний слід (рамка + «плюсовий» вивід), а не смугу на всю
    // ширину: на головній сторінці поруч стоять цифри відсотка, і широке
    // затирання з'їдало б їх.
    tft.fillRect(x - 1, y - 1, w + 6, h + 2, C_BG);

    tft.drawRoundRect(x, y, w, h, 4, C_TEXT);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 3, C_TEXT);
    tft.fillRect(x + w, y + h / 3, 4, h - 2 * (h / 3), C_TEXT);   // "плюсовий" вивід
    if (fw > 0) tft.fillRect(x + 3, y + 3, fw, h - 6, col);

    // Поле за полем, а не агрегатною ініціалізацією: у BattBarDrawn є типові
    // значення полів, і сумісність такої ініціалізації залежить від стандарту.
    // Виграшу від однорядковості тут нема, а причина відмови збірки була б
    // геть неочевидною.
    g_battDrawn.x = x; g_battDrawn.y = y; g_battDrawn.w = w; g_battDrawn.h = h;
    g_battDrawn.fw = fw; g_battDrawn.col = col; g_battDrawn.gen = g_screenGen;
}

// ===================== Заставка =====================
//
// Кастомна КОЛЬОРОВА заставка: покладіть у папку скетчу файл custom_splash.h
// (згенерований tools/make_color_splash.py) і розкоментуйте в settings.h
//   #define DISPLAY_SPLASH_CUSTOM
// Він має визначати SPLASH_W, SPLASH_H і масив splash_rgb565[] (RGB565).
#if defined(DISPLAY_SPLASH_CUSTOM)
  #include "custom_splash.h"
#endif

// ── ЗАСТАВКА ІЗ SPIFFS ────────────────────────────────────────────────────
//  Формат і розбір заголовка — у splash.h (спільні з приймальником у
//  web_server.h і з хостовим тестом). Тут лише виведення на екран.
#if defined(DISPLAY_SPLASH_SPIFFS)
  #include <FS.h>
  #include <SPIFFS.h>
  #include "splash.h"
  #ifdef DISPLAY_SPLASH_JPEG
    #include <TJpg_Decoder.h>
  #endif

// Останній результат спроби — щоб пристрій міг сказати, ЧОМУ показує типову
// заставку замість завантаженої. Мовчазна відмова тут була б найгіршим
// варіантом: людина завантажила файл, нічого не змінилось, і жодного сліду.
static int      g_splashLast = SPLASH_ERR_SHORT;
static uint16_t g_splashW = 0, g_splashH = 0;

inline int      splashLastResult() { return g_splashLast; }

// Намалювати заставку з файла. true — намалювали, false — лишається типова.
//
//  ⚑ ПОТОКОМ, А НЕ ЦІЛИМ БУФЕРОМ. 240×240 RGB565 — це 115 200 байтів; ESP32
//  таке виділити зазвичай може, але робити це заради півтори секунди на
//  старті (та ще й одночасно з підняттям Wi-Fi, який сам просить пам'яті) —
//  марна витрата. Читаємо шматками по рядку-два й одразу відправляємо в шину.
//
//  ⚑ bigEndian=true — і саме тому файл зберігається старшим байтом уперед:
//  так дані йдуть у панель без жодного перевертання (див. splash.h).
#ifdef DISPLAY_SPLASH_JPEG
// Куди TJpg_Decoder віддає розкодовані блоки MCU. Зсув до центру екрана
// рахується один раз при старті декодування й лежить тут, бо сигнатура
// callback-а фіксована й нічого свого в неї не передаси.
static int16_t g_jpgOffX = 0, g_jpgOffY = 0;

// ⚑ swapBytes(true) у splashDrawJpeg() робить порядок байтів таким самим, як
//  у сирого формату, тож обидва шляхи виводять пікселі однаково.
static bool splashJpegBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    if (y >= TFT_H) return false;                 // нижче екрана — decoder зупиниться
    tft.drawRGBBitmap(g_jpgOffX + x, g_jpgOffY + y, bmp, w, h);
    return true;
}

// Намалювати JPEG зі SPIFFS. Розміри перевіряємо ДО малювання: TJpgDec уміє
// віддати їх, не декодуючи кадр.
inline bool splashDrawJpeg() {
    uint16_t w = 0, h = 0;
    if (TJpgDec.getFsJpgSize(&w, &h, DISPLAY_SPLASH_JPG_PATH, SPIFFS) != JDR_OK) {
        g_splashLast = SPLASH_ERR_MAGIC;          // не розібрався — отже не JPEG
        return false;
    }
    // ⚑ ЗАВЕЛИКУ КАРТИНКУ НЕ ВІДХИЛЯЄМО, А ЗМЕНШУЄМО. Коефіцієнт один на обидві
    //  осі, тож пропорції зберігаються самі собою; докладно — у splash.h.
    uint8_t sc = splashJpegScaleFor(w, h, TFT_W, TFT_H);
    if (!sc) {
        g_splashLast = (w == 0 || h == 0) ? SPLASH_ERR_ZERO : SPLASH_ERR_TOO_BIG;
        return false;
    }
    uint16_t dw = splashScaled(w, sc), dh = splashScaled(h, sc);

    //  Зсув рахується від РОЗКОДОВАНОГО розміру, а не від вихідного: callback
    //  отримує координати вже в зменшеному просторі. Узявши тут вихідні w/h,
    //  ми б зсунули картинку за край рівно на різницю.
    g_jpgOffX = (int16_t)((TFT_W - (int)dw) / 2);
    g_jpgOffY = (int16_t)((TFT_H - (int)dh) / 2);
    if (g_jpgOffX < 0) g_jpgOffX = 0;
    if (g_jpgOffY < 0) g_jpgOffY = 0;

    TJpgDec.setJpgScale(sc);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(splashJpegBlock);
    if (TJpgDec.drawFsJpg(0, 0, DISPLAY_SPLASH_JPG_PATH, SPIFFS) != JDR_OK) {
        // Кадр міг лягти наполовину — не лишаємо огризок на екрані.
        tft.fillScreen(C_BG);
        displayScreenCleared();
        g_splashLast = SPLASH_ERR_SIZE;
        return false;
    }
    g_splashLast = SPLASH_OK; g_splashW = dw; g_splashH = dh;
    if (sc != 1)
        Serial.printf("Splash: JPEG %ux%u зменшено в %u рази -> %ux%u\n", w, h, sc, dw, dh);
    return true;
}
#endif  // DISPLAY_SPLASH_JPEG

inline bool splashDrawFromFs() {
    g_splashLast = SPLASH_ERR_SHORT; g_splashW = g_splashH = 0;

    // Монтуємо БЕЗ форматування: заставка малюється раніше, ніж setup() дійде
    // до SPIFFS.begin(true), і відформатувати чужу файлову систему заради
    // картинки — неприпустимо. Не змонтувалось — просто типова заставка.
    if (!SPIFFS.begin(false) && !SPIFFS.exists(DISPLAY_SPLASH_PATH)) return false;

    // ⚑ JPEG ПЕРШИМ. Обидва формати можуть лежати поруч, і перевага в JPEG не
    //  довільна: він з'являється лише тоді, коли користувач щойно його
    //  завантажив (приймальник видаляє інший формат), тож це завжди свіжіший
    //  вибір. Не розкодувався — тихо пробуємо сирий.
#ifdef DISPLAY_SPLASH_JPEG
    if (SPIFFS.exists(DISPLAY_SPLASH_JPG_PATH) && splashDrawJpeg()) return true;
#endif
    if (!SPIFFS.exists(DISPLAY_SPLASH_PATH)) return false;

    File f = SPIFFS.open(DISPLAY_SPLASH_PATH, "r");
    if (!f) return false;

    uint8_t hdr[SPLASH_HDR_BYTES];
    size_t  nHdr = f.read(hdr, sizeof(hdr));
    uint16_t w = 0, h = 0;
    g_splashLast = splashParse(hdr, nHdr, f.size(), TFT_W, TFT_H, &w, &h);
    if (g_splashLast != SPLASH_OK) { f.close(); return false; }
    g_splashW = w; g_splashH = h;

    int sx = (TFT_W - (int)w) / 2, sy = (TFT_H - (int)h) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    static uint16_t line[DISPLAY_SPLASH_MAX_W];
    tft.startWrite();
    tft.setAddrWindow(sx, sy, w, h);
    for (uint16_t y = 0; y < h; y++) {
        size_t want = (size_t)w * 2;
        if (f.read((uint8_t *)line, want) != want) {   // файл обірвався на ходу
            tft.endWrite(); f.close();
            g_splashLast = SPLASH_ERR_SIZE;
            tft.fillScreen(C_BG);
            displayScreenCleared();                      // не лишати півкартинки
            return false;
        }
        tft.writePixels(line, w, true, true);          // block=true, bigEndian=true
    }
    tft.endWrite();
    f.close();
    return true;
}
#endif  // DISPLAY_SPLASH_SPIFFS

inline void displaySplash() {
    // Заставка — на ВЕСЬ екран, статус-смуги на ній немає, тож запобіжник
    // «нижче смуги не писати» тут має бути вимкнений: інакше нижній рядок
    // напису зник би на низьких панелях.
    g_tFooter = true;
    tft.fillScreen(C_BG);
    displayScreenCleared();

    // ⚑ ПОРЯДОК ДЖЕРЕЛ: спершу файл зі SPIFFS, потім вкомпільована картинка,
    //  потім типова. Саме так, а не навпаки: завантажена користувачем
    //  заставка — це його свідомий вибір, зроблений ПІЗНІШЕ за прошивання, і
    //  вона мусить перекривати те, що зашите в код. Немає файла, він битий
    //  або чужого формату — тихо відкочуємось на наступне джерело, а причину
    //  лишаємо в splashLastResult() для сторінки стану.
#if defined(DISPLAY_SPLASH_SPIFFS)
    if (splashDrawFromFs()) { g_tFooter = false; return; }
#endif

#if defined(DISPLAY_SPLASH_CUSTOM)
    // Кастомна кольорова картинка по центру екрана.
    int sx = (TFT_W - (int)SPLASH_W) / 2;
    int sy = (TFT_H - (int)SPLASH_H) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    tft.drawRGBBitmap(sx, sy, (uint16_t *)splash_rgb565, SPLASH_W, SPLASH_H);
#else
    // Типова заставка НГУ (синьо-жовта, тризуб).
    tft.fillRect(0, 0, TFT_W, 6, C_BLUE);
    tft.fillRect(0, 6, TFT_W, 6, C_YELLOW);
    int gx = (TFT_W - NGU_W) / 2;
    int gy = (TFT_H > 240) ? (TFT_H / 2 - NGU_H) : 34;   // трохи вище центру
    if (gy < 20) gy = 20;
    tft.drawXBitmap(gx, gy, ngu_xbm, NGU_W, NGU_H, C_YELLOW);
    const char *l1 = "Національна Гвардія";
    const char *l2 = "України";
    const char *l3 = "IMPRES tool";
    tSet(FONT_HDR, C_TEXT);
    tPut((TFT_W - tWidth(l1)) / 2, gy + NGU_H + 30, l1);
    tSet(FONT_HDR, C_YELLOW);
    tPut((TFT_W - tWidth(l2)) / 2, gy + NGU_H + 54, l2);
    tSet(FONT_BODY, C_MUTED);
    tPut((TFT_W - tWidth(l3)) / 2, gy + NGU_H + 82, l3);
#endif
    g_tFooter = false;
}

// Плавна поява/зникнення заставки (замість displaySplash()+delay() у setup()).
// Екран уже намальований під ВИМКНЕНОЮ підсвіткою (init) — тож артефактів немає;
// далі плавно піднімаємо/опускаємо ШІМ підсвітки.
inline void displayIntro() {
    displaySplash();                         // малюнок готовий (підсвітка ще 0)
#ifdef DISPLAY_BLK_PIN
    // Плавно піднімаємо підсвітку ОДИН раз і ЗАЛИШАЄМО заставку світитись — вона
    // тримається на екрані впродовж усієї ініціалізації (Wi-Fi/SPIFFS/веб), як
    // логотип завантаження. НЕ гасимо назад у чорне: інакше вийшло б два спалахи
    // (заставка «зазвучувалась» двічі — при появі та вдруге при вході в меню).
    for (int b = 0; b <= 255; b += 8) { analogWrite(DISPLAY_BLK_PIN, b); delay(14); }
    analogWrite(DISPLAY_BLK_PIN, 255);
    delay(1500);                             // тримаємо заставку видимою (лишається
                                             // світитись і далі — під час init —
                                             // тож без другого спалаху)
    // Далі керування повертається у setup(); плавний перехід до меню робить
    // displayFadeInMain() коротким «дипом» яскравості (без повного затемнення).
#else
    delay(1800);                             // без керування BLK — просто пауза
#endif
}

// Плавний вхід у головне меню наприкінці setup().
inline void displayFadeInMain();             // тіло нижче (після displayRender)

// ===================== Сторінки =====================

inline void drawPageMain() {
    char buf[48];
    const char *src;
    int pct = batteryPercent(&src);
    int mah = batteryRemainingMah();
    // ⚑ ШКАЛА Й ТЕКСТ ФАРБУЮТЬСЯ ПО-РІЗНОМУ, І ЦЕ НАВМИСНО. Заливка —
    //  battFillColor(): неперервний перехід червоне -> зелене, бо це і є показ
    //  РІВНЯ, і на великій площі відтінок читається сам собою. Написи там, де
    //  вони фарбуються станом, лишаються на chargeColor() — три чіткі
    //  сходинки: дрібні гліфи проміжного відтінку на чорному видно гірше, а
    //  колір тексту тут означає вирок («мало / середньо / добре»), а не рівень.
    uint16_t col = battFillColor(pct);

    drawHeaderBar("Moto IMPRES");

    // Розкладка: спершу ВЕЛИКИЙ % ЛІВОРУЧ, далі — батарея праворуч (трохи коротша),
    // щоб текст і шкала не наповзали одне на одне.
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    // На НИЗЬКИХ панелях (320x170, 240x135 у ландшафті) на все місця немає:
    // віддаємо його рядкам показань, а батарею й цифри стискаємо. Інакше
    // великий відсоток налазив би на текст — так було й раніше.
    int by = HDR_H + 12;
    bool tight  = (FOOT_Y - by) < 150;
    bool vtight = (FOOT_Y - by) < 90;              // 240x135: місця майже немає
    int bh   = vtight ? 24 : (tight ? 36 : 52);
    int bts  = vtight ? 2 : (tight ? (BIG_TSIZE > 3 ? 3 : BIG_TSIZE) : BIG_TSIZE);
    int pctW = 6 * bts * 4;                       // зона під найширше "100%"
    int gap  = 10;
    int bx = CX + pctW + gap;                     // батарея праворуч від тексту
    int bw = TFT_W - CX - 4 - bx;                 // решта ширини (−4 px під «+» вивід)
    // % — вертикально по центру батареї, вирівняно ПРАВОРУЧ у своїй зоні (правий
    // край числа завжди біля батареї, попри різну кількість цифр).
    {
        int cw = 6 * bts * (int)strlen(buf);
        int ch = 8 * bts;
        int tx = CX + (pctW - cw);
        int ty = by + (bh - ch) / 2;
        tft.setTextColor(C_TEXT);
        tft.setTextSize(bts);
        tft.setCursor(tx, ty);
        tft.print(buf);
        tft.setTextSize(1);
    }
    drawBatteryBar(bx, by, bw, bh, pct, col);
    g_pctTx = g_pctTy = g_pctTw = g_pctTh = 0;    // цифри поза шкалою -> градієнт на всю батарею

    // Джерело показника (ICA/volt). На найкоротших панелях його немає куди
    // подіти без накладання — і воно найменш потрібне: сам відсоток видно.
    if (!vtight && FOOT_Y - (by + bh) > 40) {
        tSet(FONT_SMALL, C_MUTED);
        snprintf(buf, sizeof(buf), "джерело: %s", src);
        tPut(CX, by + bh + (tight ? 11 : 14), buf);
    }

    // Деталі. П'ять рядків: залишок, DS2438, IP, точка доступу, пароль.
    //
    // Підказку по кнопках малюємо ОКРЕМО й ВНИЗУ, тим самим шрифтом, що
    // показання: їх читають безперервно, а підказку — коли шукають, куди
    // натиснути, і шукають її саме внизу екрана.
    //
    // ⚑ Рядки, що не влазять, НЕ малюємо взагалі. Коротким панелям (320x170,
    // 240x135) місця фізично бракує, і раніше нижні рядки просто лягали
    // поверх верхніх. Показати менше — чесніше, ніж накласти текст на текст.
    int hintY = FOOT_Y - 9;                        // базова лінія підказки
    bool showHint = true;
    int y  = by + bh + (vtight ? 12 : (tight ? 26 : 34));   // базова лінія 1-го рядка
    int rh = 22, rs = 16;                          // крок великих / малих рядків
    int need = 2 * rh + 2 * rs;                    // від 1-го до 5-го рядка
    int room = (hintY - 20) - y;
    while (need > room && (rh > 15 || rs > 12)) {  // тиснемо, поки не влізе
        if (rh > 15) rh--;
        if (rs > 12) rs--;
        need = 2 * rh + 2 * rs;
    }
    // Що саме показуємо. Порядок на екрані фіксований, а от ЩО викидати, коли
    // місця бракує, вирішує prio: менше число — важливіше. IP, назва точки
    // доступу й пароль ідуть першими, бо їх нема більше ніде; заряд у мА·год
    // дублює великий відсоток вище, а напруга/струм мають власну сторінку.
    struct MainRow { char txt[48]; bool small; uint16_t col; int prio; bool keep; };
    MainRow rows[5];
    int nr = 0;
    auto addRow = [&](const char *t, bool small, uint16_t col, int prio) {
        snprintf(rows[nr].txt, sizeof(rows[nr].txt), "%s", t);
        rows[nr].small = small; rows[nr].col = col; rows[nr].prio = prio;
        rows[nr].keep = true; nr++;
    };

    if (mah >= 0) snprintf(buf, sizeof(buf), "Залишок: %d мА·год", mah);
    else          snprintf(buf, sizeof(buf), "Залишок: --");
    addRow(buf, false, C_TEXT, 4);

    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t  traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        // Струм із вбудованого датчика DS2438 (його вимірювальний резистор стоїть
        // усередині пакета послідовно з банками). Від'ємний = розряд.
        int16_t  iraw = (int16_t)(((uint16_t)batteryDump2438[6] << 8) | batteryDump2438[5]);
        int      i_mA = (int)((float)iraw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f);
        snprintf(buf, sizeof(buf), "%.2fВ %dмА %.1f°C",
                 vraw * 0.01f, i_mA, traw * 0.03125f);
    } else snprintf(buf, sizeof(buf), "DS2438: немає даних");
    addRow(buf, false, C_TEXT, 5);

    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);
    addRow(buf, false, C_BLUE, 1);
    // Точка доступу й пароль — поруч з IP: щоб під'єднатися з телефона, потрібні
    // всі три, а шукати їх у settings.h саме тоді, коли пристрій у руках, —
    // найгірший момент.
    snprintf(buf, sizeof(buf), "Wi-Fi: %s", AP_SSID);
    addRow(buf, true, C_MUTED, 2);
    snprintf(buf, sizeof(buf), "Пароль: %s", AP_PASSWORD);
    addRow(buf, true, C_MUTED, 3);

    // Викидаємо найменш важливе, поки решта не влізе. Раніше нижні рядки просто
    // лягали поверх верхніх — на коротких панелях (320x170, 240x135) сторінка
    // перетворювалась на кашу.
    if (vtight) for (int i = 0; i < nr; i++) rows[i].small = true;
    auto totalH = [&]() {
        int h = 0;
        for (int i = 0; i < nr; i++) if (rows[i].keep) h += rows[i].small ? rs : rh;
        return h;
    };
    int avail = (hintY - 16) - (y - 15);           // від верху 1-го рядка до підказки
    // Якщо навіть найпотрібніше (IP + точка доступу + пароль) не влазить —
    // жертвуємо ПІДКАЗКОЮ, а не даними: без пароля пристроєм не скористатись,
    // а що кнопки гортають меню, видно з лічильника сторінок у шапці.
    {
        int needTop = 0;
        for (int i = 0; i < nr; i++)
            if (rows[i].prio <= 3) needTop += rows[i].small ? rs : rh;
        if (needTop > avail) { showHint = false; avail = (FOOT_Y - 8) - (y - 15); }
    }
    while (totalH() > avail) {
        int worst = -1;
        for (int i = 0; i < nr; i++)
            if (rows[i].keep && (worst < 0 || rows[i].prio > rows[worst].prio)) worst = i;
        if (worst < 0) break;
        rows[worst].keep = false;
    }
    for (int i = 0; i < nr; i++) {
        if (!rows[i].keep) continue;
        tSet(rows[i].small ? FONT_SMALL : FONT_BODY, rows[i].col);
        tPut(CX, y, rows[i].txt);
        y += rows[i].small ? rs : rh;
    }

    // Підказка по кнопках — унизу й ТИМ САМИМ шрифтом, що показання: раніше
    // вона стояла в загальному потоці рядків найдрібнішим шрифтом і читалась
    // гірше за все інше на екрані.
    // Підказка — тим самим шрифтом, що показання. Довгий варіант не на кожній
    // панелі влазить у ширину, тож обираємо за реальним виміром, а не «на око»:
    // обрізаний хвіст читається гірше за коротший, але цілий напис.
    tSet(FONT_BODY, C_MUTED);
#ifdef MENU_BTN3_PIN
    const char *hint = "[<][>] гортати  [OK] читати";
    if (CX + tWidth(hint) > TFT_W - 4) hint = "[<][>] меню  [OK] чит.";
    if (CX + tWidth(hint) > TFT_W - 4) hint = "[<][>] [OK]";
#else
    const char *hint = "[>] довго — зчитати АКБ";
    if (CX + tWidth(hint) > TFT_W - 4) hint = "[>] довго — читати";
#endif
    if (showHint) tPut(CX, hintY, hint);

    drawFooterBar();
}

inline void drawPageModel() {
    drawHeaderBar("Модель / Серійний");
    char model[24];

    tSet(FONT_BODY, C_MUTED);
    tPut(CX, 62, "Модель:");
    if (decodeModel(model, sizeof(model))) {
        tSet(FONT_MODEL, C_YELLOW);
        tPut(CX + 4, 92, model);
    } else {
        tSet(FONT_BODY, C_MUTED);
        tPut(CX + 4, 92, hasDump ? "(невідомо)" : "(зчитайте)");
    }

    tSet(FONT_BODY, C_MUTED);
    tPut(CX, 140, "Серійний (DS2438):");
    if (hasSN2438) {
        char sn[20]; int p = 0;
        for (int i = 0; i < 8; i++) p += snprintf(sn + p, sizeof(sn) - p, "%02X", chipSN2438[i]);
        tSet(FONT_BODY, C_TEXT);
        tPut(CX + 4, 168, sn);
    } else {
        tSet(FONT_BODY, C_MUTED);
        tPut(CX + 4, 168, "(зчитайте АКБ)");
    }
    drawFooterBar();
}

inline void drawPageTech() {
    char buf[48];
    drawHeaderBar("Дані батареї");

    if (!hasDump2438) {
        tSet(FONT_BODY, C_MUTED);
        tPut(CX, 70, "Немає даних DS2438.");
        tPut(CX, 96, "Спочатку зчитайте АКБ.");
        drawFooterBar();
        return;
    }

    uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
    int16_t  traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
    int16_t  iraw = (int16_t)(((uint16_t)batteryDump2438[6] << 8) | batteryDump2438[5]);
    float    i_mA = (float)iraw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f;
    int      remMah = batteryRemainingMah();   // за паспортною ємністю моделі

    int y = 66;
    tSet(FONT_BODY, C_TEXT);
    snprintf(buf, sizeof(buf), "Напруга:   %.2f В", vraw * 0.01f);    tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Струм:     %.0f мА", i_mA);           tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Темп.:     %.1f °C", traw * 0.03125f); tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Залишок:   ~%d мА·год", remMah);      tPut(CX, y, buf);

    drawFooterBar();
}

// Сторінка «Стан АКБ».
//
//  Тут раніше стояло «Ємність: (зчитайте)» — і воно НЕ зникало після зчитування,
//  бо decodeCapacity() принципово повертає false: строк служби в прошивці не
//  зберігається, його рахує рація з навчених даних (див. коментар до
//  decodeCapacity вище). Напис штовхав шукати неіснуючу дію.
//
//  Тепер сторінка показує те, що прошивка справді знає: ПАСПОРТНУ ємність за
//  моделлю і ЗАЛИШОК за паливоміром DS2438 — обидва числа реальні. Про знос
//  сказано чесно, і лише за відсутності дампу пропонується зчитати.
inline void drawPageHealth() {
    char buf[48];
    drawHeaderBar("Стан АКБ");

    int y = 66;
    // Рядки, що не влазять до статус-смуги, просто не малюємо — порядок нижче
    // за спаданням важливості (на вузьких панелях місця менше).
    auto row = [&](const char *txt, uint16_t col, int step) {
        if (y > FOOT_Y - 10) return;
        tSet(FONT_BODY, col); tPut(CX, y, txt); y += step;
    };

    if (!hasDump && !hasDump2438) {
        row("Ємність: зчитайте АКБ", C_MUTED, 30);
    } else {
        char m[16] = "";
        if (hasDump) impresModelName(batteryDump, m, sizeof(m));
        int rated = impresRatedMahFor(hasDump ? batteryDump : nullptr, m);
        snprintf(buf, sizeof(buf), "Ємність: %d мА*год", rated);
        row(buf, C_TEXT, 30);

        int rem = batteryRemainingMah();
        if (rem >= 0) {
            const char *src; int pct = batteryPercent(&src);
            snprintf(buf, sizeof(buf), "Залишок: %d мА*год %d%%", rem, pct < 0 ? 0 : pct);
            row(buf, chargeColor(pct), 30);
        }
    }

    // Вирок про справжність — до лічильника циклів: якщо рядків забракне
    // (вузькі панелі), обрізати треба менш важливе.
    if (hasDump || hasDump2438) {
        const char *reason;
        if (batteryGenuine(&reason)) {
            snprintf(buf, sizeof(buf), "Справжня: ТАК  (%s)", reason);
            row(buf, C_GREEN, 30);
        } else {
            snprintf(buf, sizeof(buf), "РИЗИК: %s", reason);
            row(buf, C_RED, 30);
        }
    }

    // ── штатні поля Motorola (impres_bms.h) ────────────────────────────────
    //  Тут раніше було «Знос: рахує рація», а цикли оцінювались із CCA. Обидва
    //  числа насправді лежать у чипі: цикли — у гістограмі доданого заряду
    //  (ключа не потребує), знос — у зашифрованому CTS блока калібрування.
    const ImpresBms &bms = impresBmsOf(hasDump ? batteryDump : nullptr,
                                       hasDump2438 ? batteryDump2438 : nullptr,
                                       hasSN2433 ? chipSN2433 : nullptr,
                                       DS2438_RSENSE_OHM);
    if (bms.ok && bms.cycles >= 0) {
        if (bms.nonImpresCycles > 0)
            snprintf(buf, sizeof(buf), "Циклів: %d (+%d не IMPRES)",
                     bms.cycles, bms.nonImpresCycles);
        else
            snprintf(buf, sizeof(buf), "Циклів: %d", bms.cycles);
        row(buf, C_TEXT, 30);
    }
    if (bms.ok && bms.haveKey) {
        snprintf(buf, sizeof(buf), "Знос: %d%%  (%d мА*год)", bms.health, bms.potentialMah);
        row(buf, bms.health >= 80 ? C_GREEN : (bms.health >= 60 ? C_TEXT : C_RED), 30);
        if (bms.useY) {
            snprintf(buf, sizeof(buf), "Перше вмикання: %04d-%02d-%02d",
                     bms.useY, bms.useM, bms.useD);
            row(buf, C_MUTED, 30);
        }
    } else if (hasDump || hasDump2438) {
        row("Знос: ключ не визначено", C_MUTED, 22);
        if (y <= FOOT_Y - 10) {
            tSet(FONT_SMALL, C_MUTED);
            tPut(CX, y, "потрібен ROM-ID чипа DS2433");
        }
    }

    drawFooterBar();
}

// Сирий дамп (hex): FONT_SMALL (6x12), по perRow байт у рядку.
inline void drawRawColor(const char *title, const uint8_t *data, bool has, int count) {
    drawHeaderBar(title);
    if (!has) {
        tSet(FONT_BODY, C_MUTED);
        tPut(CX, 70, "немає даних (зчитайте)");
        drawFooterBar();
        return;
    }
    tSet(FONT_SMALL, C_TEXT);
    char buf[48];
    const int perRow = 8;
    int y = HDR_H + 16;
    for (int off = 0; off < count; off += perRow) {
        if (y > FOOT_Y - 10) break;                 // не заходити під статус-смугу
        int n = snprintf(buf, sizeof(buf), "%03X:", off);
        for (int c = 0; c < perRow && off + c < count; c++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02X ", data[off + c]);
        tPut(EDGE, y, buf);
        y += 14;
    }
    drawFooterBar();
}

#define RAW2433_COUNT ((FOOT_Y - HDR_H) / 14 * 8)
inline void drawPageRaw2438() { drawRawColor("DS2438 (hex)", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawColor("DS2433 (hex)", batteryDump, hasDump, RAW2433_COUNT); }

// Сторінка «Дії»: одна обрана операція крупно + опис + попередження.
// ── СТОРІНКА ПОМИЛКИ ЖИВЛЕННЯ ──────────────────────────────────────────────
//  З'являється САМА, щойно контроль живлення бачить несправність. Без блока
//  живлення заряд не піде взагалі, тож причина має бути на екрані, а не за
//  кілька натискань кнопки.
//  Червоне тло на всю площу: цю сторінку не можна сплутати зі звичайною —
//  решта екранів у проєкті на темному тлі.
// ── ПЛАШКА ПОМИЛКИ ЖИВЛЕННЯ ───────────────────────────────────────────────
//  ⚠️ ТУТ БУЛА ПОМИЛКА, І ЇЇ ВАРТО НАЗВАТИ: спершу блимав САМ ТЕКСТ —
//  півперіоду на екрані висіла порожня червона смуга. Тобто рівно тоді, коли
//  користувач дивиться на екран, він міг побачити «щось червоне» без жодного
//  слова про причину. Аварійне повідомлення, яке пів часу нічого не повідомляє,
//  гірше за статичне.
//  Правильно навпаки: БЛИМАЄ ПЛАШКА (тло й рамка), а текст усередині стоїть
//  нерухомо й читається в обидві фази. Рух привертає увагу, зміст лишається.
#define PSU_PLATE_Y  (HDR_H + 8)

// ── ГЕОМЕТРІЯ ПЛАШКИ РАХУЄТЬСЯ, А НЕ ПІДБИРАЄТЬСЯ ─────────────────────────
//  Висота була константою 108, і трималась вона рівно доти, доки написи
//  вміщались у рядок. Не вміщались: «блок живлення просів або не той» — це 31
//  гліф, тобто 279 px шрифтом 9x15, при плашці 236 px і панелі 240 px.
//
//  Тепер під кожен напис ЗАРЕЗЕРВОВАНО стільки рядків, скільки може
//  знадобитись після переносу, і висота складається з них. Резерв, а не
//  вимір за фактом: плашка ще й БЛИМАЄ, перемальовуючись на місці, і плаваюча
//  висота лишала б смуги від попереднього стану.
#define PSU_PAD       4                       // поле від рамки до тексту
#define PSU_PLATE_W   (TFT_W - 2 * (EDGE - PSU_PAD))
#define PSU_INNER_W   (PSU_PLATE_W - 2 * PSU_PAD)
// ⚑ СКІЛЬКИ РЯДКІВ РЕЗЕРВУВАТИ — РАХУЄМО, А НЕ ВГАДУЄМО ЗА ШИРИНОЮ ПАНЕЛІ.
//  Спроба прив'язатись до «TFT_W < 200» тут уже провалилась: на панелі 240 зі
//  СКРУГЛЕНИМИ кутами (EDGE 22) у плашку влазить лише 21 гліф, а найдовше
//  пояснення — 22. Тобто справа не в панелі, а в тому, скільки лишається
//  ПІСЛЯ відступів, і рахувати треба саме це.
//
//  Числа нижче — довжина найдовшого зі штатних написів у гліфах. Вони не
//  «підібрані»: display_fit_check перевіряє КОЖЕН напис проти цього резерву на
//  всіх підтримуваних панелях, тож змінити текст і не помітити не вийде.
#define PSU_HEAD_MAX_GLYPHS 16                // «НАПРУГА ЗАНИЖЕНА»
#define PSU_SUB_MAX_GLYPHS  22                // «блок просів або не той»
#define PSU_HEAD_LINES ((PSU_INNER_W / FONT_MODEL_W) >= PSU_HEAD_MAX_GLYPHS ? 1 : 2)
#define PSU_SUB_LINES  ((PSU_INNER_W / FONT_BODY_W)  >= PSU_SUB_MAX_GLYPHS  ? 1 : 2)
#define PSU_NUM_LINES  3                      // «є X В», «треба Y В», допуск
#define PSU_LH_HEAD   22                      // крок рядка заголовка
#define PSU_LH_BODY   18                      // крок рядка пояснень
#define PSU_PLATE_H   (12 + PSU_HEAD_LINES * PSU_LH_HEAD + \
                       PSU_SUB_LINES * PSU_LH_BODY + PSU_NUM_LINES * PSU_LH_BODY + 8)

// Текст помилки — одним місцем на всі поверхні, щоб екран, веб і USB-клієнт
// не розповідали різне. Перший рядок — крупно, другий — пояснення.
inline const char *psuHeadline(uint8_t st) {
    return (st == PSU_ABSENT) ? "НЕМАЄ ЖИВЛЕННЯ"
         : (st == PSU_LOW)    ? "НАПРУГА ЗАНИЖЕНА"
                              : "НАПРУГА ЗАВИЩЕНА";
}
// ⚑ Написи навмисно КОРОТКІ. Перенос нижче — запобіжник на випадок правки й
//  вузьких панелей, а не спосіб верстки: два рядки там, де досить одного,
//  читаються гірше. Слово «живлення» з пояснень прибрано не заради економії —
//  воно вже стоїть і в шапці сторінки («ПОМИЛКА ЖИВЛЕННЯ»), і в заголовку
//  плашки, тож утретє нічого не додає. Хостовий тест стежить, щоб усі три
//  вміщались у рядок на штатній панелі.
inline const char *psuSubline(uint8_t st) {
    return (st == PSU_ABSENT) ? "блок не під'єднано"
         : (st == PSU_LOW)    ? "блок просів або не той"
                              : "не той блок (19 В?)";
}

inline void drawPsuPlate(bool on) {
    uint8_t  st = chargePsuState();
    uint16_t mv = chargePsuMv();
    char b[64];

    // Дві фази — яскраво-червона й темна. Текст білий в обидві: він мусить
    // читатись завжди, а не лише на «яскравій» половині періоду.
    uint16_t bg = on ? C_RED : C_DARKRED;
    const int px = EDGE - PSU_PAD;                 // ліва межа плашки
    const int tx = px + PSU_PAD;                   // ліва межа ТЕКСТУ всередині
    tft.fillRect(px, PSU_PLATE_Y, PSU_PLATE_W, PSU_PLATE_H, bg);
    tft.drawRect(px, PSU_PLATE_Y, PSU_PLATE_W, PSU_PLATE_H, on ? C_YELLOW : C_RED);

    // ⚑ Усе, що нижче, центрується В МЕЖАХ ПЛАШКИ і переноситься, якщо не
    //  влазить. Раніше центрували по всій ширині екрана й без обмеження —
    //  довгий рядок їхав за обидва краї одразу.
    int y = PSU_PLATE_Y + 12 + PSU_LH_HEAD - 4;
    tSet(FONT_MODEL, C_TEXT, bg);
    tPutWrapCenter(tx, PSU_INNER_W, y, PSU_LH_HEAD,
                   psuHeadline(st), FONT_MODEL_W, PSU_HEAD_LINES);
    y += PSU_HEAD_LINES * PSU_LH_HEAD;

    tSet(FONT_BODY, C_TEXT, bg);
    tPutWrapCenter(tx, PSU_INNER_W, y, PSU_LH_BODY,
                   psuSubline(st), FONT_BODY_W, PSU_SUB_LINES);
    y += PSU_SUB_LINES * PSU_LH_BODY;

    snprintf(b, sizeof(b), "є %u.%02u В", mv / 1000, (mv % 1000) / 10);
    tPutWrapCenter(tx, PSU_INNER_W, y, PSU_LH_BODY, b, FONT_BODY_W, 1);
    y += PSU_LH_BODY;

    // ⚑ Головне число тут — НОМІНАЛ («треба 14 В»), а не допуск. Допуск
    // відповідає на питання «чому цей блок відхилено», а користувачеві
    // потрібна відповідь на «який тоді під'єднати» — тож номінал іде першим і
    // тим самим шрифтом, а межі — окремим рядком під ним.
    //
    // ⚑ І САМЕ ТОМУ ЦЕ ДВА РЯДКИ, А НЕ ОДИН. Разом («треба 14 В (12.6…15.6)»)
    //  виходило 22 гліфи — на штатній панелі влазить, а на 135-піксельній і
    //  на будь-якій зі скругленими кутами вже ні. Розбивши на номінал і
    //  допуск, отримуємо 12 і 11 гліфів: вміщається скрізь без переносу.
    char nom[8];
    snprintf(b, sizeof(b), "треба %s В",
             chargeMvShort(CHARGE_SUPPLY_MV, nom, sizeof(nom)));
    tPutWrapCenter(tx, PSU_INNER_W, y, PSU_LH_BODY, b, FONT_BODY_W, 1);
    y += PSU_LH_BODY;

    snprintf(b, sizeof(b), "(%u.%u…%u.%u)",
             CHARGE_PSU_MIN_MV / 1000, (CHARGE_PSU_MIN_MV % 1000) / 100,
             CHARGE_PSU_MAX_MV / 1000, (CHARGE_PSU_MAX_MV % 1000) / 100);
    tPutWrapCenter(tx, PSU_INNER_W, y, PSU_LH_BODY, b, FONT_BODY_W, 1);
}

inline void drawPagePsuFault() {
    uint8_t st = chargePsuState();

    tft.fillScreen(C_BG);

    displayScreenCleared();
    tft.fillRect(0, 0, TFT_W, HDR_H, C_RED);
    tft.drawFastHLine(0, HDR_H - 1, TFT_W, C_YELLOW);
    tSet(FONT_HDR, C_TEXT, C_RED);
    tPut(EDGE, 21, "ПОМИЛКА ЖИВЛЕННЯ");

    drawPsuPlate(true);                       // плашка з текстом — вона й блимає

    // Нижче плашки — нерухомі пояснення: що робити і що при цьому ще працює.
    //  Крок і відступ підібрані так, щоб УСІ ТРИ рядки лишались вище смуги
    //  статусу після того, як плашка підросла під перенос (див. PSU_PLATE_H).
    //  Третій рядок — «читання пам'яті працює» — саме той, який заспокоює
    //  користувача, що пристрій не помер; втратити його було б найгірше.
    int y = PSU_PLATE_Y + PSU_PLATE_H + 16;
    tSet(FONT_BODY, C_TEXT, C_BG);
    auto row = [&](const char *txt, uint16_t col) {
        if (y > FOOT_Y - 6) return;
        tSet(FONT_BODY, col, C_BG);
        tPut(EDGE, y, txt);
        y += 17;
    };
    row(st == PSU_ABSENT ? "Під'єднайте блок живлення."
      : st == PSU_LOW    ? "Потрібен 14 В під струмом."
                         : "Зніміть завищений блок.", C_TEXT);
    row("ЗАРЯД НЕМОЖЛИВИЙ.", C_RED);
    row("Читання пам'яті працює.", C_MUTED);

    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_YELLOW);
    tSet(FONT_SMALL, C_MUTED, C_CARD);
    tPut(EDGE, TFT_H - 8, "будь-яка кнопка — сховати");
}

// ── ПОВНОЕКРАННЕ ПОВІДОМЛЕННЯ ПО КОМБІНАЦІЇ ───────────────────────────────
//  Займає весь екран, тримається COMBO_FLASH_MS і знімається сама або першою
//  ж кнопкою. Малюється НАЙПЕРШОЮ — вище за все інше: у неї немає стану, який
//  можна пропустити, і живе вона п'ять секунд.
inline void drawPageFlash() {
    tft.fillScreen(C_BG);
    displayScreenCleared();
    const char *s = "Ляшко ЛОХ";
    tSet(FONT_HDR, C_YELLOW, C_BG);
    int w = tWidth(s);
    int x = (TFT_W - w) / 2; if (x < 0) x = 0;
    tPut(x, TFT_H / 2 + 6, s);
}

// ── МЕНЮ: СПИСОК УСЬОГО, ЩО ПРИСТРІЙ УМІЄ ─────────────────────────────────
//  Замість колишньої «сторінки Дій» — однієї картки з кільцем на три десятки
//  пунктів, яке гортається лише вперед, — вертикальний СПИСОК із групами.
//  Видно кілька сусідніх пунктів одразу, тобто видно, де ти в списку і що
//  поруч; курсор ходить в обидва боки; довге натискання перестрибує групу.
//
//  Назва ГРУПИ — у шапці, а не окремим рядком усередині списку. Так усі рядки
//  однакової висоти (проста арифметика вікна прокрутки), не витрачається
//  рядок екрана, і зміна групи одразу видно в шапці під час стрибка.
//
//  Опис обраного пункту — унизу, під розділювачем: у списку видно ЩО, під ним
//  — подробиці й КУДИ пише (DS2433 — ідентичність, DS2438 — монітор; сплутати
//  їх коштує або моделі, або заводського калібрування струму).
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
    drawHeaderBar((gname && *gname) ? gname : "МЕНЮ", cnt);

    // Геометрія рахується від панелі, а не зашита числами: на 135x240 рядок
    // вужчий і нижчий, на 240x320 рядків просто більше.
    const int ROW_H  = (FONT_BODY_W >= 9) ? 18 : 14;
    const int DESC_H = (FONT_BODY_W >= 9) ? 62 : 50;
    int listTop = HDR_H + 4;
    int listBot = FOOT_Y - DESC_H;
    int rows    = (listBot - listTop) / ROW_H;
    if (rows < 3) rows = 3;
    if (rows > total) rows = total;

    // Вікно прокрутки: курсор тримаємо в середині, поки список це дозволяє.
    int first = g_menuSel - rows / 2;
    if (first > total - rows) first = total - rows;
    if (first < 0) first = 0;

    const int BARW = 3;                       // смужка небезпеки ліворуч
    int x0 = EDGE, w = TFT_W - 2 * EDGE - 4;  // 4 — під смужку прокрутки
    int maxG = (w - BARW - 14) / FONT_BODY_W;

    for (int r = 0; r < rows; r++) {
        int i = first + r, y = listTop + r * ROW_H;
        const char *n2, *a, *b; uint8_t d2, c2; char b2[OP_NAME_BUF];
        menuInfo(i, &n2, &a, &b, &d2, &c2, b2, sizeof(b2));
        uint16_t accent = (d2 == OPD_WIPE) ? C_RED
                        : (d2 == OPD_WRITE) ? C_YELLOW : C_GREEN;
        bool cur = (i == g_menuSel);
        tft.fillRect(x0, y, w, ROW_H, cur ? C_CARD : C_BG);
        tft.fillRect(x0, y + 1, BARW, ROW_H - 2, accent);
        char fit[OP_NAME_BUF + 4];
        txtFit(fit, sizeof(fit), n2, maxG);
        tSet(FONT_BODY, cur ? C_TEXT : C_MUTED, cur ? C_CARD : C_BG);
        tPut(x0 + BARW + 10, y + ROW_H - 4, fit);
    }
    // Смужка прокрутки: без неї на списку з трьох десятків пунктів не видно,
    // чи ти на початку, чи вже під кінець.
    int sbx = TFT_W - EDGE - 3, sbh = rows * ROW_H;
    tft.fillRect(sbx, listTop, 3, sbh, C_CARD);
    int kh = sbh * rows / total; if (kh < 6) kh = 6;
    int ky = listTop + (total > rows ? (sbh - kh) * first / (total - rows) : 0);
    tft.fillRect(sbx, ky, 3, kh, C_BLUE);

    // ── опис обраного ─────────────────────────────────────────────────────
    const int DL = (FONT_BODY_W >= 9) ? 12 : 10;
    int dy = listBot + 2;
    tft.fillRect(0, dy, TFT_W, FOOT_Y - dy, C_BG);
    tft.drawFastHLine(EDGE, dy, TFT_W - 2 * EDGE, C_CARD);
    tSet(FONT_SMALL, C_MUTED);
    tPut(x0, dy + DL, l1);
    tPut(x0, dy + 2 * DL, l2);
    // Передостанній рядок — про наслідки: що зробить натискання і КУДИ пише.
    // Саме тут, а не в підказці внизу: підказка одна на все меню, а наслідки в
    // кожного пункту свої, і плутанина DS2433/DS2438 коштує дорого.
    char tail[48];
    if (danger == OPD_WIPE)
        snprintf(tail, sizeof(tail), "НЕЗВОРОТНЬО! трим.OK %s", opChipsShort(chips));
    else if (kind == MI_PAGE)
        snprintf(tail, sizeof(tail), "OK - відкрити");
    else if (danger == OPD_SAFE)
        snprintf(tail, sizeof(tail), "OK - перемкнути");
    else if (code == OP_CELLSWAP)
        snprintf(tail, sizeof(tail), "трим.OK = ПУСК, далі на ЗП");
    else
        snprintf(tail, sizeof(tail), "трим.OK = ПУСК  %s", opChipsShort(chips));
    tSet(FONT_SMALL, (danger == OPD_WIPE) ? C_RED
                   : (danger == OPD_SAFE) ? C_GREEN : C_YELLOW);
    tPut(x0, dy + 3 * DL + 2, tail);
    // Останній — навігація. Вона однакова скрізь, тож найдрібнішим і сірим.
    tSet(FONT_SMALL, C_MUTED);
#ifdef MENU_BTN3_PIN
    tPut(x0, dy + 4 * DL + 2, "[<][>] пункт, довго - група");
#else
    tPut(x0, dy + 4 * DL + 2, "[>] пункт  [<] вибір/пуск");
#endif

    // Футер лишається СТАТУСОМ, а не підказкою: саме через нього прилітає
    // «ЦІЛЬ 95%» і «ВИКОНУЮ...» — зворотний зв'язок про щойно натиснуте.
    drawFooterBar();
}

// Сторінка МОНІТОРИНГУ РОЗРЯДУ. Показується автоматично, поки навантаження
// увімкнене, і має пріоритет над гортанням меню: розряд — довга операція, під
// час якої на екрані має бути видно все службове, що змінюється.
// ═══════ СПІЛЬНИЙ КАРКАС ЕКРАНІВ ОПЕРАЦІЙ (заряд / розряд / пробудження) ══
//  Скарга власника: «зроби для заряду, розряду й оживлення однотипний вигляд».
//  Було три окремі верстки, які лише СХОЖІ одна на одну — і розходились на
//  кожній правці: у розряду «ціль … (%)», у заряду те саме з іншим
//  розрахунком, а пробудження взагалі жило вкладеною гілкою всередині заряду
//  й писало «тримаємо …». Тепер каркас ОДИН, а сторінки лише наповнюють його
//  рядками в однаковому порядку:
//
//      напруга (великим) -> шкала -> ціль -> режим і фаза -> струм ->
//      уставка/ШІМ -> ємність -> температура -> час і СКІЛЬКИ ЩЕ ЛИШИЛОСЬ.
//
//  Позиція рядка тримається у файловій змінній, а не в лямбді: помічники
//  мусять бути звичайними функціями, щоб їх кликали всі три сторінки.
static int g_monY = 0;

inline void opMonFrame(const char *title, uint16_t mv) {
    drawHeaderBar(title);
    char b[40];
    tft.fillRect(0, HDR_H + 4, TFT_W, 40, C_BG);
    snprintf(b, sizeof(b), "%u.%02u В", mv / 1000, (mv % 1000) / 10);
    tSet(FONT_MODEL, chargeColor(impresPercentFromMv(mv)));
    tPut(EDGE, HDR_H + 36, b);

    // Шкала — та сама анімована іконка, що й на головній сторінці.
    // ⚑ Смугу НЕ затираємо: саме затирання й «скидало» анімацію на кожному
    //  опитуванні. drawBatteryBar() чистить власний слід сам і лише тоді, коли
    //  графічний рівень справді змінився.
    const char *csrc; int pct = batteryPercent(&csrc);
    int by = HDR_H + 44, bh = 22;
    drawBatteryBar(EDGE, by, TFT_W - 2 * EDGE - 6, bh, pct, battFillColor(pct));
    g_pctTx = g_pctTy = g_pctTw = g_pctTh = 0;     // цифр усередині шкали немає
    g_monY = by + bh + 18;
}

// Черговий рядок показань. Кожен сам чистить свою смужку на всю ширину, тож
// оновлення раз на секунду не блимає і старий текст не «просвічує». Рядки, що
// не влізли до підвалу, просто не малюємо: на 240×240 місця менше, і краще
// втратити останній рядок, ніж заїхати текстом на статус.
inline void opMonRow(const char *txt, uint16_t col) {
    if (g_monY > FOOT_Y - 4) return;
    tft.fillRect(0, g_monY - 12, TFT_W, 16, C_BG);
    tSet(FONT_BODY, col);
    tPut(EDGE, g_monY, txt);
    g_monY += 18;
}

// «Скільки ще лишилось» у компактному вигляді. Порожній рядок = невідомо.
inline void opMonEtaText(uint32_t etaS, char *out, size_t n) {
    if (!etaS)             { snprintf(out, n, "—"); return; }
    if (etaS < 60)         { snprintf(out, n, "<1 хв"); return; }
    if (etaS < 3600)       { snprintf(out, n, "%lu хв", (unsigned long)(etaS / 60)); return; }
    snprintf(out, n, "%lu год %lu хв", (unsigned long)(etaS / 3600),
             (unsigned long)((etaS % 3600) / 60));
}

// Час і залишок — ОДНИМ рядком і однаково в усіх трьох режимах.
//  etaS == 0 означає «оцінити не можна», і тоді ми так і пишемо: показати
//  вигадане число гірше, ніж не показати жодного, бо на нього розраховують.
inline void opMonTime(uint32_t elapsedS, uint32_t etaS) {
    char b[56], e[24];
    opMonEtaText(etaS, e, sizeof(e));
    unsigned long el = elapsedS;
    snprintf(b, sizeof(b), "%lu:%02lu:%02lu · ще %s%s",
             el / 3600, (el / 60) % 60, el % 60, etaS ? "≈" : "", e);
    opMonRow(b, C_TEXT);
}

inline void opMonFoot(bool running, bool aborted, const char *text) {
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    tSet(FONT_SMALL, aborted ? C_RED : C_MUTED, C_CARD);
    tPut(EDGE, TFT_H - 8, running ? "[OK] тримати = ЗУПИНИТИ" : text);
}

inline void drawPageDischarge() {
    const DischargeState &d = g_dis;
    opMonFrame("РОЗРЯД", d.lastMv);
    char b[56];

    // Прогрес до цілі — числом у рядку «ціль».
    int span = (int)d.startMv - (int)d.targetMv;
    int done = (int)d.startMv - (int)d.lastMv;
    int pct  = (span > 0) ? (done * 100 / span) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    snprintf(b, sizeof(b), "ціль %u.%02u В  (%d%%)",
             d.targetMv / 1000, (d.targetMv % 1000) / 10, pct);
    opMonRow(b, C_TEXT);

    // Режим і фаза — на тому самому місці, що й у заряді.
    snprintf(b, sizeof(b), "режим %s · %s%s",
             dischargeProfileShort(dischargeProfile()),
             dischargePhaseShort(d.phase),
             dischargeManualMa() ? " · руч." : "");
    opMonRow(b, d.phase == DIS_PH_HOLD ? C_YELLOW : C_MUTED);

    // Струм і потужність — із ВБУДОВАНОГО датчика DS2438 (його резистор стоїть
    // усередині пакета послідовно з банками). Показуємо СЕРЕДНІЙ струм: ключ
    // працює ШІМом, тож саме він і тече, а не виміряний пік.
    int wx10 = dischargeWattsX10(d.lastMv, d.lastMa);
    snprintf(b, sizeof(b), "струм %d мА · %d.%d Вт", d.lastMa, wx10 / 10, wx10 % 10);
    opMonRow(b, (d.state == DIS_RUN && !dischargeInBand(d)) ? C_YELLOW : C_TEXT);

    if (dischargePwmOk()) {
        snprintf(b, sizeof(b), "уст %u · ШІМ %u%% · пік %u", d.setMa, d.dutyPct, d.peakMa);
        opMonRow(b, C_MUTED);
    } else {
        opMonRow("БЕЗ ШІМ: не обмежено!", C_RED);
    }

    snprintf(b, sizeof(b), "віддано %lu мА·год", (unsigned long)dischargeMah());
    opMonRow(b, C_GREEN);
    snprintf(b, sizeof(b), "DCA %lu мА·год · ICA %u",
             (unsigned long)dischargeDcaMah(), d.lastIca);
    opMonRow(b, C_MUTED);

    snprintf(b, sizeof(b), "темп. %d.%d °C", d.lastTempC10 / 10, abs(d.lastTempC10 % 10));
    opMonRow(b, d.lastTempC10 >= DISCHARGE_MAX_TEMP_C * 10 - 50 ? C_RED : C_TEXT);

    opMonTime(d.elapsedS,
              dischargeEtaS(impresPercentFromMv(d.lastMv),
                            impresPercentFromMv(d.targetMv),
                            dischargeRatedMah(),
                            (uint16_t)(d.lastMa < 0 ? -d.lastMa : d.lastMa)));

    opMonFoot(d.state == DIS_RUN, d.state == DIS_ABORT,
              (d.state == DIS_DONE) ? "ГОТОВО -> на IMPRES-ЗП"
                                    : dischargeReasonText(d.reason));
}

// Сторінка МОНІТОРИНГУ ЗАРЯДУ — той самий каркас, що й розряд вище.
//  ⚑ ПРОБУДЖЕННЯ — ОКРЕМА СТОРІНКА, А НЕ ГІЛКА ВСЕРЕДИНІ ЗАРЯДУ. Половина
//  рядків заряду в ньому була б вигадкою: ціль у відсотках, CCA, ICA й
//  температура беруться з DS2438, а він мовчить — у цьому вся суть режиму.
//  Показувати «темп. 0.0 °C» і «CCA 0» означало б видавати відсутність даних
//  за дані. Каркас при цьому спільний, тож виглядають вони однаково.
inline void drawPageWake() {
    const ChargeState &c = g_chg;
    opMonFrame("ПРОБУДЖЕННЯ", c.lastMv);
    char b[56];

    snprintf(b, sizeof(b), "тримаємо %u.%02u В",
             c.targetMv / 1000, (c.targetMv % 1000) / 10);
    opMonRow(b, C_TEXT);
    snprintf(b, sizeof(b), "режим пробудження · %u с", (unsigned)CHARGE_WAKE_MAX_S);
    opMonRow(b, C_MUTED);
    snprintf(b, sizeof(b), "струм %d мА зі стелі %u", c.lastMa, (unsigned)CHARGE_WAKE_MA);
    opMonRow(b, C_TEXT);
    if (chargePwmOk()) {
        snprintf(b, sizeof(b), "ШІМ %u%% (межа %u з %u)", chargeDutyPct(),
                 chargeDutyCap(), (unsigned)CHARGE_DUTY_FULL);
        opMonRow(b, C_MUTED);
    } else {
        opMonRow("БЕЗ КЕРУВАННЯ: перевірте!", C_RED);
    }
    snprintf(b, sizeof(b), "віддано %lu з %u мА·год",
             (unsigned long)chargeMah(), (unsigned)CHARGE_WAKE_MAH_MAX);
    opMonRow(b, C_GREEN);
    snprintf(b, sizeof(b), "проб %u · %s", c.wakeProbes,
             chargeWakeGoalText(c.wakeGoal, c.reason));
    opMonRow(b, chargeReasonIsDone(c.reason) ? C_GREEN : C_MUTED);

    // ⚑ ТУТ ЗАЛИШОК ТОЧНИЙ, А НЕ ОЦІНКА: режим обмежений часом жорстко
    //  (CHARGE_WAKE_MAX_S), тож лишилось рівно стільки, скільки не минуло.
    uint32_t left = (c.elapsedS < (uint32_t)CHARGE_WAKE_MAX_S)
                  ? ((uint32_t)CHARGE_WAKE_MAX_S - c.elapsedS) : 0;
    opMonTime(c.elapsedS, (c.state == CHG_RUN) ? left : 0);

    opMonFoot(c.state == CHG_RUN, c.state == CHG_ABORT,
              (c.state == CHG_DONE) ? "ГОТОВО" : chargeReasonText(c.reason));
}

inline void drawPageCharge() {
    if (chargeWakeShown()) { drawPageWake(); return; }
    const ChargeState &c = g_chg;
    opMonFrame("ЗАРЯД", c.lastMv);
    char b[56];

    // ЖИВЛЕННЯ — найпершим рядком і тільки коли з ним негаразд. Без блока
    // заряд не піде взагалі, тож ховати причину нижче за наслідки не можна.
    if (chargePsuFault()) {
        snprintf(b, sizeof(b), "БЖ %u.%02u В — %s", chargePsuMv() / 1000,
                 (chargePsuMv() % 1000) / 10,
                 chargePsuState() == PSU_ABSENT ? "НЕМАЄ ЖИВЛЕННЯ" :
                 chargePsuState() == PSU_LOW    ? "НАПРУГА ЗАНИЖЕНА" : "НАПРУГА ЗАВИЩЕНА");
        opMonRow(b, C_RED);
    }

    snprintf(b, sizeof(b), "ціль %u.%02u В (%u%%)",
             c.targetMv / 1000, (c.targetMv % 1000) / 10, c.targetPct);
    opMonRow(b, C_TEXT);

    snprintf(b, sizeof(b), "режим %s · %s%s",
             chargeProfileShort(chargeProfile()), chargePhaseShort(c.phase),
             chargeManualMa() ? " · руч." : "");
    opMonRow(b, c.phase == CHG_PH_HOLD ? C_YELLOW : C_MUTED);

    // Струм і потужність — із ВЛАСНОГО шунта пристрою, а не з DS2438: монітор
    // пакета читається лише на закритому ключі й дає температуру, не струм.
    int wx10 = chargeWattsX10(c.lastMv, c.lastMa);
    snprintf(b, sizeof(b), "струм %d мА · %d.%d Вт", c.lastMa, wx10 / 10, wx10 % 10);
    opMonRow(b, C_TEXT);

    // Вершина пульсацій злітає, коли дросель фактично випав із кола, а середнє
    // це ще приховує — тому пік показуємо окремо.
    if (chargePwmOk()) {
        snprintf(b, sizeof(b), "уст %u мА · пік %u мА · %u%%",
                 c.setMa, c.peakMa, chargeDutyPct());
        opMonRow(b, C_MUTED);
    } else {
        opMonRow("БЕЗ КЕРУВАННЯ: перевірте!", C_RED);
    }

    snprintf(b, sizeof(b), "отримано %lu мА·год", (unsigned long)chargeMah());
    opMonRow(b, C_GREEN);
    snprintf(b, sizeof(b), "CCA %lu мА·год · ICA %u",
             (unsigned long)chargeCcaMah(), c.lastIca);
    opMonRow(b, C_MUTED);

    snprintf(b, sizeof(b), "темп. %d.%d °C", c.lastTempC10 / 10, abs(c.lastTempC10 % 10));
    opMonRow(b, c.lastTempC10 >= CHARGE_MAX_TEMP_C * 10 - 50 ? C_RED : C_TEXT);

    opMonTime(c.elapsedS,
              chargeEtaS(c.phase, c.lastPct, c.targetPct, chargeRatedMah(), c.lastMa));

    opMonFoot(c.state == CHG_RUN, c.state == CHG_ABORT,
              (c.state == CHG_DONE) ? "ГОТОВО" : chargeReasonText(c.reason));
}

// Сторінка Майстра відновлення (кольорова). Дані готує wizDeviceRefresh().
inline void drawPageWizard() {
    drawHeaderBar("Майстер відновлення");
    int x = EDGE + 4;
    if (g_wizBusy) {
        tSet(FONT_MODEL, C_YELLOW);
        tPut(x, 92, "Виконую крок...");
    } else if (g_wizProblems < 0) {
        tSet(FONT_BODY, C_TEXT);
        tPut(x, 82, "Аналіз стану АКБ.");
        tSet(FONT_SMALL, C_MUTED);
#ifdef MENU_BTN3_PIN
        tPut(x, 110, "[OK] аналіз");
#else
        tPut(x, 110, "[<] коротко = аналіз");
#endif
    } else if (g_wizHealthy) {
        tSet(FONT_MODEL, C_GREEN);
        tPut(x, 90, "OK: справна");
        tSet(FONT_BODY, C_MUTED);
        tPut(x, 120, "Відновлення не потрібне.");
    } else {
        char b[28]; snprintf(b, sizeof(b), "Проблем: %d", g_wizProblems);
        tSet(FONT_MODEL, C_RED);
        tPut(x, 80, b);
        tSet(FONT_BODY, C_TEXT);
        tPut(x, 106, g_wizTop);
        if (g_wizAwait) {
            tSet(FONT_BODY, C_YELLOW);
            tPut(x, 136, "Чекаю ЗП. Поверніть АКБ");
#ifdef MENU_BTN3_PIN
            tPut(x, 158, "і тримайте [OK].");
#else
            tPut(x, 158, "і тримайте [<].");
#endif
        } else {
            tSet(FONT_BODY, C_GREEN);
            char n[48]; snprintf(n, sizeof(n), "Далі: %s", g_wizNext);
            tPut(x, 136, n);
            tSet(FONT_SMALL, C_MUTED);
            char p[24]; snprintf(p, sizeof(p), "Крок %d/%d", g_wizProg + 1, g_wizTotal);
            tPut(x, 160, p);
        }
    }
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    tSet(FONT_SMALL, C_MUTED, C_CARD);
#ifdef MENU_BTN3_PIN
    tPut(EDGE, TFT_H - 8, "[<][>] меню   [OK] аналіз/крок");
#else
    tPut(EDGE, TFT_H - 8, "[<] кор=аналіз   трим=крок");
#endif
}

// ===================== Рендер і кнопки =====================

// Без миготіння: НІКОЛИ не робимо fillScreen (він гасить весь екран у чорне і
// дає спалах при перелистуванні). Завжди чистимо лише ТІЛО між шапкою і
// статус-смугою; шапка й статус перемальовуються самі поверх себе тим самим
// кольором, тож візуально не блимають. При зміні сторінки міняється лише вміст
// тіла (і заголовок) — саме те, що реально змінилось.
// clearBody=true — очистити тіло в чорне перед малюванням (для зміни сторінки).
// clearBody=false — перемалювати ТУ Ж сторінку поверх наявної (елементи опуклі й
// перекривають старі; фон і так чорний). Без чорного «спалаху» — для зміни лише
// відтінку (червоний фільтр помилки), щоб екран НЕ блимав.
inline void displayRenderBody(bool clearBody) {
    // Очищення тіла стирає й шкалу батареї, тож кеш «уже намальовано» після
    // нього брехав би: стан той самий, а пікселів немає.
    if (clearBody) { tft.fillRect(0, HDR_H, TFT_W, FOOT_Y - HDR_H, C_BG);
                     displayScreenCleared(); }
    if (comboFlashActive(g_flash, millis())) { drawPageFlash(); return; }
    // Поки навантаження увімкнене — примусово показуємо моніторинг розряду,
    // хоч би яку сторінку було обрано: це довга операція із запобіжниками, її
    // стан має бути на екрані завжди, а не за кілька натискань кнопки.
    // Заряд і розряд не можуть іти одночасно, тож порядок цих двох перевірок
    // не має значення.
    if (dischargeScreenActive()) { drawPageDischarge(); return; }
    if (chargeScreenActive())    { drawPageCharge();    return; }
    // Помилка живлення — НИЖЧЕ за операції, що йдуть: розряд блока живлення не
    // потребує взагалі, а зупинений через живлення заряд і так показує ту саму
    // причину на своїй сторінці. Перебивати роботу, що триває, було б гірше,
    // ніж почекати — «!» у шапці нікуди не дівається.
    if (chargePsuScreenActive()) { drawPagePsuFault(); return; }
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
}
inline void displayRender() { displayRenderBody(true); }

// ── БЛИМАННЯ НАПИСУ ПРО ЖИВЛЕННЯ ──────────────────────────────────────────
//  Кличеться часто з loop(). Поза сторінкою помилки — миттєво виходить.
static bool          g_psuBlinkOn = true;
static unsigned long g_psuBlinkMs = 0;

// Зняти повноекранне повідомлення, коли його час вийшов. Окремим завданням із
// loop(), як і блимання: сама по собі комбінація нічого більше не робить, і
// чекати наступного натискання, щоб прибрати напис, було б дивно.
inline void displayFlashTask() {
    if (comboFlashExpired(g_flash, millis())) { displayScreenCleared(); displayRender(); }
}

inline void displayPsuBlinkTask() {
    if (!chargePsuScreenActive()) { g_psuBlinkOn = true; return; }
    unsigned long now = millis();
    if (now - g_psuBlinkMs < DISPLAY_PSU_BLINK_MS) return;
    g_psuBlinkMs = now;
    g_psuBlinkOn = !g_psuBlinkOn;
    drawPsuPlate(g_psuBlinkOn);               // блимає ПЛАШКА, текст усередині стоїть
}

// Оновлення екрана під час РОЗРЯДУ. full=false — перемальовуємо лише рядки
// показань (кожен сам чистить свою смужку), тож картинка не блимає раз на 5 с.
// full=true — повна перемальовка: потрібна на вході в режим і на виході з нього,
// інакше поверх моніторингу лишаються написи попередньої сторінки.
inline void displayDischargeRefresh(bool full) {
    if (full) displayRender(); else displayRenderBody(false);
}
// Те саме, для ЗАРЯДУ.
inline void displayChargeRefresh(bool full) {
    if (full) displayRender(); else displayRenderBody(false);
}

// Освітлити колір RGB565 у бік білого на частку amt (0..255).

// Масштаб яскравості кольору RGB565: lvl 0..255 (255 = без змін, менше = темніше).
inline uint16_t scale565(uint16_t c, uint8_t lvl) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = r * lvl / 255; g = g * lvl / 255; b = b * lvl / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Заповнити прямокутник R кольором col, ОМИНАЮЧИ рамку тексту T (перетин R∩T не
// малюємо) — до 4 смуг. Так пульсуємо все заповнення, не торкаючись цифр %.

// Тік анімації батареї (з loop() ~9 разів/с). ПУЛЬСАЦІЯ ЗАПОВНЕННЯ: усе залите
// поле шкали плавно «дихає» яскравістю (колір заряду <-> світліший відтінок),
// оминаючи рамку цифр % (щоб текст не блимав). Період ~3.5 с — м'яко.
// Синус 0..255 (32 точки) для «плавучого градієнта» заповнення.
static const uint8_t ANIM_SINE32[32] = {
  128,152,176,198,218,234,246,254,255,254,246,234,218,198,176,152,
  128,103, 79, 57, 37, 21,  9,  1,  0,  1,  9, 21, 37, 57, 79,103};

inline void displayAnimTick() {
    // Анімуємо на головній сторінці та на сторінці РОЗРЯДУ (там показник заряду
    // такий самий, і статична шкала під час довгої операції виглядала б як
    // «завис»).
    if (!(g_displayPage == 0 || dischargeScreenActive() || chargeScreenActive()) || g_battW == 0) return;
    // ⚑ АНІМУЄМО ЛИШЕ ТУ ШКАЛУ, ЩО СПРАВДІ ЗАРАЗ НА ЕКРАНІ.
    //  Скарга власника: «при помилці живлення поверх попередження малюється
    //  індикатор заряду». Так і було. Сторінка помилки живлення виводиться
    //  ПОВЗ звичайний перемикач сторінок (chargePsuScreenActive() перехоплює
    //  малювання першим), а g_displayPage при цьому лишається нулем — тобто
    //  «головна». Анімація дивилась саме на нього, бачила «головна», брала
    //  ЗАПАМ'ЯТОВАНІ координати шкали й ~9 разів на секунду клала градієнт
    //  просто поверх червоної плашки з попередженням.
    //
    //  Перелічувати тут сторінки-перехоплювачі — програшна гра: наступна така
    //  сторінка знову про це не знатиме. Тому питання ставиться інакше й
    //  однозначно: чи та шкала, координати якої ми пам'ятаємо, намальована на
    //  ЦЬОМУ екрані. Кожне очищення екрана піднімає лічильник поколінь
    //  (displayScreenCleared), а drawBatteryBar() запам'ятовує покоління, у
    //  якому малював. Розійшлись — на екрані вже інша сторінка, і чіпати його
    //  анімації нема чого.
    if (g_battDrawn.gen != g_screenGen) return;
    if (g_errTint) return;              // під час оповіщення про помилку — статичний
                                        // червоний екран (без руху градієнта)
    if (g_ledMode == LED_READ || g_ledMode == LED_WRITE)
        return;                         // під час операції (читання/запис) — екран
                                        // статичний, без руху/блимання градієнта
    // ⚑ БЕРЕМО ТЕ, ЩО СПРАВДІ НАМАЛЬОВАНО, а не рахуємо заново. Раніше тут
    //  був власний перерахунок із batteryPercent(), тобто ДРУГЕ джерело тих
    //  самих чисел. Поки воно збігалося з тим, що намалювала сторінка, все
    //  виглядало гаразд; варто було б їм розійтися на піксель — і анімація
    //  почала б малювати градієнт іншої довжини, ніж рамка, тобто вилазити за
    //  заповнення або лишати смужку рівного кольору з краю.
    if (g_battDrawn.fw < 4) return;          // немає даних або смужка надто вузька
    int fw = g_battDrawn.fw;
    uint16_t col = g_battDrawn.col;
    int fx = g_battX + 3, fy = g_battY + 3, fh = g_battH - 6;
    int tx0 = g_pctTx, tx1 = g_pctTx + g_pctTw;   // рамка цифр % (оминаємо)
    int ty0 = g_pctTy, ty1 = g_pctTy + g_pctTh;
    // Напрямок «течії» градієнта: під час ЗАРЯДУ — у бік зростання (як
    // заповнення прибуває), під час РОЗРЯДУ/спокою — у бік спадання (як
    // заповнення витікає). Без цього градієнт завжди «тік» в один бік і під
    // час заряду читався як «розряджається».
    if (chargeScreenActive()) g_animPhase -= ANIM_GRADIENT_SPEED;
    else                      g_animPhase += ANIM_GRADIENT_SPEED;

    // Малюємо заповнення вертикальними смугами по 4 px, яскравість кожної —
    // за синусом від позиції + фази. Виходить світло-темний градієнт, що плавно
    // біжить уздовж шкали (набагато помітніше за рівномірну пульсацію). Смуги над
    // цифрами % ділимо на верх/низ, щоб не зачепити текст.
    const int band = 4;
    for (int x = fx; x < fx + fw; x += band) {
        int w = (x + band <= fx + fw) ? band : (fx + fw - x);
        // Індекс синуса: позиція, масштабована під довжину хвилі, + фаза (тече).
        uint8_t s = ANIM_SINE32[(((x - fx) * 32 / ANIM_GRADIENT_WAVELEN) + g_animPhase) & 31];
        uint8_t lvl = (uint8_t)(ANIM_GRADIENT_MINLVL +
                                (int)s * (255 - ANIM_GRADIENT_MINLVL) / 255);
        uint16_t c = scale565(col, lvl);
        bool overText = (x + w > tx0) && (x < tx1) && (ty1 > fy) && (ty0 < fy + fh);
        if (overText) {
            if (ty0 > fy)      tft.fillRect(x, fy, w, ty0 - fy, c);       // над рамкою
            if (fy + fh > ty1) tft.fillRect(x, ty1, w, fy + fh - ty1, c); // під рамкою
        } else {
            tft.fillRect(x, fy, w, fh, c);
        }
    }
}

// Плавний вхід у головне меню: малюємо головну сторінку під вимкненою
// підсвіткою і плавно піднімаємо ШІМ до повної яскравості.
inline void displayFadeInMain() {
    g_displayPage = 0;
#ifdef DISPLAY_BLK_PIN
    // Заставка ще світиться (255). Робимо ОДИН плавний «дип»: пригашуємо, під
    // приглушеним світлом перемальовуємо на головне меню (щоб не було видно
    // самого перемальовування), і плавно повертаємо повну яскравість. Жодного
    // повного затемнення й другого спалаху — м'який кросфейд заставка -> меню.
    for (int b = 255; b >= 40; b -= 12) { analogWrite(DISPLAY_BLK_PIN, b); delay(12); }
    tft.fillScreen(C_BG);
    displayScreenCleared();
    displayRender();
    for (int b = 40; b <= 255; b += 10) { analogWrite(DISPLAY_BLK_PIN, b); delay(12); }
    analogWrite(DISPLAY_BLK_PIN, 255);
#else
    tft.fillScreen(C_BG);
    displayScreenCleared();
    displayRender();
#endif
}

inline void displaySetStatus(const char *s) {
    strncpy(g_displayStatus, s, sizeof(g_displayStatus) - 1);
    g_displayStatus[sizeof(g_displayStatus) - 1] = '\0';
}

// Оновлення статусу операції ("ЗАПИС...", "OK", "ЗБІЙ" тощо). Оновлює ЛИШЕ
// смугу статусу знизу — БЕЗ перемальовки тіла сторінки. Тіло displayRender()
// щоразу гасить у чорне (fillRect), тож кілька таких оновлень під час операції
// давали «блимання». Оновлюючи лише футер, прибираємо блимання; тіло (батарея/
// деталі) лишається на місці, а нові дані показуються при наступному повному
// перемальовку (навігація / після читання — див. loop()).
inline void displayShow(const char *s) {
    displaySetStatus(s);
    // ⚑ У МЕНЮ оновлюємо ще й тіло. Назви частини пунктів залежать від стану
    //  («Ціль заряду 100%» -> «...95%»), і саме після натискання, коли статус
    //  каже «ЦІЛЬ 95%», список показував би стару ціль — тобто дві різні
    //  відповіді на одне натискання. Тіло перемальовується БЕЗ очищення в
    //  чорне, тож блимання немає, а drawPageMenu наприкінці сам малює футер
    //  зі щойно виставленим статусом.
    if (g_displayPage == PAGE_MENU) { displayRenderBody(false); return; }
    drawFooterBar();
}

// Увімкнути/вимкнути червоний «світлофільтр» на час оповіщення про помилку.
// Один перемальовок при зміні стану — екран стає (чи перестає бути) червоним,
// БЕЗ блимання. Викликається з loop() за станом індикатора (LED_ERROR).
inline void displaySetErrorTint(bool on) {
    if (g_errTint == on) return;
    g_errTint = on;
    // Відтінок міняє ВСЮ палітру, тож те, що намальовано, більше не відповідає
    // кешу шкали батареї. Колір заливки й так зміниться (chargeColor піде в
    // червоне) і кеш це впіймає, але рамка малюється C_TEXT — а її кеш не
    // стереже. Найпростіше й найнадійніше — оголосити покоління екрана новим.
    displayScreenCleared();
    // Перемальовуємо ТУ Ж сторінку БЕЗ очищення тіла в чорне (displayRenderBody
    // з clearBody=false): елементи перезаписуються на місці, змінюється лише
    // відтінок. Жодного чорного спалаху й блимання — просто екран стає (чи
    // перестає бути) червоним.
    displayRenderBody(false);
}

// Плавне керування яскравістю через ШІМ підсвітки (BLK). Якщо BLK-пін не
// заданий — no-op (підсвітка керується апаратно/завжди увімкнена).
inline void displaySetBrightness(uint8_t v) {
#ifdef DISPLAY_BLK_PIN
    analogWrite(DISPLAY_BLK_PIN, v);
#else
    (void)v;
#endif
}

// ── САМОПЕРЕВІРКА ЕКРАНА: розрізнити «немає підсвітки» і «немає обміну» ───
//  Вмикається DISPLAY_ST7789_SELFTEST у settings.h. Потрібна рівно тоді, коли
//  екран чорний: сама по собі чорнота нічого не каже — однаково виглядають і
//  згасла підсвітка, і мовчазна шина SPI. Тест заливає екран чистими кольорами
//  при ПОВНІЙ підсвітці, і відповідь читається очима, без осцилографа:
//    • бачите кольори       -> і підсвітка, і обмін працюють (шукайте далі: у
//                              верстці, орієнтації, оффсетах);
//    • екран світлий, але без кольорів -> підсвітка є, обміну немає
//                              (SDA/SCL/DC/RST, живлення панелі, CS);
//    • екран лишається темним -> не працює підсвітка (BLK, її живлення).
inline void displaySelfTest() {
#if defined(DISPLAY_ST7789_SELFTEST)
  #ifdef DISPLAY_BLK_PIN
    analogWrite(DISPLAY_BLK_PIN, 255);       // на час тесту — на повну
  #endif
    struct { uint16_t c; const char *n; } steps[] = {
        { 0xF800, "ЧЕРВОНИЙ" }, { 0x07E0, "ЗЕЛЕНИЙ" },
        { 0x001F, "СИНІЙ" },    { 0xFFFF, "БІЛИЙ" },
    };
    for (auto &s : steps) {
        tft.fillScreen(s.c);
        displayScreenCleared();
        Serial.printf("DISPLAY SELFTEST: %s\n", s.n);
        delay(700);
    }
    tft.fillScreen(C_BG);
    displayScreenCleared();
    Serial.println("DISPLAY SELFTEST: завершено. Бачили кольори — обмін і "
                   "підсвітка справні; світлий екран без кольорів — немає "
                   "обміну; темний — немає підсвітки.");
  #ifdef DISPLAY_BLK_PIN
    analogWrite(DISPLAY_BLK_PIN, 0);         // повертаємо як було до заставки
  #endif
#endif
}

inline void displayInit() {
#ifdef DISPLAY_BLK_PIN
    pinMode(DISPLAY_BLK_PIN, OUTPUT);
    analogWrite(DISPLAY_BLK_PIN, 0);          // підсвітка ВИМК до заставки —
                                              // ховаємо артефакти ініціалізації
#endif
    // ⚑ РЕЖИМ SPI передаємо ЯВНО. Раніше init() кликався без нього, тобто
    //  мовчки брав зашитий у бібліотеку режим 0 — а панелі без виводу CS
    //  (240x240 GMT130 і подібні) типово вимагають режим 3: CS у них
    //  припаяний до землі, контролер вибраний завжди, і межу посилки він
    //  визначає за станом такту в спокої. З «не тим» режимом біти приймаються
    //  зі зсувом, жодної команди панель не впізнає — і лишається чорною.
    //  Номер -> константа платформи тут, а не в settings.h: SPI_MODE0..3 на
    //  різних платформах мають різні значення, тож голе число передавати не
    //  можна (на AVR це 0x00/0x04/0x08/0x0C, на ESP32 — 0..3).
    tft.init(PANEL_W, PANEL_H, ST7789_SPI_MODE_CONST);   // рідні (портретні) розміри
    // ⚑ ЧАСТОТА теж явно. Типова в бібліотеки — десятки МГц: на доріжках
    //  плати це працює, на дротах-перемичках дає «сніг» або чорний екран.
    tft.setSPISpeed(DISPLAY_ST7789_SPI_HZ);
    tft.setRotation(DISPLAY_ST7789_ROT);
#if defined(ST7789_USE_OFFSET_CLASS)
    // Ручні оффсети пам'яті (для нестандартних панелей або якщо авто-зсув хибний).
    #ifndef PANEL_XOFF
      #define PANEL_XOFF 0
    #endif
    #ifndef PANEL_YOFF
      #define PANEL_YOFF 0
    #endif
    tft.applyOffsets(PANEL_XOFF, PANEL_YOFF);
#endif
#if defined(DISPLAY_ST7789_INVERT)
    tft.invertDisplay(true);                  // деякі панелі показують інверсно
#endif
    u8g2Fonts.begin(tft);
    u8g2Fonts.setFontMode(0);                 // НЕпрозорий: фон гліфа малюємо
                                              // під колір ділянки (без чорних ореолів)
    u8g2Fonts.setFontDirection(0);
    tft.fillScreen(C_BG);
    displayScreenCleared();
    displaySelfTest();                        // порожньо, доки не увімкнено
                                              // DISPLAY_ST7789_SELFTEST
    // ── ЗВІТ ПРО КОНФІГУРАЦІЮ ЕКРАНА ──────────────────────────────────────
    //  Друкуємо ВСЕ, що впливає на «чорний екран»: геометрію, орієнтацію,
    //  керуючі піни й те, чи використовується CS. Скарга «зображення немає»
    //  без цих чисел не діагностується взагалі — а з ними одразу видно, чи
    //  дійшла прошивка до ініціалізації і з якими саме параметрами.
    Serial.printf("DISPLAY: ST7789 %dx%d (panel %dx%d) color, rot=%d, "
                  "SPI mode=%d %.1f МГц, DC=%d RST=%d CS=%s BLK=%s\n",
                  (int)TFT_W, (int)TFT_H, (int)PANEL_W, (int)PANEL_H,
                  (int)DISPLAY_ST7789_ROT,
                  (int)DISPLAY_ST7789_SPI_MODE, DISPLAY_ST7789_SPI_HZ / 1000000.0,
                  (int)DISPLAY_DC_PIN, (int)DISPLAY_RST_PIN,
#ifdef DISPLAY_CS_PIN
                  String((int)DISPLAY_CS_PIN).c_str(),
#else
                  "немає (тягнеться до GND на модулі)",
#endif
#ifdef DISPLAY_BLK_PIN
                  String((int)DISPLAY_BLK_PIN).c_str()
#else
                  "немає (підсвітка на живленні)"
#endif
    );
}


// ---- Кнопки (та ж логіка, що й у монохромній версії) ----

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

struct BtnState {
    bool stable = HIGH;
    bool lastRaw = HIGH;
    unsigned long tChange = 0;
    unsigned long tPress = 0;
    bool longFired = false;
};

// Опитування кнопки за вже зчитаним сирим станом (true = натиснуто) — див.
// докладний коментар у display.h. Та сама антидребезгова логіка, лише
// відокремлена від того, ЯК саме зчитується «натиснуто».
inline int pollButtonRaw(bool pressed, BtnState &b, unsigned long longMs) {
    bool raw = pressed ? LOW : HIGH;
    unsigned long now = millis();
    if (raw != b.lastRaw) { b.lastRaw = raw; b.tChange = now; }
    int ev = 0;
    if (now - b.tChange > 25 && raw != b.stable) {
        b.stable = raw;
        if (b.stable == LOW) { b.tPress = now; b.longFired = false; }
        else { if (!b.longFired) ev = 1; }
    }
    if (b.stable == LOW && longMs && !b.longFired && now - b.tPress >= longMs) {
        b.longFired = true; ev = 2;
    }
    return ev;
}

#ifdef MENU_BTN_ADC_PIN
// Три кнопки на одному ADC-піні — див. докладний коментар у display.h.
// НЕ analogReadMilliVolts(): вона потребує апаратної eFuse-калібровки ADC,
// якої немає на частині клон-плат (ESP-IDF валить "default vref didn't
// set" і повертає невалідні мВ) — analogRead() калібрування не потребує.
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

inline bool displayConsumeReadRequest() {
    if (g_readRequested) { g_readRequested = false; return true; }
    return false;
}

inline int displayConsumeActionRequest() {
    int a = g_actionRequested; g_actionRequested = -1; return a;
}

// Запит Майстра для .ino: 0 нема, 1 аналіз, 2 наступний крок.
inline int displayConsumeWizRequest() { int r = g_wizReq; g_wizReq = 0; return r; }

// Плавний перехід між сторінками: короткий «дип» підсвітки (crossfade). Без
// BLK-піна — звичайний рендер без анімації.
inline void displayFlip() {
#ifdef DISPLAY_BLK_PIN
    for (int b = 255; b > 90; b -= 33) { analogWrite(DISPLAY_BLK_PIN, b); delay(5); }
    displayRender();
    for (int b = 90; b < 255; b += 33) { analogWrite(DISPLAY_BLK_PIN, b); delay(5); }
    analogWrite(DISPLAY_BLK_PIN, 255);
#else
    displayRender();
#endif
    // ⚑ Звук — В КІНЦІ, після всієї блокуючої роботи. Фразу веде buzzTask() із
    // loop(); поки триває «дип» підсвітки (12 × delay(5)) і displayRender() по
    // SPI, loop() стоїть, тік не приходить жодного разу — і 76-мілісекундний
    // бліп, запущений ДО переходу, просто протікав повз: перший же тік після
    // повернення бачив, що фраза скінчилась, і гасив вихід. Саме так виглядало
    // «бліп не працює».
    buzzClick();
}

// Натискання «OK» на пункті меню. Одна функція на всі роди пунктів — саме
// тому, що «що зробить OK» мусить залежати від ПУНКТА, а не від сторінки.
//
//  ⚑ ЧОМУ БЕЗПЕЧНЕ ЙДЕ КОРОТКИМ, А ЗАПИС — ДОВГИМ. Довге натискання тут — це
//  не прикраса, а єдиний бар'єр між «гортаю список» і «стер ідентичність».
//  Але той самий бар'єр стояв і перед «Ціль заряду», яка нічого не пише й
//  існує рівно для того, щоб її натискали по колу, — тримати кнопку заради
//  зміни числа безглуздо. Тому бар'єр тепер за ознакою НАСЛІДКУ (OPD_*), а не
//  за родом сторінки.
inline void menuActivate(bool longPress) {
    uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
    if (!menuRow(g_menuSel, &kind, &code, &group)) return;
    if (kind == MI_PAGE) {
        if (longPress) return;                    // сторінку відкриває коротке
        g_displayPage = menuPageToDisplayPage(code);
        displayFlip();
        return;
    }
    char nb[OP_NAME_BUF]; const char *n, *a, *b2; uint8_t d, c;
    opInfo(code, &n, &a, &b2, &d, nb, sizeof(nb), &c);
    if (d == OPD_SAFE) {
        if (longPress) return;
        g_actionRequested = code;
        return;                                   // .ino сам покаже, що вийшло
    }
    if (!longPress) { displayShow("тримайте OK = ПУСК"); return; }
    g_actionRequested = code;
    displayShow("ВИКОНУЮ...");
}

// Скільки триває «довге» натискання. Одне число на всі гілки: раніше кожна
// заводила своє, і сторінка помилки живлення мовчки жила з нулем — тобто без
// поняття «довге» взагалі. Саме число живе в combo.h разом із порогами
// жестів: усі три — сходинки однієї шкали, і перевіряти відстані між ними
// можна лише там, де вони лежать поруч.
#define BTN_LONG_MS COMBO_LONG_MS

// ── ПРИХОВАНІ ЖЕСТИ: УТРИМАННЯ ОДНІЄЇ КНОПКИ ──────────────────────────────
//  Кнопка та сама, що зчитує пакет: у складанні з трьома кнопками це «OK», у
//  складанні з двома окремого OK немає — і роль дістається «‹» (вибір і
//  виконання). Пороги — 5 с (перемкнути заряд) і 10 с; див. combo.h.
#ifdef MENU_BTN3_PIN
  #define HOLD_BTN_ST    b3
#else
  #define HOLD_BTN_ST    b2
#endif

// Чи робить довге натискання щось на поточному екрані — і що саме.
//
//  ⚑ ПОРОЖНЬО — ТЕЖ ВІДПОВІДЬ. Обіцяти «відпустіть = ПУСК» там, де довге
//  натискання нічого не виконує (пункт-сторінка, безпечна операція), гірше,
//  ніж мовчати: людина відпускає, нічого не стається, і наступного разу вона
//  вже не вірить жодній підказці.
inline const char *holdLongHint() {
    if (g_displayPage == PAGE_MENU) {
        uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
        if (!menuRow(g_menuSel, &kind, &code, &group)) return nullptr;
        if (kind == MI_PAGE) return nullptr;          // сторінку відкриває коротке
        char nb[OP_NAME_BUF]; const char *n, *a, *b2; uint8_t d, c;
        opInfo(code, &n, &a, &b2, &d, nb, sizeof(nb), &c);
        return (d == OPD_SAFE) ? nullptr : "відпустіть = ПУСК";
    }
    if (g_displayPage == PAGE_WIZARD)      return "відпустіть = ВИКОНАТИ";
    if (g_displayPage < NUM_STATUS_PAGES)  return "відпустіть = ЗЧИТАТИ";
    return nullptr;
}

// Перемкнути «версію без заряду» й показати, що вийшло.
inline void displayToggleChargeMode() {
    bool off = chargeSetOffByUser(!chargeOffByUser());
    // Список щойно став коротшим (або довшим) — курсор міг лишитись за межами.
    g_menuSel = menuClampSel(g_menuSel);
    displaySetStatus(off ? "ЗАРЯД ВИМКНЕНО" : "ЗАРЯД УВІМКНЕНО");
    displayScreenCleared();
    displayRender();
    if (off) buzzErr(); else buzzOk();
}

inline void displayHandleButton() {
    static BtnState b1, b2, b3;
#ifdef MENU_BTN_ADC_PIN
    btnAdcRefresh();   // один аналоговий зчит на весь прохід нижче
#endif

    // ⚑ ОПИТУЄМО КНОПКИ ОДИН РАЗ НА ВСІ ГІЛКИ. Раніше кожен режим (розряд,
    //  заряд, помилка живлення, звичайне меню) опитував їх сам, своїм набором
    //  BtnState. Поки подія просто розгалужувалась, це працювало; але жест —
    //  це ТРИВАЛІСТЬ, і виміряти її чотирма незалежними детекторами
    //  неможливо: перейшов між сторінками — і відлік почався заново, бо
    //  тримав його вже інший детектор.
    // ⚑ ПІД ЧАС ОПЕРАЦІЇ УТРИМАННЯ НАЛЕЖИТЬ АВАРІЙНІЙ ЗУПИНЦІ, А НЕ ЖЕСТАМ.
    //  Зупинка мусить спрацювати НА ПОРОЗІ: «відпустіть, і тоді зупиниться» —
    //  це вже не аварійна зупинка. Тому на екранах заряду й розряду жестів
    //  немає взагалі, і кнопка там працює точно так, як працювала.
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

    // ── ЖЕСТИ УТРИМАННЯ ───────────────────────────────────────────────────
    //  ⚑ «ДОВГЕ» НА ЦІЙ КНОПЦІ ВИРІШУЄТЬСЯ НА ВІДПУСКАННІ. Через те їй і
    //  передано holdLongMs = 0: pollButtonRaw() робить лише антидребезг, а
    //  подію «довге» дає детектор нижче. Інакше дорога до вимикача заряду
    //  проходила б через уже виконану операцію: на 0.8 с зробилось би
    //  скидання, і аж потім, на 5 с, перемкнувся б заряд.
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

    // Поки на екрані повноекранне повідомлення — кнопки лише прибирають його.
    // Кнопку, якою його щойно викликали, ще тримають: жест уже спрацював і
    // більше подій не дає, тож повідомлення не зникає само собою під пальцем.
    if (displayFlashActive()) {
        if (e1 || e2 || e3 || hev != CHOLD_NONE) { g_flash.until = 0; displayRender(); }
        return;
    }

    if (hev == CHOLD_FLASH) {
        comboFlashArm(g_flash, millis(), COMBO_FLASH_MS);
        displayScreenCleared();
        displayRender();
        return;
    }
    if (hev == CHOLD_CHARGE) { displayToggleChargeMode(); return; }
    if (hev == CHOLD_ARM_CHARGE) {
        // П'ять секунд, протягом яких нічого не відбувається, читаються як
        // «не працює». Кажемо, що буде, якщо відпустити зараз.
        displayShow(chargeOffByUser() ? "відпустіть = ЗАРЯД УВІМК"
                                      : "відпустіть = ЗАРЯД ВИМК");
        return;
    }
    if (hev == CHOLD_ARM_LONG) {
        const char *h = holdLongHint();
        if (h) displayShow(h);
        return;
    }

    // ── РЕЖИМ РОЗРЯДУ ──────────────────────────────────────────────────────
    // Поки навантаження увімкнене, кнопки НЕ гортають меню: на екрані
    // моніторинг, а зміна сторінки «у фоні» лише збиває з пантелику (сторінка
    // мовчки їхала, і після зупинки з'являлась не та, що очікували).
    //   коротке натискання — негайно оновити показання;
    //   довге (0.8 с) на будь-якій кнопці — АВАРІЙНА ЗУПИНКА.
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
    // Будь-яке натискання прибирає її й повертає звичайне меню. Сама
    // несправність нікуди не дівається: лишаються «!» у шапці й код на
    // світлодіоді, а якщо стан зміниться на інший — сторінка з'явиться знову.
    if (chargePsuScreenActive()) {
        if (e1 || e2 || e3) { chargePsuDismiss(); displayRender(); }
        return;
    }

    // ── ОДНЕ ПРАВИЛО НА ВСІ ЕКРАНИ ────────────────────────────────────────
    //  Раніше «OK» означав п'ять різних речей залежно від сторінки (вибір,
    //  аналіз, читання, перехід у Майстер, «додому»), а на сторінці «Дії»
    //  взагалі НЕ був вибором — він гортав список. Тепер напрям і дія
    //  розділені раз і назавжди:
    //     «‹»/«›» — рух (сторінка або пункт), довге — стрибок (додому/група);
    //     «OK»    — увійти чи виконати, і більше нічого.
#ifdef MENU_BTN3_PIN
    if (g_displayPage == PAGE_MENU) {
        int total = menuCount();
        if      (e1 == 1) { g_menuSel = (g_menuSel + 1) % total;         displayRenderBody(false); }
        else if (e1 == 2) { g_menuSel = menuNextGroup(g_menuSel);        displayRenderBody(false); }
        else if (e2 == 1) { g_menuSel = (g_menuSel - 1 + total) % total; displayRenderBody(false); }
        else if (e2 == 2) { g_menuSel = menuPrevGroup(g_menuSel);        displayRenderBody(false); }
        else if (e3)      { menuActivate(e3 == 2); }
        return;
    }
    if (g_displayPage >= NUM_STATUS_PAGES) {
        // Службова сторінка, відкрита з меню: «‹» повертає туди, звідки
        // прийшли, довге «‹» — одразу до показань.
        if      (e2 == 1) { g_displayPage = PAGE_MENU; displayFlip(); }
        else if (e2 == 2) { g_displayPage = PAGE_MAIN; displayFlip(); }
        else if (e1 == 1 && (g_displayPage == PAGE_RAW38 || g_displayPage == PAGE_RAW33)) {
            // Два дампи — пара, між ними ходити зручніше «›», ніж через меню.
            g_displayPage = (g_displayPage == PAGE_RAW38) ? PAGE_RAW33 : PAGE_RAW38;
            displayFlip();
        } else if (g_displayPage == PAGE_WIZARD) {
            if      (e3 == 1) { g_wizReq = 1; g_wizBusy = true; displaySetStatus("АНАЛІЗ..."); displayRender(); }
            else if (e3 == 2) { g_wizReq = 2; g_wizBusy = true; displaySetStatus("ВИКОНУЮ..."); displayRender(); }
        }
        return;
    }
    // Кільце показань.
    if      (e1 == 1) { g_displayPage = (g_displayPage + 1) % NUM_STATUS_PAGES; displayFlip(); }
    else if (e2 == 1) { g_displayPage = (g_displayPage - 1 + NUM_STATUS_PAGES) % NUM_STATUS_PAGES; displayFlip(); }
    else if (e2 == 2) { g_displayPage = PAGE_MAIN; displayFlip(); }
    else if (e3 == 1) { g_displayPage = PAGE_MENU; displayFlip(); }
    else if (e3 == 2) { g_readRequested = true; displayShow("ЗЧИТУВАННЯ..."); }
#else
    // ДВІ кнопки: «›» — рух, «‹» — вибір/виконання. Те саме правило, лише
    // рух назад дістається довгому натисканню замість окремої кнопки.
    if (g_displayPage == PAGE_MENU) {
        int total = menuCount();
        if      (e1 == 1) { g_menuSel = (g_menuSel + 1) % total;  displayRenderBody(false); }
        else if (e1 == 2) { g_menuSel = menuNextGroup(g_menuSel); displayRenderBody(false); }
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
    else if (e1 == 2) { g_readRequested = true; displayShow("ЗЧИТУВАННЯ..."); }
    else if (e2 == 1) { g_displayPage = PAGE_MENU; displayFlip(); }
#endif
}

#endif  // DISPLAY_COLOR_H
