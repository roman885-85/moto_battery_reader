// ===========================================================================
//  radio_mode_check — ОДНЕ РАДІО ЗА РАЗ, І ПРИЛАД НІКОЛИ НЕ ЛИШАЄТЬСЯ НІМИМ
// ===========================================================================
//  Ціна помилки тут не в даних пакета, а в доступності самого приладу: обидва
//  канали зв'язку з ним — це радіо, і хибне рішення про режим означає прилад,
//  до якого можна достукатись лише кабелем. Тому перевіряються не «функції
//  повертають щось», а три властивості:
//
//    1. увімкнених радіо РІВНО ОДНЕ — на кожному значенні, включно з тими,
//       яких немає в переліку;
//    2. те, що не можна підняти, НЕ ОБИРАЄТЬСЯ — і в збірці з Bluetooth, і в
//       збірці без нього (обидва світи ганяються тут одночасно, бо доступність
//       передається параметром, а не читається з #ifdef);
//    3. збережений режим переживає перезавантаження, а побитий файл не робить
//       прилад німим.
//
//  ⚑ ЗБІРКА БЕЗ BLUETOOTH — НЕ ТЕОРІЯ. Саме в ній живе найважливіше правило:
//  у файлі лежить «Bluetooth», а підняти його нічим. Прошивку перезбирають, і
//  файл лишається від попередньої.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "radio_mode.h"

static int fails = 0;
static void check(bool ok, const char *m) {
    printf(ok ? "   ок    %s\n" : "   ЗБІЙ  %s\n", m);
    if (!ok) fails++;
}

