#ifndef SETTINGS_H
#define SETTINGS_H

// Налаштування точки доступу ESP32
#define AP_SSID "BatteryReader_moto"
#define AP_PASSWORD "12345678"

// Піни ESP32
#define PULLUP_PIN 12      // Керування підтяжкою 1-Wire
#define DS_PIN 13          // Пін даних 1-Wire
#define LED_GREEN_PIN 14   // Зелений LED
#define LED_RED_PIN 27     // Червоний LED

// Веб-сервер
#define HTTP_PORT 80

// Пароль для записи прошивки
#define ADMIN_PASSWORD "admin123"

// Розмір дампа
#define DUMP_SIZE 512

// IP адреса ESP32 в режимі AP
#define ESP_IP "192.168.4.1"

// ======================= ДИСПЛЕЙ =======================
// Крок 1. Виберіть модель дисплея — розкоментуйте Рівно Одну рядок.
// (інтерфейс/роздільність підставляться автоматично)
#define DISPLAY_SSD1306_I2C        // 0.96" OLED 128x64 I2C (GME12864) — за замовчуванням
// #define DISPLAY_SH1106_I2C      // 1.3"  OLED 128x64 I2C (SH1106)
// #define DISPLAY_SH1107_128_I2C  // 1.5"  OLED 128x128 I2C (GME128128-02, варіант SH1107)
// #define DISPLAY_SSD1327_128_I2C // 1.5"  OLED 128x128 I2C (GME128128-02, варіант SSD1327)
// #define DISPLAY_ST7567_SPI      // Open-Smart 1.8" ST7567, 128x64, SPI
// #define DISPLAY_PCD8544_SPI     // Nokia 5110 (PCD8544), 84x48, SPI
// --- КОЛЬОРОВІ TFT на ST7789 (SPI) — кольорове меню + кольорова заставка ---
// Розкоментуйте DISPLAY_ST7789_SPI + РІВНО ОДИН розмір нижче.
// Потрібні бібліотеки: Adafruit GFX, Adafruit ST7735/ST7789, Adafruit BusIO,
// U8g2_for_Adafruit_GFX (кириличні шрифти на кольоровому екрані).
// #define DISPLAY_ST7789_SPI
//   #define DISPLAY_ST7789_240X240   // ST7789VW — 240x240
//   #define DISPLAY_ST7789_240X280   // ST7789V3 — 240x280
// Орієнтація 0..3 (0 — портрет). Інверсія — для панелей із «негативом».
// #define DISPLAY_ST7789_ROT     0
// #define DISPLAY_ST7789_INVERT
// Підсвітка (BLK/BL) — необов'язково; вкажіть GPIO, куди підключено:
// #define DISPLAY_BLK_PIN        4
// Піни SPI ST7789: SCK=GPIO18, MOSI(SDA)=GPIO23 (апаратний SPI ESP32);
// CS/DC(RS)/RST — з блоку «Крок 2b» нижче (DISPLAY_CS/DC/RST_PIN).

// Якщо обраний SSD1327 і він НЕ працює/порожній екран — панелі GME128128-02
// бувають з різною "розводкою" контролера. Розкоментуйте один варіант:
// #define SSD1327_WS     // Waveshare-сумісний remap
// #define SSD1327_EA     // EA W128128
// #define SSD1327_ZJY    // ZJY 128x128
// (за замовчуванням — MIDAS, як було). Також допомагає задати контраст нижче.

// Крок 2a. Піни I2C (для I2C-дисплеїв). Працюють на Будь-яких GPIO — і
// програмний (SW), і апаратний (HW) I2C: ESP32 розводить їх матрицею.
#define DISPLAY_SDA_PIN   21     // I2C SDA
#define DISPLAY_SCL_PIN   22     // I2C SCL
#define DISPLAY_I2C_ADDR  0x3C   // адреса (зазвичай 0x3C, рідше 0x3D)
// Апаратний I2C — рендер в ~10 раз швидше. Безпечно: якщо апаратна
// шина не відповідає (немає ACK), прошивка при старте сама перейде на
// програмний I2C на тех же пінах (в Serial: "SW I2C fallback").
// Частая апаратна причина відмови HW I2C — слабкі підтяжки: поставте
// зовнішні резистори 4.7к з SDA і SCL на 3.3V.
// #define DISPLAY_HW_I2C
// Частота шини I2C, кГц. 0 = АВТО: безпечний максимум з драйвера дисплея
// (SSD1306/SH1106/SH1107 — 400 кГц, SSD1327 — лише 100 кГц: 400 він НЕ
// тримає, і на апаратному I2C зависає уся шина!). Змінюйте лише свідомо.
#define DISPLAY_I2C_KHZ   0

// Крок 2b. Піни SPI (для ST7567 / Nokia 5110). Апаратний SPI ESP32:
// SCK=GPIO18, MOSI=GPIO23 (підключаються до CLK/DIN дисплея). Керуючі:
#define DISPLAY_CS_PIN    5      // CS  (Chip Select)
#define DISPLAY_DC_PIN    17     // DC  (Data/Command; у Nokia — «D/C»)
#define DISPLAY_RST_PIN   16     // RST (Reset)
// Зсув картинки вправо для ST7567, пікселів (у панелі Open-Smart RAM на
// 132 колонки, видно 4..131). Якщо картинка зсунута — підберіть 0..4.
#define DISPLAY_ST7567_XOFF 4
// Контраст дисплея 0..255 (розкоментуйте, щоб задати вручну;
// для ST7567 більше = темніше, типово 30..90).
// #define DISPLAY_CONTRAST 128

// --- Кнопки меню (між GPIO і GND, активний рівень LOW, внутр. підтяжка) ---
#define MENU_BTN_PIN  25   // "Вперед": наступна сторінка
#define MENU_BTN2_PIN 26   // "Назад": попередня сторінка

// --- Индикатор заряду ---
// Пріоритет ICA (DS2438). При вимкненому обліку струму (IAD=0) — по напрузі.
#define ICA_FULL_SCALE    255      // значення ICA, відповідне 100%
#define BATTERY_EMPTY_MV  6000     // "порожньо" для запасного розрахунку по U, мВ
#define BATTERY_FULL_MV   8400     // "повно", мВ
// Струмовимірювальний резистор DS2438, Ом. Впливає на струм (мА) і перерахунок
// лічильників ICA/CCA/DCA в мА·ч. Значення калібрувальне — підберіть под свою
// АКБ, щоб ємність збігалася з паспортної (типово 20..40 мОм).
#define DS2438_RSENSE_OHM 0.025f
// Ціна молодшого розряду лічильників заряду в мА·ч (даташит DS2438): 0.4882/Rsense.
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
// Паспортна ємність АКБ, мА·ч — для підрахунку циклів заряду/розряду:
// 1 цикл = сумарний заряд (розряд), рівний однієї повної ємності.
// Вкажіть ємність вашої батареї (PMNN4409 — 2500, PMNN4493 — 3000 і т.п.).
#define BATTERY_RATED_MAH 2500
// Повна шкала ICA (залишкової ємності DS2438), мА·ч: ICA=255 -> стільки мА·ч.
// Використовується для показу/редагування залишкової ємності в мА·ч (замість відсотків).
#define ICA_FULL_MAH (255.0f * DS2438_MAH_PER_LSB)

#endif