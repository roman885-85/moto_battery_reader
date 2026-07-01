#ifndef DISPLAY_H
#define DISPLAY_H

#include <U8g2lib.h>
#include "settings.h"
#include "battery_reader.h"

// Состояние, которое отображаем (заполняется из .ino и обработчиков веб-сервера).
extern bool hasDump;
extern bool hasDump2438;
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];

// Объект дисплея. Программный (bit-bang) I2C — работает на любых GPIO,
// пины берутся из settings.h. Полный буфер кадра (_F_) = 1 КБ ОЗУ.
#if defined(DISPLAY_SH1106)
  U8G2_SH1106_128X64_NONAME_F_SW_I2C  u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN, U8X8_PIN_NONE);
#endif

// Текущая строка статуса (нижняя строка экрана).
static char g_displayStatus[22] = "BOOT";

// Инициализация дисплея.
inline void displayInit() {
    u8g2.setI2CAddress(DISPLAY_I2C_ADDR << 1);
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x8_tr);
}

inline void displayRender(); // определение ниже

// Установить строку статуса (не перерисовывает — вызовите displayRender()).
inline void displaySetStatus(const char *s) {
    strncpy(g_displayStatus, s, sizeof(g_displayStatus) - 1);
    g_displayStatus[sizeof(g_displayStatus) - 1] = '\0';
}

// Установить статус и сразу перерисовать.
inline void displayShow(const char *s) {
    displaySetStatus(s);
    displayRender();
}

// Перерисовать весь экран из текущего состояния.
inline void displayRender() {
    char line[28];

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x8_tr);

    // Заголовок
    u8g2.drawStr(0, 7, "Moto IMPRES Reader");
    u8g2.drawHLine(0, 9, 128);

    // Точка доступа и IP
    snprintf(line, sizeof(line), "AP:%s", AP_SSID);
    u8g2.drawStr(0, 18, line);
    snprintf(line, sizeof(line), "IP:%s", ESP_IP);
    u8g2.drawStr(0, 27, line);

    // Наличие дампов
    snprintf(line, sizeof(line), "Dump 2433:%s 2438:%s",
             hasDump ? "Y" : "N", hasDump2438 ? "Y" : "N");
    u8g2.drawStr(0, 36, line);

    // Расшифровка измерений DS2438 (если считаны)
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        float voltage = vraw * 0.01f; // 10 мВ/LSb
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        float temp = traw * 0.03125f; // °C/LSb
        snprintf(line, sizeof(line), "V:%.2fV  T:%.1fC", voltage, temp);
        u8g2.drawStr(0, 45, line);
    } else {
        u8g2.drawStr(0, 45, "DS2438: no data");
    }

    // Строка статуса (текущая операция)
    u8g2.drawHLine(0, 53, 128);
    snprintf(line, sizeof(line), ">%s", g_displayStatus);
    u8g2.drawStr(0, 62, line);

    u8g2.sendBuffer();
}

#endif
