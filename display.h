#ifndef DISPLAY_H
#define DISPLAY_H

#include <U8g2lib.h>
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
//   5 - сырой дамп DS2433 (первые 64 байта, hex)
//   6 - сброс счётчиков (рекалибровка)
#define NUM_DISPLAY_PAGES 7
#define RESET_PAGE        6

// Объект дисплея. Программный (bit-bang) I2C — работает на любых GPIO,
// пины берутся из settings.h. Полный буфер кадра (_F_) = 1 КБ ОЗУ.
#if defined(DISPLAY_SH1106)
  U8G2_SH1106_128X64_NONAME_F_SW_I2C  u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#endif

static char g_displayStatus[36] = "ЗАПУСК";  // нижняя строка статуса (UTF-8)
static int  g_displayPage = 0;             // текущая страница меню
static bool g_readRequested = false;       // запрос повторного чтения после цикла
static bool g_resetRequested = false;      // запрос сброса из меню (кнопкой)
static bool g_resetArmed = false;          // сброс "взведён" (первое нажатие)
static unsigned long g_resetArmedAt = 0;   // время взведения (авто-сброс через 5с)

inline void displayRender(); // определение ниже

// Емблема Національної Гвардії України для стартової заставки (1-біт XBM).
#define NGU_W 46
#define NGU_H 64
static const unsigned char ngu_xbm[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x50, 0x02, 0x00, 0x00, 0x00, 0x00, 0xD8, 0x02, 0x00, 0x00,
  0x00, 0x00, 0xEC, 0x27, 0x00, 0x00, 0x00, 0x80, 0xFD, 0x77, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0xA0, 0xDF, 0x7F, 0x00, 0x00,
  0x00, 0xE0, 0xFF, 0x7F, 0x0C, 0x00, 0x00, 0xB0, 0xFF, 0xBF, 0x0D, 0x00,
  0x00, 0xB0, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0xBF, 0xFB, 0xFF, 0x0F, 0x00,
  0x00, 0x9E, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0xDF, 0xFF, 0xFF, 0x3F, 0x00,
  0xC0, 0xDF, 0xFF, 0xEF, 0x3F, 0x00, 0xC0, 0xFD, 0xEF, 0xFF, 0x1F, 0x00,
  0x80, 0xDF, 0xCF, 0xFF, 0x1F, 0x00, 0x80, 0x9F, 0x9F, 0xFE, 0x7F, 0x00,
  0x80, 0x1F, 0xBF, 0xFD, 0xFF, 0x01, 0xE0, 0x3F, 0x7E, 0xFF, 0xFF, 0x01,
  0xC0, 0xFF, 0xFE, 0xFB, 0xFF, 0x01, 0x80, 0xFF, 0xFD, 0x7F, 0xEE, 0x00,
  0x00, 0xFF, 0xF9, 0xFF, 0x3F, 0x00, 0x00, 0xFE, 0xFF, 0x1F, 0x0F, 0x00,
  0x00, 0x0C, 0xFE, 0x1F, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x1F, 0x00, 0x00,
  0x80, 0x03, 0xFC, 0x07, 0x78, 0x00, 0x00, 0x7F, 0xFC, 0x87, 0x3F, 0x00,
  0x00, 0x7F, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xFE, 0x07, 0x70, 0x1F, 0x00,
  0x00, 0xFE, 0x01, 0xC0, 0x1F, 0x00, 0x00, 0x7E, 0x00, 0x80, 0x1D, 0x00,
  0x00, 0x3E, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x1C, 0x02, 0x10, 0x0E, 0x00,
  0x00, 0x1C, 0x06, 0x18, 0x04, 0x00, 0x00, 0x0C, 0x02, 0x10, 0x0C, 0x00,
  0x00, 0x06, 0x0A, 0x14, 0x08, 0x00, 0x80, 0x07, 0x0A, 0x14, 0x78, 0x00,
  0xE0, 0x07, 0x02, 0x14, 0xF8, 0x01, 0xF0, 0x06, 0x02, 0x14, 0xF8, 0x03,
  0xFE, 0x03, 0x02, 0x10, 0xF8, 0x0F, 0xFE, 0x03, 0xCA, 0x14, 0xF8, 0x1F,
  0xF0, 0x07, 0x44, 0x18, 0xF8, 0x01, 0xC0, 0x07, 0x2A, 0x15, 0xF8, 0x00,
  0x80, 0x06, 0x32, 0x17, 0x78, 0x00, 0x00, 0x04, 0x82, 0x10, 0x18, 0x00,
  0x00, 0x0C, 0x5A, 0x13, 0x0C, 0x00, 0x00, 0x0C, 0x00, 0x02, 0x0C, 0x00,
  0x00, 0x1C, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x3E, 0xC0, 0x01, 0x0F, 0x00,
  0x00, 0x7E, 0xC0, 0x80, 0x0F, 0x00, 0x00, 0xFE, 0x00, 0xC0, 0x0F, 0x00,
  0x00, 0xFF, 0x03, 0xF0, 0x0F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x1F, 0x00,
  0x00, 0x3F, 0xF8, 0x0F, 0x3A, 0x00, 0x80, 0x03, 0xF8, 0x07, 0x30, 0x00,
  0x00, 0x00, 0xF0, 0x03, 0x40, 0x00, 0x00, 0x00, 0xF0, 0x03, 0x00, 0x00,
  0x00, 0x00, 0xE0, 0x01, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Пересчёт контрольной суммы TLV-записи DS2433: сумма всех байт == 0x5A.
inline void fixRecordChecksum(uint8_t *buf, int start, int len) {
    int s = 0;
    for (int k = 0; k < len - 1; k++) s += buf[start + k];
    buf[start + len - 1] = (0x5A - s) & 0xFF;
}

// ---------- базовая настройка ----------

inline void displayInit() {
    u8g2.setI2CAddress(DISPLAY_I2C_ADDR << 1);
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
}

// Стартовая заставка: тризуб + "Національна Гвардія України".
inline void displaySplash() {
    u8g2.clearBuffer();
    u8g2.drawXBM(2, 0, NGU_W, NGU_H, ngu_xbm);
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    u8g2.drawUTF8(52, 22, "Національна");
    u8g2.drawUTF8(52, 38, "Гвардія");
    u8g2.drawUTF8(52, 54, "України");
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
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(0, 7, title);
    snprintf(h, sizeof(h), "%d/%d", g_displayPage + 1, NUM_DISPLAY_PAGES);
    u8g2.drawUTF8(108, 7, h);
    u8g2.drawHLine(0, 9, 128);
}

inline void drawFooter() {
    char f[42];
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawHLine(0, 53, 128);
    snprintf(f, sizeof(f), ">%s", g_displayStatus);
    u8g2.drawUTF8(0, 62, f);
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

    drawBatteryIcon(0, 13, 52, 14, pct);
    u8g2.setFont(u8g2_font_6x12_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawUTF8(58, 24, buf);
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
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

    drawFooter();
}

inline void drawPageModel() {
    drawHeader("Модель / Серійний");

    char model[24];
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(0, 20, "Модель:");
    if (decodeModel(model, sizeof(model))) {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.drawUTF8(6, 33, model);
    } else {
        u8g2.setFont(u8g2_font_5x8_t_cyrillic);
        u8g2.drawUTF8(48, 20, hasDump ? "(невідомо)" : "(зчитайте)");
    }

    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(0, 46, "Серійний (DS2438):");
    if (hasSN2438) {
        char sn[20];
        int p = 0;
        for (int i = 0; i < 8; i++) p += snprintf(sn + p, sizeof(sn) - p, "%02X", chipSN2438[i]);
        u8g2.drawUTF8(6, 56, sn);
    } else {
        u8g2.drawUTF8(6, 56, "(зчитайте АКБ)");
    }
}

inline void drawPageTech() {
    char buf[40];
    drawHeader("Дані батареї");

    if (!hasDump2438) {
        u8g2.setFont(u8g2_font_5x8_t_cyrillic);
        u8g2.drawUTF8(0, 24, "Немає даних DS2438.");
        u8g2.drawUTF8(0, 34, "Спочатку зчитайте АКБ.");
        drawFooter();
        return;
    }

    uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
    int16_t  traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
    int16_t  iraw = (int16_t)(((uint16_t)batteryDump2438[6] << 8) | batteryDump2438[5]);
    float    i_mA = (float)iraw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f;
    uint8_t  rem  = batteryDump2438[12];                                             // ICA
    uint16_t chg  = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];      // CCA
    uint16_t dis  = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];      // DCA

    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    snprintf(buf, sizeof(buf), "Напруга:  %.2f V", vraw * 0.01f);      u8g2.drawUTF8(0, 18, buf);
    snprintf(buf, sizeof(buf), "Струм:    %.0f mA", i_mA);             u8g2.drawUTF8(0, 27, buf);
    snprintf(buf, sizeof(buf), "Темп:     %.1f C", traw * 0.03125f);   u8g2.drawUTF8(0, 36, buf);
    snprintf(buf, sizeof(buf), "Зал:%u Зар:%u Роз:%u", rem, chg, dis); u8g2.drawUTF8(0, 45, buf);

    drawFooter();
}

