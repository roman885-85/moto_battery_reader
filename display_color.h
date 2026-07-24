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
#include "battery_reader.h"
#include "templates.h"    // BATTERY_TEMPLATES/COUNT — дії «Новий АКБ»

// Стан, яке відображаємо (визначене в .ino / web_server.h).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump[DUMP_SIZE];
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern uint8_t chipSN2438[8];
extern bool hasSN2438;

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

// -------------------- Палітра (RGB565) --------------------
#define C_BG      0x0000     // чорний фон
#define C_CARD    0x18E3     // темно-сіра картка
#define C_HDRBG   0x0208     // темно-синій заголовок
#define C_TEXT    0xFFFF     // білий
#define C_MUTED   0x8C71     // приглушений сірий
#define C_BLUE    0x02D6     // синій (прапор UA)
#define C_YELLOW  0xFEA0     // жовтий (прапор UA)
#define C_GREEN   0x07E6     // добрий заряд
#define C_ORANGE  0xFC60     // середній
#define C_RED     0xF800     // низький / небезпека

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
    uint8_t config = batteryDump2438[0];
    if (config & 0x01) {
        *src = "ICA";
        int pct = (int)batteryDump2438[12] * 100 / ICA_FULL_SCALE;
        return pct > 100 ? 100 : pct;
    }
    *src = "volt";
    long vmv = (long)(((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3]) * 10;
    long pct = (vmv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int)pct;
}

inline int batteryRemainingMah() {
    if (!hasDump2438) return -1;
    return (int)(batteryDump2438[12] * DS2438_MAH_PER_LSB);
}

// Знайти модель (part number) в дампі DS2433. Пріоритет — запис 0x0B+літера.
inline bool decodeModel(char *out, size_t n) {
    if (!hasDump) return false;
    for (int i = 0x30; i < (int)DUMP_SIZE - 12; i++) {
        if (batteryDump[i] == 0x0B && batteryDump[i + 1] >= 'A' && batteryDump[i + 1] <= 'Z') {
            int j = i + 1, len = 0;
            char tmp[16];
            while (j < (int)DUMP_SIZE && len < 12) {
                uint8_t d = batteryDump[j];
                if ((d >= '0' && d <= '9') || (d >= 'A' && d <= 'Z')) { tmp[len++] = (char)d; j++; }
                else break;
            }
            bool digit = false;
            for (int k = 0; k < len; k++) if (tmp[k] >= '0' && tmp[k] <= '9') digit = true;
            if (len >= 6 && len <= 11 && digit) {
                if ((size_t)len >= n) len = n - 1;
                memcpy(out, tmp, len); out[len] = '\0';
                return true;
            }
        }
    }
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

// Ємність/знос з записи історії ємності DS2433 (тег 0x17).
inline bool decodeCapacity(int *capPct, int *wearPct) {
    if (!hasDump) return false;
    for (int i = 0x100; i < (int)DUMP_SIZE - 23; i++) {
        if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) {
            int cap = batteryDump[i + 21];
            if (cap <= 100) { *capPct = cap; *wearPct = 100 - cap; return true; }
        }
    }
    return false;
}

