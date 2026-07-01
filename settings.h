#ifndef SETTINGS_H
#define SETTINGS_H

// Настройки точки доступа ESP32
#define AP_SSID "BatteryReader_moto"
#define AP_PASSWORD "12345678"

// Пины ESP32
#define PULLUP_PIN 12      // Управление подтяжкой 1-Wire
#define DS_PIN 13          // Пин данных 1-Wire
#define LED_GREEN_PIN 14   // Зеленый LED
#define LED_RED_PIN 27     // Красный LED

// Веб-сервер
#define HTTP_PORT 80

// Пароль для записи прошивки
#define ADMIN_PASSWORD "admin123"

// Размер дампа
#define DUMP_SIZE 512

// IP адрес ESP32 в режиме AP
#define ESP_IP "192.168.4.1"

// --- Дисплей GME12864 (OLED 128x64, I2C) ---
// Пины I2C (любые свободные GPIO — используется программный I2C).
#define DISPLAY_SDA_PIN   21     // I2C SDA
#define DISPLAY_SCL_PIN   22     // I2C SCL
#define DISPLAY_I2C_ADDR  0x3C   // адрес дисплея (обычно 0x3C, реже 0x3D)
// Контроллер по умолчанию SSD1306. Если экран остаётся пустым — вероятно,
// у вас SH1106: раскомментируйте строку ниже.
// #define DISPLAY_SH1106

// Аппаратный I2C дисплея — рендер в ~10 раз быстрее (кнопки отзывчивее),
// но работает ТОЛЬКО на GPIO21(SDA)/22(SCL). Если экран на этих пинах —
// раскомментируйте. Для других пинов оставьте закомментированным (SW I2C).
// #define DISPLAY_HW_I2C

// --- Кнопки меню (между GPIO и GND, активный уровень LOW, внутр. подтяжка) ---
#define MENU_BTN_PIN  25   // "Вперёд": следующая страница
#define MENU_BTN2_PIN 26   // "Назад": предыдущая страница

// --- Индикатор заряда ---
// Приоритет ICA (DS2438). При отключённом учёте тока (IAD=0) — по напряжению.
#define ICA_FULL_SCALE    255      // значение ICA, соответствующее 100%
#define BATTERY_EMPTY_MV  6000     // "пусто" для запасного расчёта по U, мВ
#define BATTERY_FULL_MV   8400     // "полно", мВ
#define DS2438_RSENSE_OHM 0.010f   // токоизмерительный резистор, Ом (для тока в мА)

#endif