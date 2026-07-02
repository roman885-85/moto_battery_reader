#ifndef DISPLAY_H
#define DISPLAY_H

#include <U8g2lib.h>
#include <Wire.h>
#include "settings.h"
#include "battery_reader.h"

// Стан, яке відображаємо (заповнюється з .ino і обробників веб-сервера).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump[DUMP_SIZE];
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern uint8_t chipSN2438[8];
extern bool hasSN2438;

// Сторінки меню (перегортаються кнопкою):
//   0 - головна (заряд + статус)
//   1 - модель і серійний номер
//   2 - технічні дані DS2438
//   3 - здоров'я: ємність / знос / цикли
//   4 - сирий дамп DS2438 (hex)
//   5 - сирий дамп DS2433 (перші 64/128 байт — за висотою екрана, hex)
//   6 - дії з АКБ (скидання / ремонт / очистка)
#define NUM_DISPLAY_PAGES 7
#define RESET_PAGE        6   // сторінка «Дії» (історична назва константи)

// Об'єкт дисплея вибирається по моделі з settings.h (повний буфер _F_).
// DISP_W/DISP_H — роздільність; DISPLAY_USES_I2C — ознака шини I2C.
//
// HW I2C: піни SCL/SDA передаються прямо в конструктор — U8g2 сама викликає
// Wire.begin(SDA, SCL). При увімкненому DISPLAY_HW_I2C додатково створюється
// запасний програмний (SW) об'єкт: якщо апаратна шина не відповідає (немає
// ACK — слабкі підтяжки, довгі проводи), displayInit автоматично
// перемикається на програмний I2C на тех же пінах. Кадровий буфер у пари
// об'єктів спільний (статичний в U8g2), зайвої пам'яті це майже не ест.
#if defined(DISPLAY_ST7567_SPI)
  #define DISP_W 128
  #define DISP_H 64
  // ST7567 (Open-Smart 1.8"), апаратний SPI (SCK=18, MOSI=23).
  // Варіант ENH_DG128064: коректна електрика (bias 1/9, помірний
  // контраст — варіант OS12864 з bias 1/7 дає темний засвічений екран).
  // Зсув картинки +4px робиться через x_offset в displayInit
  // (DISPLAY_ST7567_XOFF в settings.h).
  U8G2_ST7567_ENH_DG128064_F_4W_HW_SPI u8g2_spi(U8G2_R0, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
  static U8G2 *g_u8g2p = &u8g2_spi;
#elif defined(DISPLAY_PCD8544_SPI)
  #define DISP_W 84
  #define DISP_H 48
  // Nokia 5110 (PCD8544), 84x48, апаратний SPI.
  U8G2_PCD8544_84X48_F_4W_HW_SPI u8g2_spi(U8G2_R0, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
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
static bool g_readRequested = false;       // запит повторного читання після циклу
// Сторінка «Дії»: вибір операції (BTN2 коротко) + виконання (BTN2 довго).
static int  g_actionSel = 0;               // 0=Скидання 1=Ремонт 2=Очистка
static int  g_actionRequested = -1;        // -1 нема; інакше — обрана дія для .ino

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
#if defined(DISPLAY_CONTRAST)
    u8g2.setContrast(DISPLAY_CONTRAST);
#endif
    u8g2.setFont(BODY_FONT);
    Serial.printf("DISPLAY: %dx%d, I2C clock=%lu Hz\n", (int)DISP_W, (int)DISP_H, (unsigned long)DISPLAY_CLK_HZ);
}

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

inline void displaySetStatus(const char *s) {
    strncpy(g_displayStatus, s, sizeof(g_displayStatus) - 1);
    g_displayStatus[sizeof(g_displayStatus) - 1] = '\0';
}

inline void displayShow(const char *s) {
    displaySetStatus(s);
    displayRender();
}

// ---------- допоміжні елементи відмальовування ----------

inline void drawHeader(const char *title) {
    char h[16];
    u8g2.setFont(HEAD_FONT);
    u8g2.drawUTF8(0, HEAD_Y, title);
    snprintf(h, sizeof(h), "%d/%d", g_displayPage + 1, NUM_DISPLAY_PAGES);
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
    u8g2.drawFrame(x, y, w, h);
    u8g2.drawBox(x + w, y + h / 3, 3, h - 2 * (h / 3)); // "плюсовий" вивід
    if (pct < 0) return;
    int fillw = (w - 4) * pct / 100;
    if (fillw < 0) fillw = 0;
    if (fillw > w - 4) fillw = w - 4;
    if (fillw > 0) u8g2.drawBox(x + 2, y + 2, fillw, h - 4);
}

// Відсоток заряду. Пріоритет — ICA (якщо увімкнений облік струму IAD=1),
// інакше оцінка по напрузі. src отримує мітку джерела ("ICA"/"volt"/"--").
inline int batteryPercent(const char **src) {
    if (!hasDump2438) { *src = "--"; return -1; }

    uint8_t config = batteryDump2438[0];       // стр.0 байт0 = Status/Config
    if (config & 0x01) {                        // IAD=1 -> ICA підтримується
        *src = "ICA";
        int pct = (int)batteryDump2438[12] * 100 / ICA_FULL_SCALE; // стр.1 байт4 = ICA
        return pct > 100 ? 100 : pct;
    }

    *src = "volt";                              // запасний варіант — по напрузі
    long vmv = (long)(((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3]) * 10;
    long pct = (vmv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int)pct;
}

// Знайти модель (part number, напр. "PMNN4409A"/"PT4409A") в дампі DS2433.
// Пріоритет — авторитетна запис моделі: тег 0x0B, потім ASCII-назва,
// що починається з 'P' (перевірено на усіх дампах: 0B 50 4D 4E 4E ... = "PMNN...";
// 0B 50 54 ... = "PT..."). Запасний шлях — компактна алфавітно-цифрова
// рядок 7..11 з цифрою (відсікає "COPYRIGHT...MOTOROLASOLUTIONS").
inline bool decodeModel(char *out, size_t n) {
    if (!hasDump) return false;

    // 1) Запис 0x0B c назвою моделі (0x0B 'P' ...).
    for (int i = 0x100; i < (int)DUMP_SIZE - 12; i++) {
        if (batteryDump[i] == 0x0B && batteryDump[i + 1] == 'P') {
            int j = i + 1, len = 0;
            char tmp[16];
            while (j < (int)DUMP_SIZE && len < 12) {
                uint8_t d = batteryDump[j];
                if ((d >= '0' && d <= '9') || (d >= 'A' && d <= 'Z')) { tmp[len++] = (char)d; j++; }
                else break;
            }
            // валідна модель: довжина 6..11 і є цифра
            bool digit = false;
            for (int k = 0; k < len; k++) if (tmp[k] >= '0' && tmp[k] <= '9') digit = true;
            if (len >= 6 && len <= 11 && digit) {
                if ((size_t)len >= n) len = n - 1;
                memcpy(out, tmp, len); out[len] = '\0';
                return true;
            }
        }
    }

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

// Ємність/знос з записи історії ємності DS2433 (тег/довжина 0x17, "17 00 ...").
// Формат записи: [0x17][22 байта даних][контрольна сума]; останній байт
// даних (перед CRC) — найбільш свіже значення ємності в % (перевірено на дампах
// робочих і залочених PMNN4409/4493). Знос = 100 - ємність.
inline bool decodeCapacity(int *capPct, int *wearPct) {
    if (!hasDump) return false;
    for (int i = 0x100; i < (int)DUMP_SIZE - 23; i++) {
        if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) {
            int cap = batteryDump[i + 21];   // останнє значення ємності, %
            if (cap <= 100) {
                *capPct = cap;
                *wearPct = 100 - cap;
                return true;
            }
        }
    }
    return false;
}

// Евристика справжності / ризику блокування. Перевірено на дампах робочих
// (6 шт.) і залочених (2 шт.) PMNN4409/4493 — чітко розділяє їх. Ознаки
// залоченої/підробленої після заміни елементів АКБ:
//   - відсутній блок автентифікації "MOTOROLA..." (0xDF+ стертий);
//   - стерта калібрувальна підпис 0x1B-0x1E (усі FF);
//   - переповнений лічильник заряду CCA (0xFFFF) в DS2438.
// Повертає true, якщо ознак блокування немає; в reason — стисла причина.
inline bool batteryGenuine(const char **reason) {
    if (!hasDump) { *reason = "нема дампу"; return false; }

    bool auth = false;
    static const char pat[] = "MOTOROLA";
    const int plen = 8;
    for (int i = 0; i + plen <= (int)DUMP_SIZE && !auth; i++) {
        int k = 0;
        while (k < plen && batteryDump[i + k] == (uint8_t)pat[k]) k++;
        if (k == plen) auth = true;
    }
    if (!auth) { *reason = "нема автент."; return false; }

    if (batteryDump[0x1B] == 0xFF && batteryDump[0x1C] == 0xFF &&
        batteryDump[0x1D] == 0xFF && batteryDump[0x1E] == 0xFF) {
        *reason = "нема калібр.";
        return false;
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) { *reason = "CCA перепов."; return false; }
    }

    *reason = "OK";
    return true;
}

// ---------- сторінки меню ----------

// залишкова ємність (заряд) в мА·ч з регістра ICA (DS2438 байт 12).
inline int batteryRemainingMah() {
    if (!hasDump2438) return -1;
    return (int)(batteryDump2438[12] * DS2438_MAH_PER_LSB);
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
        snprintf(buf, sizeof(buf), "%.2f V   %.1f C", vraw * 0.01f, traw * 0.03125f);
    } else snprintf(buf, sizeof(buf), "DS2438: немає даних");
    u8g2.drawUTF8(0, 84, buf);
    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);        u8g2.drawUTF8(0, 98, buf);
    u8g2.drawUTF8(0, 112, "[>] довго - зчитати");
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

    int remMah = (int)(rem * DS2438_MAH_PER_LSB);   // залишок в мА*ч

    u8g2.setFont(BODY_FONT);
    snprintf(buf, sizeof(buf), "Напруга:  %.2f V", vraw * 0.01f);      u8g2.drawUTF8(0, ROW(0), buf);
    snprintf(buf, sizeof(buf), "Струм:    %.0f mA", i_mA);             u8g2.drawUTF8(0, ROW(1), buf);
    snprintf(buf, sizeof(buf), "Темп:     %.1f C", traw * 0.03125f);   u8g2.drawUTF8(0, ROW(2), buf);
    snprintf(buf, sizeof(buf), "Залишок: ~%d mAh", remMah);            u8g2.drawUTF8(0, ROW(3), buf);

    drawFooter();
}

