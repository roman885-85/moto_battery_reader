#ifndef DISPLAY_H
#define DISPLAY_H

#include <U8g2lib.h>
#include <Wire.h>
#include "settings.h"
#include "battery_reader.h"

// Состояние, которое отображаем (заполняется из .ino и обработчиков веб-сервера).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump[DUMP_SIZE];
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern uint8_t chipSN2438[8];
extern bool hasSN2438;

// Страницы меню (перелистываются кнопкой):
//   0 - главная (заряд + статус)
//   1 - модель и серийный номер
//   2 - технические данные DS2438
//   3 - здоровье: ёмкость / износ / циклы
//   4 - сырой дамп DS2438 (hex)
//   5 - сырой дамп DS2433 (первые 64/128 байт — по высоте экрана, hex)
//   6 - сброс счётчиков (рекалибровка)
#define NUM_DISPLAY_PAGES 7
#define RESET_PAGE        6

// Объект дисплея выбирается по модели из settings.h (полный буфер _F_).
// DISP_W/DISP_H — разрешение; DISPLAY_USES_I2C — признак шины I2C.
//
// HW I2C: пины SCL/SDA передаются прямо в конструктор — U8g2 сама вызывает
// Wire.begin(SDA, SCL). При включённом DISPLAY_HW_I2C дополнительно создаётся
// запасной программный (SW) объект: если аппаратная шина не отвечает (нет
// ACK — слабые подтяжки, длинные провода), displayInit автоматически
// переключается на программный I2C на тех же пинах. Кадровый буфер у пары
// объектов общий (статический в U8g2), лишней памяти это почти не ест.
#if defined(DISPLAY_ST7567_SPI)
  #define DISP_W 128
  #define DISP_H 64
  // ST7567 (Open-Smart 1.8"), аппаратный SPI (SCK=18, MOSI=23).
  // Вариант ENH_DG128064: корректная электрика (bias 1/9, умеренный
  // контраст — вариант OS12864 с bias 1/7 даёт тёмный засвеченный экран).
  // Сдвиг картинки +4px делается через x_offset в displayInit
  // (DISPLAY_ST7567_XOFF в settings.h).
  U8G2_ST7567_ENH_DG128064_F_4W_HW_SPI u8g2_spi(U8G2_R0, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
  static U8G2 *g_u8g2p = &u8g2_spi;
#elif defined(DISPLAY_PCD8544_SPI)
  #define DISP_W 84
  #define DISP_H 48
  // Nokia 5110 (PCD8544), 84x48, аппаратный SPI.
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
  U8G2_SSD1327_MIDAS_128X128_F_SW_I2C u8g2_sw(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
  #if defined(DISPLAY_HW_I2C)
    U8G2_SSD1327_MIDAS_128X128_F_HW_I2C u8g2_hw(U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);
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
#else   // DISPLAY_SSD1306_I2C — по умолчанию
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

// Весь код ниже работает через указатель: обращения "u8g2." идут к активному
// объекту (аппаратному либо запасному программному после fallback).
#define u8g2 (*g_u8g2p)

// Частота пробы аппаратной шины = рабочая частота дисплея.
#if DISPLAY_I2C_KHZ > 0
  #define DISPLAY_PROBE_HZ (DISPLAY_I2C_KHZ * 1000UL)
#elif defined(DISPLAY_SSD1327_128_I2C)
  #define DISPLAY_PROBE_HZ 100000UL   // предел SSD1327
#else
  #define DISPLAY_PROBE_HZ 400000UL
#endif

// Разметка меню под разрешение экрана: шрифты, шапка, строки тела (ROW),
// нижняя строка статуса. На 128x128 — крупнее шрифт и больше строк.
#if DISP_H >= 128            // 128x128 (GME128128-02 и т.п.)
  #define HEAD_FONT  u8g2_font_6x12_t_cyrillic
  #define HEAD_Y     10                // базовая линия заголовка
  #define HEAD_LINE  13                // Y разделительной линии шапки
  #define BODY_FONT  u8g2_font_6x12_t_cyrillic
  #define BODY_Y0    27                // базовая линия первой строки тела
  #define ROW_H      14                // шаг строк
  #define FOOT_HL    (DISP_H - 15)     // линия над статусом
  #define FOOT_Y     (DISP_H - 3)      // базовая линия статуса
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
// Базовая линия n-й строки тела страницы.
#define ROW(n) (BODY_Y0 + (n) * ROW_H)

static char g_displayStatus[36] = "ЗАПУСК";  // нижняя строка статуса (UTF-8)
static int  g_displayPage = 0;             // текущая страница меню
static bool g_readRequested = false;       // запрос повторного чтения после цикла
static bool g_resetRequested = false;      // запрос сброса из меню (кнопкой)
static bool g_resetArmed = false;          // сброс "взведён" (первое нажатие)
static unsigned long g_resetArmedAt = 0;   // время взведения (авто-сброс через 5с)

inline void displayRender(); // определение ниже

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

// Пересчёт контрольной суммы TLV-записи DS2433: сумма всех байт == 0x5A.
inline void fixRecordChecksum(uint8_t *buf, int start, int len) {
    int s = 0;
    for (int k = 0; k < len - 1; k++) s += buf[start + k];
    buf[start + len - 1] = (0x5A - s) & 0xFF;
}

// ---------- базовая настройка ----------

inline void displayInit() {
#if defined(DISPLAY_USES_I2C) && defined(DISPLAY_HW_I2C)
    // Проба аппаратной шины НА РАБОЧЕЙ ЧАСТОТЕ до u8g2.begin(). Повторный
    // Wire.begin теми же пинами внутри U8g2 безвреден ("already started").
    // Если дисплей не отвечает (нет ACK: слабые подтяжки, длинные провода,
    // капризный контроллер) — освобождаем пины и автоматически переходим
    // на запасной ПРОГРАММНЫЙ I2C: дисплей работает всегда, только медленнее.
    Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, DISPLAY_PROBE_HZ);
    Wire.setTimeOut(50);
    bool hwOk = true;
    for (int i = 0; i < 3 && hwOk; i++) {
        Wire.beginTransmission(DISPLAY_I2C_ADDR);
        hwOk = (Wire.endTransmission() == 0);
    }
    if (!hwOk) {
        Wire.end();                 // отдать пины бит-бэнгу
        g_u8g2p = &u8g2_sw;         // запасной программный I2C
        Serial.printf("DISPLAY: HW I2C no ACK at 0x%02X (SDA=%d SCL=%d) -> SW I2C fallback\n",
                      DISPLAY_I2C_ADDR, DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    } else {
        Serial.println("DISPLAY: HW I2C OK");
    }
#endif
#if defined(DISPLAY_USES_I2C)
    u8g2.setI2CAddress(DISPLAY_I2C_ADDR << 1);
  #if DISPLAY_I2C_KHZ > 0
    // Ручное задание частоты. 0 (по умолчанию) = авто: U8g2 берёт безопасный
    // максимум из драйвера дисплея (SSD1327 держит только 100 кГц!).
    u8g2.setBusClock(DISPLAY_I2C_KHZ * 1000UL);
  #endif
#endif
    u8g2.begin();
#if defined(DISPLAY_ST7567_SPI)
    // Панель Open-Smart: RAM ST7567 на 132 колонки, стекло показывает 4..131.
    u8g2.getU8x8()->x_offset = DISPLAY_ST7567_XOFF;
#endif
#if defined(DISPLAY_CONTRAST)
    u8g2.setContrast(DISPLAY_CONTRAST);
#endif
    u8g2.setFont(BODY_FONT);
}

// Стартовая заставка: тризуб + "Національна Гвардія України".
inline void displaySplash() {
    u8g2.clearBuffer();
#if DISP_H >= 128
    // 128x128: эмблема по центру сверху, текст под ней.
    u8g2.drawXBM((DISP_W - NGU_W) / 2, 6, NGU_W, NGU_H, ngu_xbm);
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    u8g2.drawUTF8(7,  88,  "Національна Гвардія");   // 19 зн. x 6px = 114
    u8g2.drawUTF8(43, 103, "України");                //  7 зн. x 6px = 42
    u8g2.drawUTF8(31, 121, "IMPRES tool");            // 11 зн. x 6px = 66
#elif (DISP_H >= 64) && (DISP_W >= 110)
    // 128x64: эмблема слева + текст справа.
    u8g2.drawXBM(0, 0, NGU_W, NGU_H, ngu_xbm);
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(66, 18, "Національна");
    u8g2.drawUTF8(66, 34, "Гвардія");
    u8g2.drawUTF8(66, 50, "України");
#else
    // Мелкие экраны (Nokia 5110): только текст.
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

// ---------- вспомогательные элементы отрисовки ----------

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

// Иконка батареи со шкалой заполнения; pct<0 — данных нет.
inline void drawBatteryIcon(int x, int y, int w, int h, int pct) {
    u8g2.drawFrame(x, y, w, h);
    u8g2.drawBox(x + w, y + h / 3, 3, h - 2 * (h / 3)); // "плюсовой" вывод
    if (pct < 0) return;
    int fillw = (w - 4) * pct / 100;
    if (fillw < 0) fillw = 0;
    if (fillw > w - 4) fillw = w - 4;
    if (fillw > 0) u8g2.drawBox(x + 2, y + 2, fillw, h - 4);
}

// Процент заряда. Приоритет — ICA (если включён учёт тока IAD=1),
// иначе оценка по напряжению. src получает метку источника ("ICA"/"volt"/"--").
inline int batteryPercent(const char **src) {
    if (!hasDump2438) { *src = "--"; return -1; }

    uint8_t config = batteryDump2438[0];       // стр.0 байт0 = Status/Config
    if (config & 0x01) {                        // IAD=1 -> ICA поддерживается
        *src = "ICA";
        int pct = (int)batteryDump2438[12] * 100 / ICA_FULL_SCALE; // стр.1 байт4 = ICA
        return pct > 100 ? 100 : pct;
    }

    *src = "volt";                              // запасной вариант — по напряжению
    long vmv = (long)(((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3]) * 10;
    long pct = (vmv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int)pct;
}

// Найти модель (Motorola part number, напр. "PMNN4409A") в дампе DS2433.
// Ищем компактную строку из заглавных букв и цифр длиной 7..11, содержащую
// цифру, — так отсекаем длинный блок "COPYRIGHT...MOTOROLASOLUTIONS".
inline bool decodeModel(char *out, size_t n) {
    if (!hasDump) return false;
    int best = -1, bestLen = 0;
    int i = 0;
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
        } else {
            i++;
        }
    }
    if (best < 0) return false;
    int len = bestLen;
    if ((size_t)len >= n) len = n - 1;
    memcpy(out, batteryDump + best, len);
    out[len] = '\0';
    return true;
}

// Ёмкость/износ из записи истории ёмкости DS2433 (тег/длина 0x17, "17 00 ...").
// Формат записи: [0x17][22 байта данных][контрольная сумма]; последний байт
// данных (перед CRC) — самое свежее значение ёмкости в % (проверено на дампах
// рабочих и залоченных PMNN4409/4493). Износ = 100 - ёмкость.
inline bool decodeCapacity(int *capPct, int *wearPct) {
    if (!hasDump) return false;
    for (int i = 0x100; i < (int)DUMP_SIZE - 23; i++) {
        if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) {
            int cap = batteryDump[i + 21];   // последнее значение ёмкости, %
            if (cap <= 100) {
                *capPct = cap;
                *wearPct = 100 - cap;
                return true;
            }
        }
    }
    return false;
}

// Эвристика подлинности / риска блокировки. Проверено на дампах рабочих
// (6 шт.) и залоченных (2 шт.) PMNN4409/4493 — чётко разделяет их. Признаки
// залоченной/подделанной после замены элементов АКБ:
//   - отсутствует блок аутентификации "MOTOROLA..." (0xDF+ стёрт);
//   - стёрта калибровочная подпись 0x1B-0x1E (все FF);
//   - переполнен счётчик заряда CCA (0xFFFF) в DS2438.
// Возвращает true, если признаков блокировки нет; в reason — краткая причина.
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

// ---------- страницы меню ----------

inline void drawPageMain() {
    char buf[40];
    const char *src;
    int pct = batteryPercent(&src);

    drawHeader("Moto IMPRES");

#if DISP_H >= 128
    // 128x128: крупная иконка и проценты, ниже — параметры и подсказка.
    drawBatteryIcon(0, 22, 60, 22, pct);
    u8g2.setFont(u8g2_font_10x20_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(70, 41, buf);
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(70, 56, src);                  // источник данных заряда

    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2f V   %.1f C", vraw * 0.01f, traw * 0.03125f);
    } else {
        snprintf(buf, sizeof(buf), "DS2438: немає даних");
    }
    u8g2.drawUTF8(0, 76, buf);

    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);
    u8g2.drawUTF8(0, 92, buf);
    u8g2.drawUTF8(0, 106, "[>] довго - зчитати");
#elif DISP_W < 100
    // Nokia 84x48: компактно, без источника данных (не влезает).
    drawBatteryIcon(0, 12, 38, 12, pct);
    u8g2.setFont(u8g2_font_6x12_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(46, 22, buf);

    u8g2.setFont(BODY_FONT);
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2fV %.1fC", vraw * 0.01f, traw * 0.03125f);
    } else {
        snprintf(buf, sizeof(buf), "2438: немає");
    }
    u8g2.drawUTF8(0, 30, buf);
    snprintf(buf, sizeof(buf), "%s", ESP_IP);
    u8g2.drawUTF8(0, 38, buf);
#else
    // 128x64.
    drawBatteryIcon(0, 13, 52, 14, pct);
    u8g2.setFont(u8g2_font_6x12_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(58, 24, buf);
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(90, 23, src);                  // источник данных заряда

    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2f V   %.1f C", vraw * 0.01f, traw * 0.03125f);
    } else {
        snprintf(buf, sizeof(buf), "DS2438: немає даних");
    }
    u8g2.drawUTF8(0, 40, buf);

    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);
    u8g2.drawUTF8(0, 49, buf);
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
    drawFooter();   // на малых экранах не влезает — там страница без статуса
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

    int remMah = (int)(rem * DS2438_MAH_PER_LSB);   // остаток в мА*ч

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
        // Циклы: суммарный заряд (разряд) / паспортная ёмкость (settings.h).
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