int main() {
    printf("radio_mode_check — режим зв'язку (ця збірка: Bluetooth %s)\n",
           radioBtCompiled() ? "вкомпільовано" : "НЕ вкомпільовано");

    printf("\n1) увімкнених радіо рівно одне\n");
    {
        // ⚑ ПЕРЕБІР ПО ВСЬОМУ БАЙТУ, а не по двох відомих значеннях. Число
        //  приходить із файла, який могли побити, і з команди, яку могли
        //  надіслати ззовні; правило «рівно одне» мусить триматись на КОЖНОМУ
        //  з 256 значень, інакше сміття в файлі підняло б обидва або жодного.
        bool oneEach = true, wifiIsDefault = true;
        for (int m = 0; m < 256; m++) {
            bool w = radioWantsWifi((uint8_t)m), b = radioWantsBt((uint8_t)m);
            if (w == b) oneEach = false;                 // ні обидва, ні жодного
            if (m != RADIO_BT && !w) wifiIsDefault = false;
        }
        check(oneEach, "на кожному з 256 значень увімкнене рівно одне радіо");
        check(wifiIsDefault, "…і все, що не Bluetooth, — це Wi-Fi");
        check(radioWantsBt(RADIO_BT) && !radioWantsWifi(RADIO_BT),
              "Bluetooth вимикає Wi-Fi — власне те, заради чого режим і є");
    }

    printf("\n2) недоступний режим не обирається — в ОБОХ світах\n");
    {
        for (int world = 0; world < 2; world++) {
            bool bt = (world == 1);
            const char *w = bt ? "збірка З Bluetooth" : "збірка БЕЗ Bluetooth";
            char m[140];

            snprintf(m, sizeof(m), "%s: сміття в режимі стає Wi-Fi", w);
            bool junkOk = true;
            for (int v = RADIO_COUNT; v < 256; v++)
                if (radioModeSanitizeFor((uint8_t)v, bt) != RADIO_WIFI) junkOk = false;
            check(junkOk, m);

            snprintf(m, sizeof(m), "%s: Bluetooth %s", w,
                     bt ? "лишається Bluetooth" : "стає Wi-Fi — прилад не німіє");
            check(radioModeSanitizeFor(RADIO_BT, bt) == (bt ? RADIO_BT : RADIO_WIFI), m);

            snprintf(m, sizeof(m), "%s: Wi-Fi лишається Wi-Fi завжди", w);
            check(radioModeSanitizeFor(RADIO_WIFI, bt) == RADIO_WIFI, m);

            // ⚑ КОЛО МУСИТЬ ЗАМИКАТИСЬ. Перебираємо його стільки ж разів,
            //  скільки режимів: якщо десь воно вивалиться в недоступний режим
            //  або зациклиться на місці там, де вибір Є, це видно тут.
            uint8_t cur = RADIO_WIFI;
            bool cycleOk = true, everBt = false;
            for (int i = 0; i < 8; i++) {
                cur = radioModeNextFor(cur, bt);
                if (cur != radioModeSanitizeFor(cur, bt)) cycleOk = false;
                if (cur == RADIO_BT) everBt = true;
            }
            snprintf(m, sizeof(m), "%s: коло не заводить у режим, який не підняти", w);
            check(cycleOk, m);
            snprintf(m, sizeof(m), "%s: Bluetooth %s у колі", w,
                     bt ? "трапляється" : "не трапляється жодного разу");
            check(everBt == bt, m);
        }
    }

    printf("\n3) перемикач: міняє режим і просить рестарт — але не дарма\n");
    {
        // Стан модуля глобальний, тож перевіряємо послідовно, як воно й
        // працює в приладі.
        (void)radioConsumeSave(); (void)radioConsumeReboot();
        radioModeSet(RADIO_WIFI);
        check(!radioConsumeSave() && !radioConsumeReboot(),
              "прочитане з файла не просить ані запису, ані рестарту");

        uint8_t before = radioMode();
        uint8_t after  = radioCycleMode();
        if (radioBtCompiled()) {
            check(after == RADIO_BT && radioMode() == RADIO_BT,
                  "перемикання з Wi-Fi дає Bluetooth");
            check(radioConsumeSave(), "…просить зберегти");
            check(radioConsumeReboot(), "…і перезавантажитись");
            check(!radioConsumeSave() && !radioConsumeReboot(),
                  "…рівно один раз: прапорці забираються, а не тліють");
            check(radioCycleMode() == RADIO_WIFI, "наступне перемикання вертає Wi-Fi");
            (void)radioConsumeSave(); (void)radioConsumeReboot();
        } else {
            // ⚑ РЕСТАРТ, ЯКИЙ НІЧОГО НЕ МІНЯЄ, — ЦЕ НЕСПРАВНІСТЬ НА ВИГЛЯД.
            //  Людина натискає пункт, прилад перезавантажується й повертається
            //  тим самим. Тут перевіряється, що цього не буде.
            check(after == before, "без Bluetooth перемикати нема на що — режим той самий");
            check(!radioConsumeSave(), "…і зберігати нічого");
            check(!radioConsumeReboot(), "…і перезавантажуватись нема за чим");
        }
        check(radioSwitchable() == radioBtCompiled(),
              "«чи є з чого вибирати» відповідає тому, що вкомпільовано");
        radioModeSet(RADIO_WIFI);
        (void)radioConsumeSave(); (void)radioConsumeReboot();
    }

    printf("\n4) файл налаштувань: пишеться, читається й переживає оновлення\n");
    {
        char line[64];
        uint8_t m = 0xEE; bool off = false;

        radioCfgFormat(line, sizeof(line), RADIO_BT, true);
        check(radioCfgParse(line, &m, &off) && m == RADIO_BT && off,
              "записали «Bluetooth + без заряду» — прочитали те саме");

        radioCfgFormat(line, sizeof(line), RADIO_WIFI, false);
        m = 0xEE; off = true;
        check(radioCfgParse(line, &m, &off) && m == RADIO_WIFI && !off,
              "…і навпаки: «Wi-Fi + заряд є»");

        // ⚑ СТАРИЙ ФАЙЛ ПРИЛАДУ ВЛАСНИКА. У ньому записано, що силової частини
        //  немає. Перестати його розуміти — це не «дрібна несумісність», а
        //  прилад, який після оновлення скаржиться на блок живлення, якого в
        //  ньому не передбачено.
        m = 0xEE; off = false;
        check(radioCfgParse("v1 chgoff=1\n", &m, &off) && off && m == RADIO_WIFI,
              "файл старого формату читається, і зв'язок у ньому — Wi-Fi");

        // Сміття не мовчить і не підмінює собою значення.
        uint8_t keepM = RADIO_BT; bool keepOff = true;
        check(!radioCfgParse("абищо", &keepM, &keepOff) &&
              keepM == RADIO_BT && keepOff,
              "нерозбірливий рядок відхиляється, а виходи лишаються недоторканими");
        check(!radioCfgParse(nullptr, &keepM, &keepOff), "порожній рядок — теж відмова");

        // ⚑ І ГОЛОВНЕ ПРО ФАЙЛ: він може пережити перезбирання прошивки.
        //  Розбір каже, ЩО ЗАПИСАНО (навіть якщо цього не підняти), а рішення
        //  «чи можна це підняти» ухвалює sanitize — і саме воно рятує від
        //  німого приладу.
        m = 0xEE;
        radioCfgFormat(line, sizeof(line), RADIO_BT, false);
        check(radioCfgParse(line, &m, nullptr) && m == RADIO_BT,
              "розбір повертає записане, не вирішуючи за залізо");
        check(radioModeSanitizeFor(m, false) == RADIO_WIFI,
              "…а прошивка без Bluetooth підніме Wi-Fi, а не мовчання");
    }

    printf("\n5) назви є в кожного режиму й вони різні\n");
    {
        check(radioModeName(RADIO_WIFI)[0] && radioModeName(RADIO_BT)[0],
              "обидва режими мають назву");
        check(strcmp(radioModeName(RADIO_WIFI), radioModeName(RADIO_BT)) != 0,
              "…і назви не збігаються");
        check(strcmp(radioModeShort(RADIO_WIFI), radioModeShort(RADIO_BT)) != 0,
              "короткі підписи теж різні");
        check(strcmp(radioModeName(200), "?") == 0, "невідомий режим не вдає відомий");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
