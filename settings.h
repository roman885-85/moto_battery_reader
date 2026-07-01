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

#endif