// Общая отрисовка сырого дампа (hex), шрифт 4x6. На узких экранах (Nokia)
// по 6 байт в строке, иначе по 8; глубина — сколько влезает по высоте.
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

// На 128x128 влезает вдвое больше дампа DS2433.
#define RAW2433_COUNT ((DISP_H >= 128) ? 128 : 64)
inline void drawPageRaw2438() { drawRawPage("DS2438 дамп 0-63", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawPage((DISP_H >= 128) ? "DS2433 дамп 0-127" : "DS2433 дамп 0-63",
                                            batteryDump, hasDump, RAW2433_COUNT); }

inline void drawPageReset() {
    drawHeader("Скидання");
    u8g2.setFont(BODY_FONT);
    u8g2.drawUTF8(0, ROW(0), "Скинути лічильники");
    u8g2.drawUTF8(0, ROW(1), "(цикли/знос) для");
    u8g2.drawUTF8(0, ROW(2), "рекалібрування.");
    if (g_resetArmed)
        u8g2.drawUTF8(0, ROW(3) + 2, "Ще раз [<] = СКИДАННЯ!");
    else
        u8g2.drawUTF8(0, ROW(3) + 2, "[<] двічі = скидання");
    drawFooter();
}

// ---------- рендер и кнопка ----------

inline void displayRender() {
    u8g2.clearBuffer();
    switch (g_displayPage) {
        case 0:  drawPageMain();     break;
        case 1:  drawPageModel();    break;
        case 2:  drawPageTech();     break;
        case 3:  drawPageHealth();   break;
        case 4:  drawPageRaw2438();  break;
        case 5:  drawPageRaw2433();  break;
        case 6:  drawPageReset();    break;
        default: drawPageMain();     break;
    }
    u8g2.sendBuffer();
}

inline void displayButtonSetup() {
    pinMode(MENU_BTN_PIN, INPUT_PULLUP);
    pinMode(MENU_BTN2_PIN, INPUT_PULLUP);
}

// Состояние одной кнопки для антидребезга + распознавания долгого нажатия.
struct BtnState {
    bool stable = HIGH;        // устойчивый (антидребезг) уровень
    bool lastRaw = HIGH;
    unsigned long tChange = 0; // время последнего изменения сырого уровня
    unsigned long tPress = 0;  // время нажатия
    bool longFired = false;
};

// Опрос кнопки. Возвращает: 0 — ничего, 1 — короткое (по отпусканию),
// 2 — долгое (при удержании longMs). longMs=0 отключает долгое нажатие.
inline int pollButton(int pin, BtnState &b, unsigned long longMs) {
    bool raw = digitalRead(pin);
    unsigned long now = millis();
    if (raw != b.lastRaw) { b.lastRaw = raw; b.tChange = now; }

    int ev = 0;
    if (now - b.tChange > 25 && raw != b.stable) {   // устойчивое изменение
        b.stable = raw;
        if (b.stable == LOW) {                        // нажатие
            b.tPress = now;
            b.longFired = false;
        } else {                                      // отпускание
            if (!b.longFired) ev = 1;                 // короткое (если не было долгого)
        }
    }
    if (b.stable == LOW && longMs && !b.longFired && now - b.tPress >= longMs) {
        b.longFired = true;
        ev = 2;                                        // долгое (единожды)
    }
    return ev;
}

// true один раз после того, как кнопка провернула меню на полный круг.
inline bool displayConsumeReadRequest() {
    if (g_readRequested) { g_readRequested = false; return true; }
    return false;
}

// true один раз, когда пользователь подтвердил сброс из меню (двойное [<]).
inline bool displayConsumeResetRequest() {
    if (g_resetRequested) { g_resetRequested = false; return true; }
    return false;
}

// Опрос кнопок. BTN1: короткое — следующая страница, долгое (0.8с) — повторное
// чтение АКБ. BTN2: короткое — назад, а на странице сброса — взвод/подтверждение.
inline void displayHandleButton() {
    static BtnState b1, b2;

    int e1 = pollButton(MENU_BTN_PIN, b1, 800);
    if (e1 == 2) {                                   // долгое -> перечитать
        g_resetArmed = false;
        g_readRequested = true;
        displaySetStatus("ЗЧИТУВАННЯ...");
        displayRender();
    } else if (e1 == 1) {                            // короткое -> следующая страница
        g_resetArmed = false;
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        displayRender();
    }

    int e2 = pollButton(MENU_BTN2_PIN, b2, 0);
    if (e2 == 1) {
        if (g_displayPage == RESET_PAGE) {           // взвод -> подтверждение сброса
            if (!g_resetArmed) { g_resetArmed = true; g_resetArmedAt = millis(); }
            else               { g_resetArmed = false; g_resetRequested = true; }
            displayRender();
        } else {                                     // назад
            g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
            displayRender();
        }
    }

    // Авто-снятие взвода сброса через 5 с без подтверждения.
    if (g_resetArmed && millis() - g_resetArmedAt > 5000) {
        g_resetArmed = false;
        if (g_displayPage == RESET_PAGE) displayRender();
    }
}

#endif