inline void drawPageHealth() {
    char buf[48];
    drawHeader("Стан АКБ");
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);

    int cap, wear;
    if (decodeCapacity(&cap, &wear)) {
        snprintf(buf, sizeof(buf), "Ємність: %d %%", cap);  u8g2.drawUTF8(0, 17, buf);
        snprintf(buf, sizeof(buf), "Знос:    %d %%", wear); u8g2.drawUTF8(0, 26, buf);
    } else {
        u8g2.drawUTF8(0, 17, "Ємність: (зчитайте)");
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        uint16_t dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        snprintf(buf, sizeof(buf), "Заряд:%u Розр:%u", cca, dca);  u8g2.drawUTF8(0, 35, buf);
    }

    const char *reason;
    if (batteryGenuine(&reason)) {
        u8g2.drawUTF8(0, 44, "Справжня: ТАК");
    } else {
        snprintf(buf, sizeof(buf), "РИЗИК: %s", reason);
        u8g2.drawUTF8(0, 44, buf);
    }

    drawFooter();
}

// Общая отрисовка сырого дампа (hex), шрифт 4x6, по 8 байт в строке.
inline void drawRawPage(const char *title, const uint8_t *data, bool has, int count) {
    drawHeader(title);
    if (!has) {
        u8g2.setFont(u8g2_font_5x8_t_cyrillic);
        u8g2.drawUTF8(0, 26, "немає даних (зчитайте)");
        return;
    }
    u8g2.setFont(u8g2_font_4x6_tr);
    char buf[36];
    const int perRow = 8;
    int y = 16;
    for (int off = 0; off < count; off += perRow) {
        int n = snprintf(buf, sizeof(buf), "%02X:", off);
        for (int c = 0; c < perRow && off + c < count; c++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02X ", data[off + c]);
        u8g2.drawUTF8(0, y, buf);
        y += 7;
        if (y > 64) break;
    }
}

inline void drawPageRaw2438() { drawRawPage("DS2438 дамп 0-63", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawPage("DS2433 дамп 0-63", batteryDump,     hasDump,     64); }

inline void drawPageReset() {
    drawHeader("Скидання");
    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    u8g2.drawUTF8(0, 19, "Скинути лічильники");
    u8g2.drawUTF8(0, 28, "(цикли/знос) для");
    u8g2.drawUTF8(0, 37, "рекалібрування.");
    if (g_resetArmed)
        u8g2.drawUTF8(0, 50, "Ще раз [<] = СКИДАННЯ!");
    else
        u8g2.drawUTF8(0, 50, "[<] двічі = скидання");
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

// Универсальный опрос кнопки с антидребезгом: возвращает true в момент нажатия.
inline bool buttonPressed(int pin, bool &committed, bool &lastRaw, unsigned long &tChange) {
    bool raw = digitalRead(pin);
    if (raw != lastRaw) { lastRaw = raw; tChange = millis(); }
    if ((millis() - tChange) > 30 && committed != lastRaw) {
        committed = lastRaw;
        if (committed == LOW) return true;
    }
    return false;
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

// Опрос кнопок: BTN = следующая страница; BTN2 = назад, а на странице
// сброса — взвод/подтверждение (двойное нажатие с защитой от случайного).
inline void displayHandleButton() {
    static bool c1 = HIGH, r1 = HIGH; static unsigned long t1 = 0;
    static bool c2 = HIGH, r2 = HIGH; static unsigned long t2 = 0;

    if (buttonPressed(MENU_BTN_PIN, c1, r1, t1)) {   // "Вперёд"
        g_resetArmed = false;                        // уход со страницы снимает взвод
        int prev = g_displayPage;
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        // Полный круг (с последней страницы на первую) — запросить перечитывание.
        if (g_displayPage == 0 && prev == NUM_DISPLAY_PAGES - 1) g_readRequested = true;
        displayRender();
    }

    if (buttonPressed(MENU_BTN2_PIN, c2, r2, t2)) {
        if (g_displayPage == RESET_PAGE) {           // взвод -> подтверждение сброса
            if (!g_resetArmed) { g_resetArmed = true; g_resetArmedAt = millis(); }
            else               { g_resetArmed = false; g_resetRequested = true; }
            displayRender();
        } else {                                     // "Назад"
            g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
            displayRender();
        }
    }

    // Авто-снятие взвода через 5 с без подтверждения.
    if (g_resetArmed && millis() - g_resetArmedAt > 5000) {
        g_resetArmed = false;
        if (g_displayPage == RESET_PAGE) displayRender();
    }
}

#endif
