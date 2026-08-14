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
  static ST7789Panel tft = ST7789Panel(DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
#else
  static Adafruit_ST7789 tft = Adafruit_ST7789(DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
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

// -------------------- Шрифти, адаптивно за шириною --------------------
// ВАЖЛИВО: беремо ЛИШЕ ті кириличні шрифти, що зашиті в U8g2_for_Adafruit_GFX
// (це підмножина u8g2: 4x6/5x8/6x12/7x13/8x13/9x15/10x20 *_t_cyrillic).
// Немає 9x18_t_cyrillic і fub* — тому великий % малюємо вбудованим шрифтом GFX.
#if TFT_W < 200                                   // вузькі панелі (135/170/172)
  #define FONT_HDR    u8g2_font_7x13_t_cyrillic
  #define FONT_BODY   u8g2_font_6x12_t_cyrillic
  #define FONT_SMALL  u8g2_font_5x8_t_cyrillic
  #define FONT_MODEL  u8g2_font_8x13_t_cyrillic
  #define BIG_TSIZE   3                           // масштаб вбудованого шрифту GFX
#else                                             // 240-піксельні панелі
  #define FONT_HDR    u8g2_font_10x20_t_cyrillic
  #define FONT_BODY   u8g2_font_9x15_t_cyrillic
  #define FONT_SMALL  u8g2_font_6x12_t_cyrillic
  #define FONT_MODEL  u8g2_font_10x20_t_cyrillic
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
static bool g_readRequested = false;
static int  g_actionSel = 0;
static int  g_actionRequested = -1;

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
static int g_animPrevX = -1;                 // (сумісність; більше не використ.)
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
    int vpct = (int)((vmv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
    if (vpct < 0) vpct = 0; if (vpct > 100) vpct = 100;
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
inline bool decodeFactoryHealthTable(const uint8_t **data, int *len) {
    if (!hasDump || !impresRecordOk(batteryDump, IMPRES_FACTORY_REC)) return false;
    *data = batteryDump + IMPRES_FACTORY_REC + 1;
    *len  = batteryDump[IMPRES_FACTORY_REC] - 2;
    return true;
}

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

// Склад і порядок операцій задає operations.h (спільний для всіх поверхонь).
inline int numActions() { return opCount(); }

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

inline uint16_t chargeColor(int pct) {
    if (pct < 0)   return C_MUTED;
    if (pct >= 60) return C_GREEN;
    if (pct >= 30) return C_YELLOW;
    return C_RED;
}

inline void drawHeaderBar(const char *title) {
    tft.fillRect(0, 0, TFT_W, HDR_H, C_HDRBG);
    tft.drawFastHLine(0, HDR_H - 1, TFT_W, C_BLUE);
    char h[16];
    // ⚑ «!» перед номером сторінки — ознака несправності ЖИВЛЕННЯ, видима з
    // БУДЬ-ЯКОЇ сторінки: без блока живлення заряд не піде, хай що користувач
    // зараз гортає. Розшифровка — на сторінці заряду й у вебі.
    bool psuBad = chargePsuFault();
    snprintf(h, sizeof(h), "%s%d/%d", psuBad ? "!" : "",
             g_displayPage + 1, NUM_DISPLAY_PAGES);
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

// Іконка батареї зі шкалою заповнення; pct<0 — даних немає.
inline void drawBatteryBar(int x, int y, int w, int h, int pct, uint16_t col) {
    g_battX = x; g_battY = y; g_battW = w; g_battH = h;   // для displayAnimTick()
    g_animPrevX = -1;                                     // скинути слід блику
    tft.drawRoundRect(x, y, w, h, 4, C_TEXT);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 3, C_TEXT);
    tft.fillRect(x + w, y + h / 3, 4, h - 2 * (h / 3), C_TEXT);   // "плюсовий" вивід
    if (pct < 0) return;
    int fw = (w - 6) * pct / 100;
    if (fw < 0) fw = 0;
    if (fw > w - 6) fw = w - 6;
    if (fw > 0) tft.fillRect(x + 3, y + 3, fw, h - 6, col);
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

inline void displaySplash() {
    // Заставка — на ВЕСЬ екран, статус-смуги на ній немає, тож запобіжник
    // «нижче смуги не писати» тут має бути вимкнений: інакше нижній рядок
    // напису зник би на низьких панелях.
    g_tFooter = true;
    tft.fillScreen(C_BG);

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
    uint16_t col = chargeColor(pct);

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
inline void drawPageActions() {
    // Назви/описи/небезпека беруться з operations.h — того самого каталогу, що
    // й у монохромному екрані, вебі й USB-клієнті. Локальних списків більше немає.
    int sel = g_actionSel, total = numActions();
    const char *name, *l1, *l2; uint8_t danger, chips; char nbuf[26];
    opInfo(sel, &name, &l1, &l2, &danger, nbuf, sizeof(nbuf), &chips);

    // У шапці — не лише номер дії, а й КУДИ вона пише: плутанина між DS2433
    // (ідентичність) і DS2438 (монітор) коштує або моделі, або заводського
    // калібрування вимірювача струму.
    char t[36];
    if (chips == OPC_NONE) snprintf(t, sizeof(t), "Дія %d/%d", sel + 1, total);
    else                   snprintf(t, sizeof(t), "Дія %d/%d %s", sel + 1, total,
                                    opChipsShort(chips));
    drawHeaderBar(t);

    // Картка операції (у межах безпечної зони кутів). Оновлюємо НА МІСЦІ: рамку
    // перемальовуємо, а кожен рядок тексту чистимо ТОНКО й друкуємо. Тож при
    // перемиканні операції екран НЕ блимає всім тілом.
    int cardx = EDGE > 10 ? EDGE - 4 : 10;
    int txtx  = cardx + 10;
    int cxi   = cardx + 2;
    int cw    = TFT_W - 2 * cardx - 4;
    uint16_t accent = (danger == OPD_WIPE) ? C_RED
                    : (danger == OPD_WRITE) ? C_YELLOW : C_GREEN;
    tft.drawRoundRect(cardx, 44, TFT_W - 2 * cardx, 96, 6, accent);
    tft.fillRect(cxi, 60, cw, 26, C_BG);  tSet(FONT_MODEL, accent); tPut(txtx, 78, name);
    tft.fillRect(cxi, 90, cw, 20, C_BG);  tSet(FONT_BODY, C_TEXT);  tPut(txtx, 106, l1);
    tft.fillRect(cxi, 112, cw, 20, C_BG);                           tPut(txtx, 128, l2);
    tft.fillRect(cxi, 148, cw, 22, C_BG);                 // рядок попередження — чистимо завжди
    if (danger == OPD_WIPE) {
        tSet(FONT_BODY, C_RED);
        tPut(txtx, 164, "!! НЕЗВОРОТНЬО !!");
    } else if (sel == OP_CELLSWAP) {
        // Головна операція ремонту — одразу нагадуємо про обов'язковий крок.
        tSet(FONT_BODY, C_GREEN);
        tPut(txtx, 164, "далі -> на IMPRES-ЗП");
    }

    // Підказка керування.
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    tSet(FONT_SMALL, C_MUTED, C_CARD);
#ifdef MENU_BTN3_PIN
    tPut(EDGE, TFT_H - 8, "[<][>] меню  [OK] вибір, трим=ПУСК");
#else
    tPut(EDGE, TFT_H - 8, "[<] вибір   [<] тримати = ПУСК");
#endif
}

// Сторінка МОНІТОРИНГУ РОЗРЯДУ. Показується автоматично, поки навантаження
// увімкнене, і має пріоритет над гортанням меню: розряд — довга операція, під
// час якої на екрані має бути видно все службове, що змінюється.
inline void drawPageDischarge() {
    drawHeaderBar("РОЗРЯД");
    const DischargeState &d = g_dis;

    // Напруга — найбільшим, це головне число процесу.
    char b[56];                    // кирилиця в UTF-8 — 2 байти на літеру
    tft.fillRect(0, HDR_H + 4, TFT_W, 40, C_BG);
    snprintf(b, sizeof(b), "%u.%02u В", d.lastMv / 1000, (d.lastMv % 1000) / 10);
    tSet(FONT_MODEL, chargeColor(impresPercentFromMv(d.lastMv)));
    tPut(EDGE, HDR_H + 36, b);

    // Прогрес до цілі — числом у рядку «ціль» нижче.
    int span = (int)d.startMv - (int)d.targetMv;
    int done = (int)d.startMv - (int)d.lastMv;
    int pct  = (span > 0) ? (done * 100 / span) : 0;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;

    // Рівень заряду — ТАКОЮ Ж анімованою іконкою батареї, як на головній
    // сторінці: drawBatteryBar() запам'ятовує геометрію в g_battX/Y/W/H, а
    // displayAnimTick() ганяє по заповненню той самий градієнт.
    const char *csrc; int chargePct = batteryPercent(&csrc);
    int by = HDR_H + 44, bh = 22;
    int bx = EDGE, bw = TFT_W - 2 * EDGE - 6;      // −6 px під «плюсовий» вивід
    tft.fillRect(0, by - 2, TFT_W, bh + 4, C_BG);
    drawBatteryBar(bx, by, bw, bh, chargePct, chargeColor(chargePct));
    g_pctTx = g_pctTy = g_pctTw = g_pctTh = 0;     // цифр усередині шкали немає

    // Рядки показань. Кожен сам чистить свою смужку на всю ширину, тож при
    // оновленні раз на 5 с екран не блимає і старий текст не «просвічує».
    tSet(FONT_BODY, C_TEXT);
    int y = by + bh + 18;
    // Рядки, що не влізли до статус-смуги, просто не малюємо: на вузьких
    // панелях (240×240) місця менше, і краще втратити «час», ніж заїхати
    // текстом на підвал. Порядок нижче — за спаданням важливості.
    auto row = [&](const char *txt, uint16_t col) {
        if (y > FOOT_Y - 4) return;
        tft.fillRect(0, y - 12, TFT_W, 16, C_BG);
        tSet(FONT_BODY, col);
        tPut(EDGE, y, txt);
        y += 18;
    };

    snprintf(b, sizeof(b), "ціль %u.%02u В  (%d%%)", d.targetMv / 1000, (d.targetMv % 1000) / 10, pct);
    row(b, C_TEXT);

    // Струм і потужність — з ВБУДОВАНОГО датчика струму DS2438 (його резистор
    // стоїть усередині пакета послідовно з банками). Показуємо СЕРЕДНІЙ струм:
    // ключ працює ШІМом, тож саме він і тече, а не виміряний пік.
    int wx10 = dischargeWattsX10(d.lastMv, d.lastMa);
    snprintf(b, sizeof(b), "струм %d мА · %d.%d Вт", d.lastMa, wx10 / 10, wx10 % 10);
    row(b, (d.state == DIS_RUN && !dischargeInBand(d)) ? C_YELLOW : C_TEXT);

    // Обмеження струму: уставка веде за напругою (1000 мА на повному заряді ->
    // 300 мА у кінці), ключ тримає її шпаруватістю. Пік — струм при 100 %.
    if (dischargePwmOk()) {
        snprintf(b, sizeof(b), "уст %u · ШІМ %u%% · пік %u", d.setMa, d.dutyPct, d.peakMa);
        row(b, C_MUTED);
    } else {
        row("БЕЗ ШІМ: не обмежено!", C_RED);
    }

    // Віддано: наш інтеграл по опитуваннях і апаратний лічильник DCA самого
    // DS2438. DCA рахує неперервно, тож велика розбіжність = опитування щось
    // пропускає.
    snprintf(b, sizeof(b), "віддано %lu мА·год", (unsigned long)dischargeMah());
    row(b, C_GREEN);
    snprintf(b, sizeof(b), "DCA %lu мА·год · ICA %u", (unsigned long)dischargeDcaMah(), d.lastIca);
    row(b, C_MUTED);

    snprintf(b, sizeof(b), "темп. %d.%d °C", d.lastTempC10 / 10, abs(d.lastTempC10 % 10));
    row(b, d.lastTempC10 >= DISCHARGE_MAX_TEMP_C * 10 - 50 ? C_RED : C_TEXT);

    unsigned long el = d.elapsedS;
    snprintf(b, sizeof(b), "час  %lu:%02lu:%02lu", el / 3600, (el / 60) % 60, el % 60);
    row(b, C_TEXT);

    // Підвал: стан або причина зупинки.
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    const char *foot = (d.state == DIS_RUN)   ? "[OK] тримати = ЗУПИНИТИ"
                     : (d.state == DIS_DONE)  ? "ГОТОВО -> на IMPRES-ЗП"
                     : dischargeReasonText(d.reason);
    tSet(FONT_SMALL, d.state == DIS_ABORT ? C_RED : C_MUTED, C_CARD);
    tPut(EDGE, TFT_H - 8, foot);
}

// Сторінка МОНІТОРИНГУ ЗАРЯДУ (кольорова) — та сама схема, що й розряд вище,
// але прогрес рахується у бік ЗРОСТАННЯ напруги (ціль вища за старт), і
// показуємо ЦІЛЬОВУ вихідну напругу ДС/ДС замість шпаруватості (крива
// керування нелінійна, див. charge.h/settings.h — сира напруга сама по собі
// нічого не каже про очікуваний струм).
inline void drawPageCharge() {
    drawHeaderBar("ЗАРЯД");
    const ChargeState &c = g_chg;

    char b[56];
    tft.fillRect(0, HDR_H + 4, TFT_W, 40, C_BG);
    snprintf(b, sizeof(b), "%u.%02u В", c.lastMv / 1000, (c.lastMv % 1000) / 10);
    tSet(FONT_MODEL, chargeColor(impresPercentFromMv(c.lastMv)));
    tPut(EDGE, HDR_H + 36, b);

    const char *csrc; int chargePct = batteryPercent(&csrc);
    int by = HDR_H + 44, bh = 22;
    int bx = EDGE, bw = TFT_W - 2 * EDGE - 6;
    tft.fillRect(0, by - 2, TFT_W, bh + 4, C_BG);
    drawBatteryBar(bx, by, bw, bh, chargePct, chargeColor(chargePct));
    g_pctTx = g_pctTy = g_pctTw = g_pctTh = 0;

    tSet(FONT_BODY, C_TEXT);
    int y = by + bh + 18;
    auto row = [&](const char *txt, uint16_t col) {
        if (y > FOOT_Y - 4) return;
        tft.fillRect(0, y - 12, TFT_W, 16, C_BG);
        tSet(FONT_BODY, col);
        tPut(EDGE, y, txt);
        y += 18;
    };

    // ЖИВЛЕННЯ — найпершим рядком і тільки коли з ним негаразд. Без блока
    // заряд не піде взагалі, тож ховати причину нижче за наслідки не можна.
    if (chargePsuFault()) {
        snprintf(b, sizeof(b), "БЖ %u.%02u В — %s", chargePsuMv() / 1000,
                 (chargePsuMv() % 1000) / 10,
                 chargePsuState() == PSU_ABSENT ? "НЕМАЄ ЖИВЛЕННЯ" :
                 chargePsuState() == PSU_LOW    ? "НАПРУГА ЗАНИЖЕНА" : "НАПРУГА ЗАВИЩЕНА");
        row(b, C_RED);
        snprintf(b, sizeof(b), "потрібен блок %u.%u..%u.%u В",
                 CHARGE_PSU_MIN_MV / 1000, (CHARGE_PSU_MIN_MV % 1000) / 100,
                 CHARGE_PSU_MAX_MV / 1000, (CHARGE_PSU_MAX_MV % 1000) / 100);
        row(b, C_RED);
    }

    snprintf(b, sizeof(b), "ціль %u.%02u В (%u%%)",
             c.targetMv / 1000, (c.targetMv % 1000) / 10, c.targetPct);
    row(b, C_TEXT);

    // Струм і потужність — із ВЛАСНОГО шунта пристрою (CHARGE_SHUNT_MOHM у
    // мінусовому проводі), а не з DS2438: монітор пакета читається лише на
    // закритому ключі й дає температуру, а не струм заряду.
    int wx10 = chargeWattsX10(c.lastMv, c.lastMa);
    snprintf(b, sizeof(b), "струм %d мА · %d.%d Вт", c.lastMa, wx10 / 10, wx10 % 10);
    row(b, C_TEXT);

    // Уставка, вершина пульсацій струму і шпаруватість ключа: вершина злітає,
    // коли дросель фактично випав із кола, а середнє це ще приховує.
    if (chargePwmOk()) {
        snprintf(b, sizeof(b), "уст %u мА · пік %u мА · %u%%", c.setMa, c.peakMa, chargeDutyPct());
        row(b, C_MUTED);
    } else {
        row("БЕЗ КЕРУВАННЯ: перевірте!", C_RED);
    }

    // Отримано: наш інтеграл по опитуваннях і апаратний лічильник CCA самого
    // DS2438.
    snprintf(b, sizeof(b), "отримано %lu мА·год", (unsigned long)chargeMah());
    row(b, C_GREEN);
    snprintf(b, sizeof(b), "CCA %lu мА·год · ICA %u", (unsigned long)chargeCcaMah(), c.lastIca);
    row(b, C_MUTED);

    snprintf(b, sizeof(b), "темп. %d.%d °C", c.lastTempC10 / 10, abs(c.lastTempC10 % 10));
    row(b, c.lastTempC10 >= CHARGE_MAX_TEMP_C * 10 - 50 ? C_RED : C_TEXT);

    unsigned long el = c.elapsedS;
    snprintf(b, sizeof(b), "час  %lu:%02lu:%02lu", el / 3600, (el / 60) % 60, el % 60);
    row(b, C_TEXT);

    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    const char *foot = (c.state == CHG_RUN)  ? "[OK] тримати = ЗУПИНИТИ"
                      : (c.state == CHG_DONE) ? "ГОТОВО"
                      : chargeReasonText(c.reason);
    tSet(FONT_SMALL, c.state == CHG_ABORT ? C_RED : C_MUTED, C_CARD);
    tPut(EDGE, TFT_H - 8, foot);
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
    if (clearBody) tft.fillRect(0, HDR_H, TFT_W, FOOT_Y - HDR_H, C_BG);
    // Поки навантаження увімкнене — примусово показуємо моніторинг розряду,
    // хоч би яку сторінку було обрано: це довга операція із запобіжниками, її
    // стан має бути на екрані завжди, а не за кілька натискань кнопки.
    // Заряд і розряд не можуть іти одночасно, тож порядок цих двох перевірок
    // не має значення.
    if (dischargeScreenActive()) { drawPageDischarge(); return; }
    if (chargeScreenActive())    { drawPageCharge();    return; }
    switch (g_displayPage) {
        case 0:  drawPageMain();     break;
        case 1:  drawPageModel();    break;
        case 2:  drawPageTech();     break;
        case 3:  drawPageHealth();   break;
        case 4:  drawPageRaw2438();  break;
        case 5:  drawPageRaw2433();  break;
        case 6:  drawPageActions();  break;
        case 7:  drawPageWizard();   break;
        default: drawPageMain();     break;
    }
}
inline void displayRender() { displayRenderBody(true); }

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
inline uint16_t lighten565(uint16_t c, uint8_t amt) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r += ((31 - r) * amt) >> 8; g += ((63 - g) * amt) >> 8; b += ((31 - b) * amt) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Масштаб яскравості кольору RGB565: lvl 0..255 (255 = без змін, менше = темніше).
inline uint16_t scale565(uint16_t c, uint8_t lvl) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = r * lvl / 255; g = g * lvl / 255; b = b * lvl / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Заповнити прямокутник R кольором col, ОМИНАЮЧИ рамку тексту T (перетин R∩T не
// малюємо) — до 4 смуг. Так пульсуємо все заповнення, не торкаючись цифр %.
inline void fillRectExcept(int rx, int ry, int rw, int rh,
                           int tx, int ty, int tw, int th, uint16_t col) {
    int rx1 = rx + rw, ry1 = ry + rh;
    int cx0 = tx > rx ? tx : rx,   cy0 = ty > ry ? ty : ry;      // перетин R∩T
    int cx1 = (tx + tw) < rx1 ? (tx + tw) : rx1;
    int cy1 = (ty + th) < ry1 ? (ty + th) : ry1;
    if (cx0 >= cx1 || cy0 >= cy1) { tft.fillRect(rx, ry, rw, rh, col); return; }
    if (cy0 > ry)  tft.fillRect(rx, ry, rw, cy0 - ry, col);              // над T
    if (ry1 > cy1) tft.fillRect(rx, cy1, rw, ry1 - cy1, col);            // під T
    if (cx0 > rx)  tft.fillRect(rx, cy0, cx0 - rx, cy1 - cy0, col);      // ліворуч T
    if (rx1 > cx1) tft.fillRect(cx1, cy0, rx1 - cx1, cy1 - cy0, col);    // праворуч T
}

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
    if (g_errTint) return;              // під час оповіщення про помилку — статичний
                                        // червоний екран (без руху градієнта)
    if (g_ledMode == LED_READ || g_ledMode == LED_WRITE)
        return;                         // під час операції (читання/запис) — екран
                                        // статичний, без руху/блимання градієнта
    const char *src; int pct = batteryPercent(&src);
    if (pct < 0) return;
    uint16_t col = chargeColor(pct);
    int fw = (g_battW - 6) * pct / 100;
    if (fw < 4) return;
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
    displayRender();
    for (int b = 40; b <= 255; b += 10) { analogWrite(DISPLAY_BLK_PIN, b); delay(12); }
    analogWrite(DISPLAY_BLK_PIN, 255);
#else
    tft.fillScreen(C_BG);
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
    drawFooterBar();
}

// Увімкнути/вимкнути червоний «світлофільтр» на час оповіщення про помилку.
// Один перемальовок при зміні стану — екран стає (чи перестає бути) червоним,
// БЕЗ блимання. Викликається з loop() за станом індикатора (LED_ERROR).
inline void displaySetErrorTint(bool on) {
    if (g_errTint == on) return;
    g_errTint = on;
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

inline void displayInit() {
#ifdef DISPLAY_BLK_PIN
    pinMode(DISPLAY_BLK_PIN, OUTPUT);
    analogWrite(DISPLAY_BLK_PIN, 0);          // підсвітка ВИМК до заставки —
                                              // ховаємо артефакти ініціалізації
#endif
    tft.init(PANEL_W, PANEL_H);               // рідні (портретні) розміри матриці
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
    Serial.printf("DISPLAY: ST7789 %dx%d (panel %dx%d) color, rot=%d\n",
                  (int)TFT_W, (int)TFT_H, (int)PANEL_W, (int)PANEL_H, (int)DISPLAY_ST7789_ROT);
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

inline void displayHandleButton() {
    static BtnState b1, b2;
#ifdef MENU_BTN3_PIN
    static BtnState b3;
#endif
#ifdef MENU_BTN_ADC_PIN
    btnAdcRefresh();   // один аналоговий зчит на весь прохід нижче
#endif

    // ── РЕЖИМ РОЗРЯДУ ──────────────────────────────────────────────────────
    // Поки навантаження увімкнене, кнопки НЕ гортають меню: на екрані
    // моніторинг, а зміна сторінки «у фоні» лише збиває з пантелику (сторінка
    // мовчки їхала, і після зупинки з'являлась не та, що очікували).
    //   коротке натискання — негайно оновити показання;
    //   довге (0.8 с) на будь-якій кнопці — АВАРІЙНА ЗУПИНКА.
    if (dischargeScreenActive()) {
        int d1 = pollButtonRaw(btn1Raw(), b1, 800);
        int d2 = pollButtonRaw(btn2Raw(), b2, 800);
        int d3 = 0;
#ifdef MENU_BTN3_PIN
        d3 = pollButtonRaw(btn3Raw(), b3, 800);
#endif
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
        int d1 = pollButtonRaw(btn1Raw(), b1, 800);
        int d2 = pollButtonRaw(btn2Raw(), b2, 800);
        int d3 = 0;
#ifdef MENU_BTN3_PIN
        d3 = pollButtonRaw(btn3Raw(), b3, 800);
#endif
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

#ifdef MENU_BTN3_PIN
    // 3 кнопки: BTN1 — ЧИСТА навігація ВПЕРЕД (жодного «довгого» читання; читання
    // акумулятора перенесено на BTN3 на головній сторінці). longMs=0 -> «довгих»
    // подій немає взагалі, тож випадкове утримання не запускає читання.
    int e1 = pollButtonRaw(btn1Raw(), b1, 0);
    if (e1 == 1) {
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        displayFlip();
    }
#else
    // 2 кнопки: коротко — наступна сторінка, ДОВГО — читання акумулятора.
    int e1 = pollButtonRaw(btn1Raw(), b1, 800);
    if (e1 == 2) {
        g_readRequested = true;
        displayShow("ЗЧИТУВАННЯ...");     // лише футер — без перемальовки тіла (без блимання)
    } else if (e1 == 1) {
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        displayFlip();
    }
#endif

    int e2 = pollButtonRaw(btn2Raw(), b2, 800);
#ifdef MENU_BTN3_PIN
    // 3 кнопки: BTN2 — ЧИСТА «назад» скрізь (усе дійове — на BTN3).
    if (e2 == 1) {
        g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
        displayFlip();
    }
#else
    // 2 кнопки: BTN2 суміщає навігацію + вибір/аналіз (коротко) + виконання (довго).
    if (g_displayPage == RESET_PAGE) {
        if (e2 == 1) { g_actionSel = (g_actionSel + 1) % numActions(); displayRenderBody(false); }  // оновити картку без блимання
        else if (e2 == 2) { g_actionRequested = g_actionSel; displayShow("ВИКОНУЮ..."); }
    } else if (g_displayPage == WIZARD_PAGE) {
        if (e2 == 1) { g_wizReq = 1; g_wizBusy = true; displaySetStatus("АНАЛІЗ..."); displayRender(); }
        else if (e2 == 2) { g_wizReq = 2; g_wizBusy = true; displaySetStatus("ВИКОНУЮ..."); displayRender(); }
    } else if (e2 == 1) {
        g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
        displayFlip();
    }
#endif

#ifdef MENU_BTN3_PIN
    // Третя кнопка «OK / Дія»: «Дії» коротко=вибір/довго=виконати; «Майстер»
    // коротко=аналіз/довго=крок; інші коротко=у Майстер/довго=додому.
    int e3 = pollButtonRaw(btn3Raw(), b3, 800);
    if (g_displayPage == RESET_PAGE) {
        if (e3 == 1) { g_actionSel = (g_actionSel + 1) % numActions(); displayRenderBody(false); }  // оновити картку без блимання
        else if (e3 == 2) { g_actionRequested = g_actionSel; displayShow("ВИКОНУЮ..."); }
    } else if (g_displayPage == WIZARD_PAGE) {
        if (e3 == 1) { g_wizReq = 1; g_wizBusy = true; displaySetStatus("АНАЛІЗ..."); displayRender(); }
        else if (e3 == 2) { g_wizReq = 2; g_wizBusy = true; displaySetStatus("ВИКОНУЮ..."); displayRender(); }
    } else if (g_displayPage == 0) {
        // Головна сторінка: BTN3 коротко = ПЕРЕЧИТАТИ акумулятор (саме третя кнопка,
        // а не довге утримання BTN1); довго = перейти в «Майстер».
        if (e3 == 1) { g_readRequested = true; displayShow("ЗЧИТУВАННЯ..."); }
        else if (e3 == 2) { g_displayPage = WIZARD_PAGE; displayFlip(); }
    } else {
        if (e3 == 1) { g_displayPage = WIZARD_PAGE; displayFlip(); }   // швидко в «Майстер»
        else if (e3 == 2) { g_displayPage = 0; displayFlip(); }       // «додому»
    }
#endif
}

#endif  // DISPLAY_COLOR_H
