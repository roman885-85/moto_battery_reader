#ifndef BUZZER_H
#define BUZZER_H
// ===========================================================================
//  Звукове оповіщення (пасивний п'єзо-буззер).
//
//  ПРИМІТКА про ЦАП: вбудований ЦАП ESP32 є ЛИШЕ на GPIO25 і GPIO26 — а вони в
//  цьому проєкті зайняті кнопками меню. Тому для звуку використовуємо пасивний
//  буззер на ВІЛЬНОМУ піні через апаратний LEDC-ШІМ.
//
//  Увімкнення: розкоментуйте BUZZER_PIN у settings.h (вільний ВИХІДНИЙ GPIO).
//  Без нього всі функції — порожні (нуль впливу на збірку).
//
//  М'ЯКИЙ ЗВУК. Різкість дає (а) повна гучність — меандр зі шпаруватістю 50%,
//  максимум для п'єзо, і (б) миттєвий старт/стоп ноти. Тому тут:
//   • гучність керується ШПАРУВАТІСТЮ (BUZZER_VOLUME) — тихіше й м'якше;
//   • кожна нота має ОГИНАЮЧУ: тихий вхід -> основна частина -> тихий вихід,
//     тож немає клацань на початку/кінці;
//   • сигнали — короткі мелодійні послідовності (акорд/терція), а не голий
//     писк на одній частоті.
//
//  Сигнали: клац при перегортанні меню, м'який акорд на початку операції, на
//  успіху та на помилці. Викликаються централізовано з ledSet() (leds.h) і
//  displayFlip(). Відтворення НЕблокуюче: buzzTask() (з ledTask()) веде
//  послідовність по millis(), жодних delay() у loop.
// ===========================================================================
#include <Arduino.h>
#include "settings.h"

// ⚠️ ЛИШЕ пасивний П'ЄЗО-буззер (споживає мкА) можна вмикати ПРЯМО на GPIO.
// ДИНАМІК (котушка 4–8 Ом) напряму НЕ підключати: перевантаження струмом ->
// brownout-reset. Динамік — лише через транзистор + ~100 Ом.
// ⚠️ GPIO34/35/36/39 — ВХІД-ТІЛЬКИ, звуку не дадуть (перевіряється в settings.h).
#ifdef BUZZER_PIN

// Окремий LEDC-канал (свій таймер), подалі від каналу підсвітки: інакше ШІМ
// підсвітки (analogWrite) перезадає частоту спільного таймера і глушить тон.
#ifndef BUZZER_LEDC_CH
  #define BUZZER_LEDC_CH 5
#endif
// Загальна гучність 0..255 (255 = максимум, тобто шпаруватість 50%). Менше =
// тихіше й м'якше. Типово помірно-тиха — задайте своє у settings.h.
#ifndef BUZZER_VOLUME
  #define BUZZER_VOLUME 150
#endif
#define BUZZ_LEDC_BITS 10                       // роздільність ШІМ (0..1023)

#if defined(ESP_ARDUINO_VERSION) && defined(ESP_ARDUINO_VERSION_VAL) && \
    ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
  #define BUZZ_USE_LEDC 1
#endif

// Крок послідовності: частота (Гц; 0 = пауза), тривалість (мс), гучність 0..255
// (відносна, множиться на BUZZER_VOLUME).
struct BuzzStep { uint16_t f; uint16_t ms; uint8_t vol; };

static const BuzzStep *g_bzSeq = nullptr;       // поточна послідовність
static uint8_t  g_bzLen = 0, g_bzIdx = 0;
static unsigned long g_bzStepEnd = 0;

// Видати тон заданої частоти й гучності (гучність — через шпаруватість).
inline void buzzApply(uint16_t f, uint8_t vol) {
#ifdef BUZZ_USE_LEDC
    if (f == 0 || vol == 0) { ledcWrite(BUZZER_PIN, 0); return; }
    ledcWriteTone(BUZZER_PIN, f);               // задає частоту (шпаруватість 50%)
    // Приглушуємо: 255 -> 50% шпаруватості, менше -> тихіше й м'якше.
    uint32_t full = (1UL << BUZZ_LEDC_BITS) / 2;            // 50% = максимум гучності
    uint32_t duty = full * ((uint32_t)vol * BUZZER_VOLUME / 255) / 255;
    ledcWrite(BUZZER_PIN, duty);
#else
    (void)vol;                                   // на старих ядрах гучність не керується
    if (f == 0) noTone(BUZZER_PIN); else tone(BUZZER_PIN, f);
#endif
}

// Запустити послідовність (неблокуюче).
inline void buzzPlay(const BuzzStep *seq, uint8_t len) {
    if (!seq || !len) return;
    g_bzSeq = seq; g_bzLen = len; g_bzIdx = 0;
    buzzApply(seq[0].f, seq[0].vol);
    g_bzStepEnd = millis() + seq[0].ms;
}

