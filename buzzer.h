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

#ifdef BUZZER_PIN
inline void buzzInit()  { pinMode(BUZZER_PIN, OUTPUT); }
inline void buzzClick() { tone(BUZZER_PIN, 2300, 6); }                 // перемикання меню
inline void buzzStart() { tone(BUZZER_PIN, 1200, 40); }               // початок операції
inline void buzzOk()    { tone(BUZZER_PIN, 1600, 70); delay(80);
                          tone(BUZZER_PIN, 2400, 110); }              // успіх (висхідний)
inline void buzzErr()   { tone(BUZZER_PIN, 400, 180); delay(120);
                          tone(BUZZER_PIN, 300, 260); }               // помилка (низький)
#else
inline void buzzInit()  {}
inline void buzzClick() {}
inline void buzzStart() {}
inline void buzzOk()    {}
inline void buzzErr()   {}
#endif

#endif // BUZZER_H
