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

#endif