#ifndef BUZZER_H
#define BUZZER_H
// ===========================================================================
//  Звукове оповіщення (пасивний буззер / динамік).
//
//  ПРИМІТКА про ЦАП: вбудований ЦАП ESP32 є ЛИШЕ на GPIO25 і GPIO26 — а вони в
//  цьому проєкті зайняті кнопками меню. Тому для звуку використовуємо пасивний
//  буззер/динамік на ВІЛЬНОМУ піні через tone() (апаратний LEDC-ШІМ) — це
//  надійно й не конфліктує з кнопками. Якщо колись звільните 25/26 — можна
//  перейти на ЦАП, але tone() дає ті самі сигнали простіше.
//
//  Увімкнення: розкоментуйте BUZZER_PIN у settings.h (вільний GPIO, напр. 32).
//  Без нього всі функції — порожні (нуль впливу на збірку).
//
//  Сигнали: клік при перемиканні меню, звук на початку операції, на успіху та
//  на помилці. Викликаються централізовано з ledSet() (leds.h) і displayFlip().
// ===========================================================================
#include <Arduino.h>
#include "settings.h"

// ⚠️ ЛИШЕ пасивний П'ЄЗО-буззер (споживає мкА) можна вмикати ПРЯМО на GPIO.
// ДИНАМІК (котушка 4–8 Ом) напряму НЕ підключати: тон дасть ~0.4 А, GPIO віддає
// ~20–40 мА -> просадка живлення -> brownout-reset. Динамік — лише через
// транзистор + ~100 Ом.
//
// ⚡ ВАЖЛИВО про ЧОМУ РАНІШЕ НЕ БУЛО ЗВУКУ: і підсвітка (analogWrite на BLK), і
// tone() користуються тим самим блоком LEDC. Автоматичний розподіл давав їм
// СУСІДНІ канали (0 і 1), а канали 0/1 ДІЛЯТЬ ОДИН ТАЙМЕР. Постійні analogWrite
// підсвітки (плавні переходи, дипи) перезадавали частоту цього таймера — і тон
// глушився. РІШЕННЯ: жорстко саджаємо буззер на ОКРЕМИЙ LEDC-канал (свій таймер),
// подалі від каналу підсвітки, через ledcAttachChannel()/ledcWriteTone() (Arduino
// core 3.x). Для старих ядер — резерв через tone()/noTone().
//
// Тон НЕблокуючий: запускаємо безперервний тон, а гасимо за таймером у buzzTask()
// (виклик щоцикл із ledTask()) — жодних delay(), loop/веб не блокуються, і тони
// достатньо довгі, щоб їх було ЧУТНО.
#ifdef BUZZER_PIN

// Окремий LEDC-канал для буззера. 5 -> таймер 2 (5/2), тоді як підсвітка через
// analogWrite зазвичай бере канал 0 -> таймер 0. Канал 5 існує на всіх ESP32
// (класичний 0..15, S2/S3 0..7, C3 0..5), тож безпечно. За потреби перевизначте.
#ifndef BUZZER_LEDC_CH
  #define BUZZER_LEDC_CH 5
#endif

static unsigned long g_buzzOff = 0;             // millis(), коли гасити тон (0 = вимк.)
static bool g_buzzOn = false;

#if defined(ESP_ARDUINO_VERSION) && defined(ESP_ARDUINO_VERSION_VAL) && \
    ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
  #define BUZZ_USE_LEDC 1
#endif

inline void buzzStop() {
#ifdef BUZZ_USE_LEDC
    ledcWriteTone(BUZZER_PIN, 0);
#else
    noTone(BUZZER_PIN);
#endif
    g_buzzOn = false;
}
inline void buzzTask() {
    if (g_buzzOn && g_buzzOff && (long)(millis() - g_buzzOff) >= 0) { buzzStop(); g_buzzOff = 0; }
}
inline void buzzTone(unsigned int f, unsigned int d) {
#ifdef BUZZ_USE_LEDC
    ledcWriteTone(BUZZER_PIN, f);               // окремий канал/таймер -> не глушиться підсвіткою
#else
    tone(BUZZER_PIN, f);
#endif
    g_buzzOn = true;
    g_buzzOff = millis() + d;                    // гасимо через d мс у buzzTask()
    if (g_buzzOff == 0) g_buzzOff = 1;           // 0 зарезервовано під «вимк.»
}
inline void buzzInit() {
#ifdef BUZZ_USE_LEDC
    bool ok = ledcAttachChannel(BUZZER_PIN, 2000, 10, BUZZER_LEDC_CH);  // свій канал -> свій таймер
    ledcWriteTone(BUZZER_PIN, 0);
    Serial.printf("BUZZER: pin=%d LEDC ch=%d attach=%s\n",
                  (int)BUZZER_PIN, (int)BUZZER_LEDC_CH, ok ? "OK" : "FAIL");
#else
    pinMode(BUZZER_PIN, OUTPUT);
    noTone(BUZZER_PIN);
    Serial.printf("BUZZER: pin=%d tone() (Arduino core < 3.0)\n", (int)BUZZER_PIN);
#endif
    g_buzzOn = false;
}

// Одноразова самоперевірка звуку на старті: дводовий «чирп» (блокуючий, лише в
// setup()). Якщо чути — динамік і канал справні; якщо ні — дивись Serial-рядок
// BUZZER: вище (attach=OK/FAIL) і перевір підключення пасивного п'єзо до піна/GND.
inline void buzzSelfTest() {
#ifdef BUZZ_USE_LEDC
    ledcWriteTone(BUZZER_PIN, 1500); delay(140);
    ledcWriteTone(BUZZER_PIN, 2600); delay(140);
    ledcWriteTone(BUZZER_PIN, 0);
#else
    tone(BUZZER_PIN, 1500); delay(140);
    tone(BUZZER_PIN, 2600); delay(140);
    noTone(BUZZER_PIN);
#endif
    Serial.println("BUZZER: self-test chirp done");
}
inline void buzzClick() { buzzTone(2300, 35);  }   // перемикання меню (короткий «тік»)
inline void buzzStart() { buzzTone(1200, 80);  }   // початок операції
inline void buzzOk()    { buzzTone(2200, 180); }   // успіх (короткий високий)
inline void buzzErr()   { buzzTone(350, 380);  }   // помилка (довгий низький)
#else
inline void buzzTask()     {}
inline void buzzInit()     {}
inline void buzzSelfTest()  {}
inline void buzzClick()    {}
inline void buzzStart()    {}
inline void buzzOk()       {}
inline void buzzErr()      {}
#endif

#endif // BUZZER_H