// Викликати щоцикл (з ledTask()): веде послідовність і гасить звук наприкінці.
inline void buzzTask() {
    if (!g_bzSeq) return;
    if ((long)(millis() - g_bzStepEnd) < 0) return;
    if (++g_bzIdx >= g_bzLen) { buzzApply(0, 0); g_bzSeq = nullptr; return; }
    buzzApply(g_bzSeq[g_bzIdx].f, g_bzSeq[g_bzIdx].vol);
    g_bzStepEnd = millis() + g_bzSeq[g_bzIdx].ms;
}

// --- Мелодії. Кожна нота: тихий вхід -> основна частина -> тихий вихід
// (огинаюча), тому звук «наростає й згасає», а не клацає. Частоти помірні —
// високі різкі тони прибрано.

// Початок операції: м'яка висхідна терція E5 -> A5 (спокійне «беруся до справи»).
static const BuzzStep BZ_START[] = {
    {659, 20,  90}, {659, 45, 200}, {659, 25, 110},
    {880, 20, 120}, {880, 55, 200}, {880, 35,  70},
};
// Успіх: м'яке мажорне тризвуччя G5 -> B5 -> D6 із плавним згасанням.
static const BuzzStep BZ_OK[] = {
    {784, 18,  90}, {784, 45, 195}, {784, 20, 120},
    {988, 18, 110}, {988, 45, 200}, {988, 20, 120},
    {1175,20, 120}, {1175,55, 195}, {1175,55,  60},
};
// Помилка: м'яка низхідна пара A4 -> E4. Низько й неквапом — зрозуміло «щось
// не так», але без різкого дзижчання.
static const BuzzStep BZ_ERR[] = {
    {440, 25, 110}, {440, 80, 210}, {440, 30, 120},
    {330, 25, 130}, {330,110, 210}, {330, 70,  60},
};
// Самоперевірка на старті: тихий двонотний «дзинь».
static const BuzzStep BZ_HELLO[] = {
    {659, 20,  90}, {659, 55, 190}, {659, 25, 100},
    {988, 20, 110}, {988, 70, 190}, {988, 45,  60},
};

inline void buzzInit() {
#ifdef BUZZ_USE_LEDC
    bool ok = ledcAttachChannel(BUZZER_PIN, 2000, BUZZ_LEDC_BITS, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_PIN, 0);
    Serial.printf("BUZZER: pin=%d LEDC ch=%d vol=%d attach=%s\n",
                  (int)BUZZER_PIN, (int)BUZZER_LEDC_CH, (int)BUZZER_VOLUME,
                  ok ? "OK" : "FAIL");
#else
    pinMode(BUZZER_PIN, OUTPUT);
    noTone(BUZZER_PIN);
    Serial.printf("BUZZER: pin=%d tone() (Arduino core < 3.0)\n", (int)BUZZER_PIN);
#endif
    g_bzSeq = nullptr;
}

// Перемикання меню — короткий м'який «клац». Це НЕ тон: даємо кілька дуже
// коротких прямих імпульсів (широкосмуговий тік), потім повертаємо LEDC для
// мелодій. Імпульси вузькі, тож клац тихий і не різкий.
inline void buzzClick() {
#ifdef BUZZ_USE_LEDC
    ledcDetach(BUZZER_PIN);
#else
    noTone(BUZZER_PIN);
#endif
    pinMode(BUZZER_PIN, OUTPUT);
    for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(140);
        digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(140);
    }
#ifdef BUZZ_USE_LEDC
    ledcAttachChannel(BUZZER_PIN, 2000, BUZZ_LEDC_BITS, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_PIN, 0);
#endif
    g_bzSeq = nullptr;
}

inline void buzzStart() { buzzPlay(BZ_START, sizeof(BZ_START) / sizeof(BZ_START[0])); }
inline void buzzOk()    { buzzPlay(BZ_OK,    sizeof(BZ_OK)    / sizeof(BZ_OK[0]));    }
inline void buzzErr()   { buzzPlay(BZ_ERR,   sizeof(BZ_ERR)   / sizeof(BZ_ERR[0]));   }

// Стартова самоперевірка (у setup(); тут delay() допустимий — loop ще не йде).
inline void buzzSelfTest() {
    const uint8_t n = sizeof(BZ_HELLO) / sizeof(BZ_HELLO[0]);
    for (uint8_t i = 0; i < n; i++) { buzzApply(BZ_HELLO[i].f, BZ_HELLO[i].vol); delay(BZ_HELLO[i].ms); }
    buzzApply(0, 0);
    Serial.println("BUZZER: self-test chime done");
}

#else
inline void buzzTask()     {}
inline void buzzInit()     {}
inline void buzzSelfTest() {}
inline void buzzClick()    {}
inline void buzzStart()    {}
inline void buzzOk()       {}
inline void buzzErr()      {}
#endif

#endif // BUZZER_H