inline void drawPageHealth() {
    char buf[48];
    drawHeader("Стан АКБ");
    u8g2.setFont(BODY_FONT);

    int cap, wear;
    if (decodeCapacity(&cap, &wear)) {
        snprintf(buf, sizeof(buf), "Ємність: %d %%", cap);  u8g2.drawUTF8(0, ROW(0), buf);
        snprintf(buf, sizeof(buf), "Знос:    %d %%", wear); u8g2.drawUTF8(0, ROW(1), buf);
    } else {
        u8g2.drawUTF8(0, ROW(0), "Ємність: (зчитайте)");
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        uint16_t dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        // Цикли: сумарний заряд (розряд) / паспортна ємність (settings.h).
        int chgCyc = (int)(cca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH);
        int disCyc = (int)(dca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH);
        snprintf(buf, sizeof(buf), "Циклів: зар.%d роз.%d", chgCyc, disCyc);
        u8g2.drawUTF8(0, ROW(2), buf);
    }

    const char *reason;
    if (batteryGenuine(&reason)) {
        u8g2.drawUTF8(0, ROW(3), "Справжня: ТАК");
    } else {
        snprintf(buf, sizeof(buf), "РИЗИК: %s", reason);
        u8g2.drawUTF8(0, ROW(3), buf);
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

// Сторінка «Дії»: 3 операції із записом у чіп. Вибір — коротке [<], виконання —
// довге утримання [<] (свідоме підтвердження). [>] — вихід на наступну сторінку.
inline void drawPageActions() {
    static const char *names[3] = { "Скидання", "Ремонт", "Очистка" };
    static const char *desc[3]  = { "лічильники/знос->свіжі",
                                    "полагодити суми/дзеркало",
                                    "стерти все, крім ID" };
    drawHeader("Дії з АКБ");
    u8g2.setFont(BODY_FONT);
    for (int i = 0; i < 3; i++) {
        char line[40];
        snprintf(line, sizeof(line), "%c %s", (i == g_actionSel ? '>' : ' '), names[i]);
        u8g2.drawUTF8(0, ROW(i), line);
    }
    u8g2.drawUTF8(0, ROW(3), desc[g_actionSel]);
#if DISP_H >= 128
    u8g2.drawUTF8(0, ROW(4), "[<] вибір · утрим.=пуск");
    u8g2.drawUTF8(0, ROW(5), "[>] далі");
#endif
    // У підвалі — підказка керування (на 64px там статус, тож коротко).
    drawFooter();
}

// ---------- рендер і кнопка ----------

inline void displayRender() {
    u8g2.clearBuffer();
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
    u8g2.sendBuffer();
}

inline void displayButtonSetup() {
    pinMode(MENU_BTN_PIN, INPUT_PULLUP);
    pinMode(MENU_BTN2_PIN, INPUT_PULLUP);
}

// Стан однієї кнопки для антидребезгу + розпізнавання довгого натискання.
struct BtnState {
    bool stable = HIGH;        // стійкий (антидребезг) рівень
    bool lastRaw = HIGH;
    unsigned long tChange = 0; // час останнього зміни сирого рівня
    unsigned long tPress = 0;  // час натискання
    bool longFired = false;
};

// Опитування кнопки. Повертає: 0 — нічого, 1 — коротке (по відпусканню),
// 2 — довге (при утриманні longMs). longMs=0 вимикає довге натискання.
inline int pollButton(int pin, BtnState &b, unsigned long longMs) {
    bool raw = digitalRead(pin);
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
inline void displayHandleButton() {
    static BtnState b1, b2;

    int e1 = pollButton(MENU_BTN_PIN, b1, 800);
    if (e1 == 2) {                                   // довге -> перечитати
        g_readRequested = true;
        displaySetStatus("ЗЧИТУВАННЯ...");
        displayRender();
    } else if (e1 == 1) {                            // коротке -> наступна сторінка
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        displayRender();
    }

    int e2 = pollButton(MENU_BTN2_PIN, b2, 800);
    if (g_displayPage == RESET_PAGE) {               // сторінка «Дії»
        if (e2 == 1) {                               // коротке -> наступна операція
            g_actionSel = (g_actionSel + 1) % 3;
            displayRender();
        } else if (e2 == 2) {                        // довге -> виконати обране
            g_actionRequested = g_actionSel;
            displaySetStatus("ВИКОНУЮ...");
            displayRender();
        }
    } else if (e2 == 1) {                            // інші сторінки: коротке -> назад
        g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
        displayRender();
    }
}

#endif
