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
#define NUM_DISPLAY_PAGES 6

// Объект дисплея. Программный (bit-bang) I2C — работает на любых GPIO,
// пины берутся из settings.h. Полный буфер кадра (_F_) = 1 КБ ОЗУ.
#if defined(DISPLAY_SH1106)
  U8G2_SH1106_128X64_NONAME_F_SW_I2C  u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#endif

static char g_displayStatus[22] = "BOOT";  // нижняя строка статуса
static int  g_displayPage = 0;             // текущая страница меню
static bool g_readRequested = false;       // запрос повторного чтения после цикла

inline void displayRender(); // определение ниже

// ---------- базовая настройка ----------

inline void displayInit() {
    u8g2.setI2CAddress(DISPLAY_I2C_ADDR << 1);
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x8_tr);
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
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 7, title);
    snprintf(h, sizeof(h), "%d/%d", g_displayPage + 1, NUM_DISPLAY_PAGES);
    u8g2.drawStr(108, 7, h);
    u8g2.drawHLine(0, 9, 128);
}

inline void drawFooter() {
    char f[26];
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawHLine(0, 53, 128);
    snprintf(f, sizeof(f), ">%s", g_displayStatus);
    u8g2.drawStr(0, 62, f);
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
    if (!hasDump) { *reason = "no dump"; return false; }

    bool auth = false;
    static const char pat[] = "MOTOROLA";
    const int plen = 8;
    for (int i = 0; i + plen <= (int)DUMP_SIZE && !auth; i++) {
        int k = 0;
        while (k < plen && batteryDump[i + k] == (uint8_t)pat[k]) k++;
        if (k == plen) auth = true;
    }
    if (!auth) { *reason = "no auth block"; return false; }

    if (batteryDump[0x1B] == 0xFF && batteryDump[0x1C] == 0xFF &&
        batteryDump[0x1D] == 0xFF && batteryDump[0x1E] == 0xFF) {
        *reason = "calib erased";
        return false;
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) { *reason = "CCA overflow"; return false; }
    }

    *reason = "OK";
    return true;
}

// ---------- страницы меню ----------

inline void drawPageMain() {
    char buf[26];
    const char *src;
    int pct = batteryPercent(&src);

    drawHeader("Moto IMPRES");

    drawBatteryIcon(0, 13, 52, 14, pct);
    u8g2.setFont(u8g2_font_6x12_tr);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else          snprintf(buf, sizeof(buf), "--%%");
    u8g2.drawStr(58, 24, buf);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(90, 23, src);                  // источник данных заряда

    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        snprintf(buf, sizeof(buf), "%.2f V   %.1f C", vraw * 0.01f, traw * 0.03125f);
    } else {
        snprintf(buf, sizeof(buf), "DS2438: no data");
    }
    u8g2.drawStr(0, 40, buf);

    snprintf(buf, sizeof(buf), "IP: %s", ESP_IP);
    u8g2.drawStr(0, 49, buf);

    drawFooter();
}

inline void drawPageModel() {
    drawHeader("Model / Serial");

    char model[24];
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 20, "Model:");
    if (decodeModel(model, sizeof(model))) {
        u8g2.setFont(u8g2_font_7x13B_tr);
        u8g2.drawStr(6, 33, model);
    } else {
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(40, 20, hasDump ? "(unknown)" : "(read first)");
    }

    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 46, "Serial (DS2438 chip):");
    if (hasSN2438) {
        char sn[20];
        int p = 0;
        for (int i = 0; i < 8; i++) p += snprintf(sn + p, sizeof(sn) - p, "%02X", chipSN2438[i]);
        u8g2.drawStr(6, 56, sn);
    } else {
        u8g2.drawStr(6, 56, "(read battery)");
    }
}

inline void drawPageTech() {
    char buf[40];
    drawHeader("Battery data");

    if (!hasDump2438) {
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 24, "No DS2438 data.");
        u8g2.drawStr(0, 34, "Read battery first.");
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

    u8g2.setFont(u8g2_font_5x8_tr);
    snprintf(buf, sizeof(buf), "Voltage:  %.2f V", vraw * 0.01f);      u8g2.drawStr(0, 18, buf);
    snprintf(buf, sizeof(buf), "Current:  %.0f mA", i_mA);             u8g2.drawStr(0, 27, buf);
    snprintf(buf, sizeof(buf), "Temp:     %.1f C", traw * 0.03125f);   u8g2.drawStr(0, 36, buf);
    snprintf(buf, sizeof(buf), "Rem:%u Chg:%u Dis:%u", rem, chg, dis); u8g2.drawStr(0, 45, buf);

    drawFooter();
}

inline void drawPageHealth() {
    char buf[26];
    drawHeader("Health");
    u8g2.setFont(u8g2_font_5x8_tr);

    int cap, wear;
    if (decodeCapacity(&cap, &wear)) {
        snprintf(buf, sizeof(buf), "Capacity: %d %%", cap);  u8g2.drawStr(0, 17, buf);
        snprintf(buf, sizeof(buf), "Wear:     %d %%", wear); u8g2.drawStr(0, 26, buf);
    } else {
        u8g2.drawStr(0, 17, "Capacity: (read DS2433)");
    }

    if (hasDump2438) {
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        uint16_t dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        snprintf(buf, sizeof(buf), "Charge:%u Dis:%u", cca, dca);  u8g2.drawStr(0, 35, buf);
    }

    const char *reason;
    if (batteryGenuine(&reason)) {
        u8g2.drawStr(0, 44, "Auth: GENUINE");
    } else {
        snprintf(buf, sizeof(buf), "Auth: LOCK-RISK/%s", reason);
        u8g2.drawStr(0, 44, buf);
    }

    drawFooter();
}

// Общая отрисовка сырого дампа (hex), шрифт 4x6, по 8 байт в строке.
inline void drawRawPage(const char *title, const uint8_t *data, bool has, int count) {
    drawHeader(title);
    if (!has) {
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 26, "no data (read first)");
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
        u8g2.drawStr(0, y, buf);
        y += 7;
        if (y > 64) break;
    }
}

inline void drawPageRaw2438() { drawRawPage("DS2438 raw 0-63", batteryDump2438, hasDump2438, DS2438_MEM_SIZE); }
inline void drawPageRaw2433() { drawRawPage("DS2433 raw 0-63", batteryDump,     hasDump,     64); }

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

// Опрос кнопок: BTN = следующая страница, BTN2 = предыдущая.
inline void displayHandleButton() {
    static bool c1 = HIGH, r1 = HIGH; static unsigned long t1 = 0;
    static bool c2 = HIGH, r2 = HIGH; static unsigned long t2 = 0;

    if (buttonPressed(MENU_BTN_PIN, c1, r1, t1)) {   // "Вперёд"
        int prev = g_displayPage;
        g_displayPage = (g_displayPage + 1) % NUM_DISPLAY_PAGES;
        // Полный круг (с последней страницы на первую) — запросить перечитывание.
        if (g_displayPage == 0 && prev == NUM_DISPLAY_PAGES - 1) g_readRequested = true;
        displayRender();
    }

    if (buttonPressed(MENU_BTN2_PIN, c2, r2, t2)) {  // "Назад"
        g_displayPage = (g_displayPage - 1 + NUM_DISPLAY_PAGES) % NUM_DISPLAY_PAGES;
        displayRender();
    }
}

#endif