// Евристика справжності/цілісності ПРОШИВКИ (модельно-залежна).
inline bool batteryGenuine(const char **reason) {
    if (!hasDump) { *reason = "нема дампу"; return false; }
    int hs = 0; for (int i = 0; i <= 0x1F; i++) hs += batteryDump[i];
    if ((hs & 0xFF) != 0x41) { *reason = "хибний заголовок"; return false; }
    bool hasModel = false;
    for (int i = 0x30; i < (int)DUMP_SIZE - 11 && !hasModel; i++)
        if (batteryDump[i] == 0x0B && batteryDump[i + 1] >= 'A' && batteryDump[i + 1] <= 'Z') hasModel = true;
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

// Базові дії + «Новий АКБ» на кожен вшитий шаблон (як у монохромній версії).
#define NUM_BASE_ACTIONS 6
inline int numActions() { return NUM_BASE_ACTIONS + BATTERY_TEMPLATE_COUNT; }

// ===================== Примітиви малювання (кольорові) =====================

// Малюємо текст у НЕПРОЗОРОМУ режимі (setFontMode(0)), задаючи фон гліфа
// РІВНИМ кольору ділянки. Так немає чорних ореолів навколо символів на
// кольорових смугах (шапка/статус). За замовчуванням фон = C_BG (чорний).
inline void tSet(const uint8_t *font, uint16_t fg, uint16_t bg = C_BG) {
    u8g2Fonts.setFont(font);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);
}
inline void tPut(int x, int y, const char *s) { u8g2Fonts.drawUTF8(x, y, s); }
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
    tSet(FONT_HDR, C_YELLOW, C_HDRBG);
    tPut(EDGE, 21, title);
    char h[16];
    snprintf(h, sizeof(h), "%d/%d", g_displayPage + 1, NUM_DISPLAY_PAGES);
    tSet(FONT_SMALL, C_TEXT, C_HDRBG);
    tPut(TFT_W - tWidth(h) - EDGE, 20, h);
}

inline void drawFooterBar() {
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    char f[42];
    snprintf(f, sizeof(f), "%s", g_displayStatus);
    tSet(FONT_BODY, C_GREEN, C_CARD);
    tPut(EDGE, TFT_H - 8, f);
}

// Іконка батареї зі шкалою заповнення; pct<0 — даних немає.
inline void drawBatteryBar(int x, int y, int w, int h, int pct, uint16_t col) {
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
}

// ===================== Сторінки =====================

inline void drawPageMain() {
    char buf[48];
    const char *src;
    int pct = batteryPercent(&src);
    int mah = batteryRemainingMah();
    uint16_t col = chargeColor(pct);

    drawHeaderBar("Moto IMPRES");

    // Велика батарея на всю ширину (з відступом від кутів).
    int bx = CX, by = 44, bw = TFT_W - 2 * CX - 4, bh = 60;
    drawBatteryBar(bx, by, bw, bh, pct, col);
    // % великим по центру шкали (вбудований шрифт GFX — завжди доступний).
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    {
        int cw = 6 * BIG_TSIZE * (int)strlen(buf);   // ширина рядка GFX-шрифтом
        int ch = 8 * BIG_TSIZE;
        tft.setTextColor(C_TEXT);
        tft.setTextSize(BIG_TSIZE);
        tft.setCursor(bx + (bw - cw) / 2, by + (bh - ch) / 2);
        tft.print(buf);
        tft.setTextSize(1);
    }
    // Джерело показника (ICA/volt).
    tSet(FONT_SMALL, C_MUTED);
    snprintf(buf, sizeof(buf), "джерело: %s", src);
    tPut(bx, by + bh + 16, buf);

    // Деталі.
    int y = by + bh + 40;
    tSet(FONT_BODY, C_TEXT);
    if (mah >= 0) snprintf(buf, sizeof(buf), "Залишок: %d мА·год", mah);
    else          snprintf(buf, sizeof(buf), "Залишок: --");
    tPut(CX, y, buf); y += 24;

    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t  traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2f В    %.1f °C", vraw * 0.01f, traw * 0.03125f);
    } else snprintf(buf, sizeof(buf), "DS2438: немає даних");
    tPut(CX, y, buf); y += 24;

    tSet(FONT_BODY, C_BLUE);
    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);
    tPut(CX, y, buf); y += 24;

    tSet(FONT_SMALL, C_MUTED);
    tPut(CX, y, "[>] довго — зчитати АКБ");

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
    int      remMah = (int)(batteryDump2438[12] * DS2438_MAH_PER_LSB);

    int y = 66;
    tSet(FONT_BODY, C_TEXT);
    snprintf(buf, sizeof(buf), "Напруга:   %.2f В", vraw * 0.01f);    tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Струм:     %.0f мА", i_mA);           tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Темп.:     %.1f °C", traw * 0.03125f); tPut(CX, y, buf); y += 30;
    snprintf(buf, sizeof(buf), "Залишок:   ~%d мА·год", remMah);      tPut(CX, y, buf);

    drawFooterBar();
}

inline void drawPageHealth() {
    char buf[48];
    drawHeaderBar("Стан АКБ");

    int cap = -1, wear = -1;
    int y = 66;
    tSet(FONT_BODY, C_TEXT);
    if (decodeCapacity(&cap, &wear)) {
        snprintf(buf, sizeof(buf), "Ємність: %d %%", cap);  tPut(CX, y, buf);
        // кольорова смуга ємності (у межах безпечної зони)
        int barx = 150, barw = TFT_W - EDGE - barx;
        uint16_t cc = cap >= 80 ? C_GREEN : cap >= 50 ? C_YELLOW : C_RED;
        tft.drawRect(barx, y - 12, barw, 14, C_MUTED);
        tft.fillRect(barx + 1, y - 11, (barw - 2) * (cap > 100 ? 100 : cap) / 100, 12, cc);
        y += 30;
        snprintf(buf, sizeof(buf), "Знос:    %d %%", wear); tPut(CX, y, buf); y += 30;
    } else {
        tPut(CX, y, "Ємність: (зчитайте)"); y += 30;
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        uint16_t dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        int chgCyc = (int)(cca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH);
        int disCyc = (int)(dca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH);
        snprintf(buf, sizeof(buf), "Циклів: зар.%d роз.%d", chgCyc, disCyc);
        tPut(CX, y, buf); y += 30;
    }

    const char *reason;
    if (batteryGenuine(&reason)) {
        tSet(FONT_BODY, C_GREEN);
        snprintf(buf, sizeof(buf), "Справжня: ТАК  (%s)", reason);
        tPut(CX, y, buf);
    } else {
        tSet(FONT_BODY, C_RED);
        snprintf(buf, sizeof(buf), "РИЗИК: %s", reason);
        tPut(CX, y, buf);
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
        int n = snprintf(buf, sizeof(buf), "%03X:", off);
        for (int c = 0; c < perRow && off + c < count; c++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02X ", data[off + c]);
        tPut(EDGE, y, buf);
        y += 14;
        if (y > FOOT_Y - 4) break;
    }
    drawFooterBar();
}

#define RAW2433_COUNT ((FOOT_Y - HDR_H) / 14 * 8)
inline void drawPageRaw2438() { drawRawColor("DS2438 (hex)", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawColor("DS2433 (hex)", batteryDump, hasDump, RAW2433_COUNT); }

// Сторінка «Дії»: одна обрана операція крупно + опис + попередження.
inline void drawPageActions() {
    static const char *nm[NUM_BASE_ACTIONS] = { "Скидання", "Ремонт", "Очистка", "СТЕРТИ 2433", "Перезавантаж.", "Рекалібр." };
    static const char *d1[NUM_BASE_ACTIONS] = { "обнулити лічильники",
                                                "полагодити суми та",
                                                "стерти все, окрім",
                                                "ПОВНЕ стирання чіпа",
                                                "рестарт пристрою",
                                                "після заміни банок:" };
    static const char *d2[NUM_BASE_ACTIONS] = { "заряд/розряд, знос",
                                                "дзеркало калібрув.",
                                                "моделі/ID/калібрув.",
                                                "DS2433 (крайній!)",
                                                "ESP32 (Wi-Fi/веб)",
                                                "стерти learned + ICA" };
    static const bool  dg[NUM_BASE_ACTIONS] = { false, false, false, true, false, false };
    int sel = g_actionSel;
    int total = numActions();

    const char *name, *l1, *l2; bool danger;
    char nbuf[26];
    if (sel < NUM_BASE_ACTIONS) {
        name = nm[sel]; l1 = d1[sel]; l2 = d2[sel]; danger = dg[sel];
    } else {
        int ti = sel - NUM_BASE_ACTIONS;
        snprintf(nbuf, sizeof(nbuf), "Новий %s", BATTERY_TEMPLATES[ti].name);
        name = nbuf; l1 = "ініціаліз. порожній"; l2 = "чіп як новий АКБ"; danger = true;
    }

    char t[20]; snprintf(t, sizeof(t), "Дія %d/%d", sel + 1, total);
    drawHeaderBar(t);

    // Картка операції (у межах безпечної зони кутів).
    int cardx = EDGE > 10 ? EDGE - 4 : 10;
    int txtx  = cardx + 10;
    uint16_t accent = danger ? C_RED : C_GREEN;
    tft.drawRoundRect(cardx, 44, TFT_W - 2 * cardx, 96, 6, accent);
    tSet(FONT_MODEL, accent);
    tPut(txtx, 78, name);
    tSet(FONT_BODY, C_TEXT);
    tPut(txtx, 106, l1);
    tPut(txtx, 128, l2);
    if (danger) {
        tSet(FONT_BODY, C_RED);
        tPut(txtx, 164, "!! НЕЗВОРОТНЬО !!");
    }

    // Підказка керування.
    tft.fillRect(0, FOOT_Y, TFT_W, FOOT_H, C_CARD);
    tft.drawFastHLine(0, FOOT_Y, TFT_W, C_BLUE);
    tSet(FONT_SMALL, C_MUTED, C_CARD);
    tPut(EDGE, TFT_H - 8, "[<] вибір   [<] тримати = ПУСК");
}

// ===================== Рендер і кнопки =====================

inline void displayRender() {
    tft.fillScreen(C_BG);
    switch (g_displayPage) {
        case 0:  drawPageMain();     break;
        case 1:  drawPageModel();    break;
        case 2:  drawPageTech();     break;
        case 3:  drawPageHealth();   break;
        case 4:  drawPageRaw2438();  break;
        case 5:  drawPageRaw2433();  break;
        case 6:  drawPageActions();  break;
        default: drawPageMain();     break;
    }
}

inline void displaySetStatus(const char *s) {
    strncpy(g_displayStatus, s, sizeof(g_displayStatus) - 1);
    g_displayStatus[sizeof(g_displayStatus) - 1] = '\0';
}

inline void displayShow(const char *s) {
    displaySetStatus(s);
    displayRender();
}

inline void displayInit() {
#ifdef DISPLAY_BLK_PIN
    pinMode(DISPLAY_BLK_PIN, OUTPUT);
    digitalWrite(DISPLAY_BLK_PIN, HIGH);      // підсвітка
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
    pinMode(MENU_BTN_PIN, INPUT_PULLUP);
    pinMode(MENU_BTN2_PIN, INPUT_PULLUP);
}

struct BtnState {
    bool stable = HIGH;
    bool lastRaw = HIGH;
    unsigned long tChange = 0;
    unsigned long tPress = 0;
    bool longFired = false;
};

inline int pollButton(int pin, BtnState &b, unsigned long longMs) {
    bool raw = digitalRead(pin);
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

inline bool displayConsumeReadRequest() {
    if (g_readRequested) { g_readRequested = false; return true; }
    return false;
}

inline int displayConsumeActionRequest() {
    int a = g_actionRequested; g_actionRequested = -1; return a;
}

inline void displayHandleButton() {
    static BtnState b1, b2;

    int e1 = pollButton(MENU_BTN_PIN, b1, 800);
    if (e1 == 2) {
        g_readRequested = true;
        displaySetStatus("ЗЧИТУВАННЯ...");
        displayRender();
    } else if (e1 == 1) {
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        displayRender();
    }

    int e2 = pollButton(MENU_BTN2_PIN, b2, 800);
    if (g_displayPage == RESET_PAGE) {
        if (e2 == 1) {
            g_actionSel = (g_actionSel + 1) % numActions();
            displayRender();
        } else if (e2 == 2) {
            g_actionRequested = g_actionSel;
            displaySetStatus("ВИКОНУЮ...");
            displayRender();
        }
    } else if (e2 == 1) {
        g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
        displayRender();
    }
}

#endif  // DISPLAY_COLOR_H
