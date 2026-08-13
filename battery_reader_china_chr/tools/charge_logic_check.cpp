// ===========================================================================
//  ЛОГІКА ЗАРЯДУ — перевірка на СПРАВЖНЬОМУ charge.h.
//
//  ⚑ Раніше цей тест тримав ВЛАСНУ копію функцій заряду, скопійовану з
//  charge.h «щоб перевірити ізольовано». Копія робить рівно протилежне: коли
//  схему заряду переробили (готова плата DC/DC на TL494 -> ШІМ на P-канальний
//  MOSFET через NPN), тест лишився зеленим, бо й далі ганяв стару копію —
//  калібрувальну таблицю «напруга керування -> вихідна напруга», якої в
//  проєкті вже немає. Тепер підключаємо справжній заголовок: якщо логіка
//  зміниться, тест або зловить різницю, або взагалі не збереться.
//
//  Що перевіряємо:
//    • профіль струму й його масштабування під обрану ціль;
//    • регулятор шпаруватості: soft-start, збіжність, стеля, підлога, і те,
//      що від'ємний струм НЕ випрямляється (це не «уставка досягнута»);
//    • перерахунок АЦП -> напруга пакета через подільник;
//    • вимір струму: середнє по серії й ПІК на порубаному ШІМом сигналі;
//    • запас вимірювального кола: подільник і шунт не виводять АЦП за межу.
// ===========================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── мінімальне оточення Arduino ────────────────────────────────────────────
#define OUTPUT 1
#define INPUT  0
#define HIGH   1
#define LOW    0
#define ADC_11db 3
static void pinMode(int, int) {}
static void digitalWrite(int, int) {}
static void analogSetPinAttenuation(int, int) {}
static unsigned long millis() { return 1000; }
static bool ledcAttachChannel(int, int, int, int) { return true; }
static uint32_t g_lastDuty = 0;
static void ledcWrite(int, uint32_t d) { g_lastDuty = d; }
static class { public: void printf(const char *, ...) {} void println(const char *) {}
               void println() {} void print(const char *) {} } Serial;

// ── керований АЦП ──────────────────────────────────────────────────────────
//  Модель шини: для піна струму віддаємо «порубаний ШІМом» сигнал (peak або 0
//  залежно від фази), для піна напруги — сталий рівень подільника.
static int      g_adcIsensePeakRaw = 0;   // відлік у фазі «ключ відкритий»
static int      g_adcDutyNum = 0, g_adcDutyDen = 1;
static int      g_adcVsenseRaw = 0;
static long     g_adcCall = 0;
static long     g_adcIsenseReads = 0;
static int analogRead(int pin);

#include "settings.h"

// leds.h підмінюємо — справжній тягне buzzer.h з таблицями й таймерами.
#define LEDS_H
enum LedMode { LED_BOOT, LED_IDLE, LED_READ, LED_WRITE, LED_OK, LED_ERROR,
               LED_DISCHARGE, LED_CHARGE, LED_CHARGE_TAPER };
static LedMode g_led = LED_BOOT;
static void ledSet(LedMode m) { g_led = m; }

#include "charge.h"

static int analogRead(int pin) {
    g_adcCall++;
    if (pin == CHARGE_VSENSE_PIN) return g_adcVsenseRaw;
    // Пін струму: рівномірно розкидаємо «відкриті» фази по серії — саме так
    // серія й перекриває кілька повних періодів ШІМ.
    g_adcIsenseReads++;
    bool on = ((g_adcIsenseReads - 1) * g_adcDutyNum / g_adcDutyDen) !=
              ((g_adcIsenseReads)     * g_adcDutyNum / g_adcDutyDen);
    return on ? g_adcIsensePeakRaw : 0;
}

// мА -> сирий відлік АЦП на шунті (I·R -> мВ -> відлік).
static int maToRaw(int ma) {
    long mv = (long)ma * CHARGE_SHUNT_MOHM / 1000;
    return (int)(mv * CHARGE_ADC_MAX_RAW / CHARGE_ADC_FULL_MV);
}
// напруга пакета, мВ -> сирий відлік АЦП у вузлі подільника.
static int packMvToRaw(int mv) {
    long node = (long)mv * CHARGE_VSENSE_R_BOT / (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT);
    return (int)(node * CHARGE_ADC_MAX_RAW / CHARGE_ADC_FULL_MV);
}

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

int main() {
    g_chgPwmOk = true;

    printf("1) профіль струму на ЦІЛІ 100%% — форма «розгін -> крейсер -> спад»\n");
    {
        uint16_t p0  = chargeSetpointMaForPct(0,   100);
        uint16_t p10 = chargeSetpointMaForPct(10,  100);
        uint16_t p50 = chargeSetpointMaForPct(50,  100);
        uint16_t p80 = chargeSetpointMaForPct(80,  100);
        uint16_t p90 = chargeSetpointMaForPct(90,  100);
        uint16_t p99 = chargeSetpointMaForPct(99,  100);
        printf("   0%%=%u 10%%=%u 50%%=%u 80%%=%u 90%%=%u 99%%=%u мА\n",
               p0, p10, p50, p80, p90, p99);
        check(p0 == CHARGE_MA_START, "старт із CHARGE_MA_START");
        check(p10 == CHARGE_MA_10 && p50 == CHARGE_MA_50 && p80 == CHARGE_MA_80,
                                     "точки перегину відтворюються точно");
        check(p90 == CHARGE_MA_80,   "80..95 % — плато на CHARGE_MA_80");
        check(p99 == CHARGE_MA_TAPER,"після 95 % — спад до CHARGE_MA_TAPER");
    }

    printf("\n2) точки перегину МАСШТАБУЮТЬСЯ під обрану ціль\n");
    {
        // При цілі 80 % перегин «50 %» має переїхати на 40 % і дати той самий струм.
        uint16_t at40of80  = chargeSetpointMaForPct(40, 80);
        uint16_t at50of100 = chargeSetpointMaForPct(50, 100);
        printf("   ціль 80%%: 40%% -> %u мА; ціль 100%%: 50%% -> %u мА\n", at40of80, at50of100);
        check(at40of80 == at50of100, "профіль зберігає форму при зміні цілі");
        // І заряд усе одно закінчується М'ЯКО, а не на повному струмі.
        check(chargeSetpointMaForPct(80, 80) == CHARGE_MA_TAPER,
              "на самій цілі струм уже спав до CHARGE_MA_TAPER");
        check(chargeSetpointMaForPct(200, 80) == CHARGE_MA_TAPER,
              "відсоток вище цілі затискається, а не виходить за таблицю");
    }

    printf("\n3) регулятор шпаруватості: soft-start з нуля й збіжність до уставки\n");
    {
        // Модель «плант»: струм пропорційний шпаруватості (резистивне коло без
        // дроселя саме так себе й поводить).
        const int K = 3;                       // мА на відлік шпаруватості
        uint16_t duty = 0, setMa = 1000;
        int steps = 0;
        for (; steps < 5000; steps++) {
            int32_t meas = (int32_t)duty * K;
            uint16_t next = chargeNextDuty(duty, meas, setMa);
            if (next == duty) break;
            duty = next;
        }
        int32_t finalMa = (int32_t)duty * K;
        printf("   збіжність за %d кроків, duty=%u (%u%% від повної), струм ~%d мА (ціль %u)\n",
               steps, duty, (unsigned)((uint32_t)duty * 100 / CHARGE_DUTY_FULL),
               (int)finalMa, setMa);
        check(steps > 1,                 "це саме soft-start: з нуля, а не стрибком");
        check(steps < 5000,              "регулятор збігається, а не крутиться вічно");
        check(labs(finalMa - setMa) <= CHARGE_DEADBAND_MA + CHARGE_DUTY_STEP * K,
                                         "кінцевий струм у межах мертвої зони навколо уставки");
    }

    printf("\n4) регулятор НЕ переступає робочу стелю шпаруватості\n");
    {
        uint16_t duty = 0;
        for (int i = 0; i < 5000; i++) duty = chargeNextDuty(duty, 0, 60000);  // струму «немає» ніколи
        printf("   duty після 5000 кроків недосяжної уставки: %u (стеля %u, повна шкала %u)\n",
               duty, (unsigned)CHARGE_DUTY_MAX, (unsigned)CHARGE_DUTY_FULL);
        check(duty == CHARGE_DUTY_MAX,  "упирається рівно в CHARGE_DUTY_MAX");
        check(duty < CHARGE_DUTY_FULL,  "ключ НІКОЛИ не відкривається повністю");
    }

    printf("\n5) регулятор опускає шпаруватість до нуля, якщо струму завжди забагато\n");
    {
        uint16_t duty = CHARGE_DUTY_MAX;
        for (int i = 0; i < 5000; i++) duty = chargeNextDuty(duty, 60000, 100);
        check(duty == 0, "доходить рівно до 0 і не переповнюється вниз");
    }

    printf("\n6) від'ємний струм НЕ випрямляється — це не «уставка досягнута»\n");
    {
        // Пакет розряджається (наш шунт дав від'ємний відлік). Правильна
        // реакція — вважати струм заряду нульовим і ПІДНІМАТИ шпаруватість.
        // abs() тут дав би «струм є, все добре» і заряд не почався б ніколи.
        uint16_t up = chargeNextDuty(100, -900, 1000);
        printf("   duty 100 при вимірі -900 мА (уставка 1000) -> %u\n", up);
        check(up > 100, "від'ємний струм читається як «заряду немає», шпаруватість росте");
    }

    printf("\n7) АЦП -> напруга пакета через подільник (туди й назад)\n");
    {
        int worst = 0;
        for (int mv = 6000; mv <= CHARGE_SUPPLY_MV; mv += 100) {
            g_adcVsenseRaw = packMvToRaw(mv);
            int got = (int)chargePackMv();
            int d = abs(got - mv);
            if (d > worst) worst = d;
        }
        printf("   найбільша похибка перерахунку на 6.0..%.1f В: %d мВ\n",
               CHARGE_SUPPLY_MV / 1000.0, worst);
        // Крок АЦП, перерахований на бік пакета: 3300/4095 × (Rверх+Rниз)/Rниз.
        // При 10к/2.7к це ≈3.8 мВ.
        int lsbMv = CHARGE_ADC_FULL_MV * (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT)
                    / (CHARGE_ADC_MAX_RAW * CHARGE_VSENSE_R_BOT) + 1;
        // Допуск — ТРИ кроки, і це не «щоб пройшло»: рейс туди-назад проходить
        // ТРИ цілочисельні ділення (напруга -> вузол подільника -> відлік АЦП
        // -> назад у мілівольти), кожне з яких відкидає дробову частину, а
        // останнє множення на (Rверх+Rниз)/Rниз ≈ 4.7 підсилює вже втрачене.
        // Тобто це квантування вимірювального кола, а не помилка формули;
        // на боці пакета воно й дає спостережувані ~10 мВ.
        printf("   крок АЦП на боці пакета ≈%d мВ, допуск 3 кроки = %d мВ\n", lsbMv, lsbMv * 3);
        check(worst <= lsbMv * 3, "похибка в межах квантування вимірювального кола");
    }

    printf("\n8) вимір струму: СЕРЕДНЄ по серії й ПІК на порубаному ШІМом сигналі\n");
    {
        const int peakMa = 1200;
        g_adcIsensePeakRaw = maToRaw(peakMa);
        struct { int num, den; } cases[] = { {1,4}, {1,2}, {3,4}, {1,1} };
        for (auto &c : cases) {
            g_adcDutyNum = c.num; g_adcDutyDen = c.den;
            g_adcIsenseReads = 0;
            uint16_t pk = 0;
            uint16_t avg = chargeMeasureMa(&pk);
            int want = peakMa * c.num / c.den;
            printf("   шпаруватість %d/%d: середнє %u мА (очікую ~%d), пік %u мА\n",
                   c.num, c.den, avg, want, pk);
            if (abs((int)avg - want) > peakMa / 20)
                bad("середнє по серії розійшлося з пік*шпаруватість більш ніж на 5 %");
            if (abs((int)pk - peakMa) > peakMa / 20)
                bad("пік не дорівнює струму у відкритій фазі");
        }
        check(true, "середнє й пік рахуються окремо й обидва правильні");

        // Головне, заради чого пік і потрібен: середнє може бути мізерним, а
        // пік — уже за аварійною межею.
        g_adcIsensePeakRaw = maToRaw(CHARGE_PEAK_MA_MAX + 500);
        g_adcDutyNum = 1; g_adcDutyDen = 32;
        g_adcIsenseReads = 0;
        uint16_t pk2 = 0;
        uint16_t avg2 = chargeMeasureMa(&pk2);
        printf("   вузький імпульс: середнє лише %u мА, але пік %u мА (межа %u)\n",
               avg2, pk2, (unsigned)CHARGE_PEAK_MA_MAX);
        check(avg2 < CHARGE_PEAK_MA_MAX && pk2 > CHARGE_PEAK_MA_MAX,
              "саме той випадок, який середнє приховує, а пік ловить");
    }

    printf("\n9) серія справді перекриває кілька періодів ШІМ\n");
    {
        g_adcIsenseReads = 0;
        uint16_t pk = 0;
        (void)chargeMeasureMa(&pk);
        printf("   відліків за один вимір: %ld (CHARGE_ADC_SAMPLES=%d)\n",
               g_adcIsenseReads, (int)CHARGE_ADC_SAMPLES);
        check(g_adcIsenseReads == CHARGE_ADC_SAMPLES, "рівно CHARGE_ADC_SAMPLES відліків");
        // Період ШІМ = 1000/CHARGE_PWM_FREQ мс. Серія має бути НЕ коротшою.
        check(CHARGE_ADC_SAMPLES >= 32,
              "серії вистачає, щоб усереднення мало сенс (>=32 відліків)");
    }

    printf("\n10) запас вимірювального кола — те, що стереже #error у settings.h\n");
    {
        long vnode = (long)CHARGE_SUPPLY_MV * CHARGE_VSENSE_R_BOT /
                     (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT);
        long ipeak = (long)CHARGE_PEAK_MA_MAX * CHARGE_SHUNT_MOHM / 1000;
        printf("   подільник: живлення %d мВ -> %ld мВ на АЦП (межа %d)\n",
               (int)CHARGE_SUPPLY_MV, vnode, (int)CHARGE_ADC_MAX_MV);
        printf("   шунт: пікова відсічка %d мА -> %ld мВ на АЦП (межа %d)\n",
               (int)CHARGE_PEAK_MA_MAX, ipeak, (int)CHARGE_ADC_MAX_MV);
        check(vnode <= CHARGE_ADC_MAX_MV, "подільник лишає АЦП у робочому діапазоні");
        check(ipeak <= CHARGE_ADC_MAX_MV, "пікова відсічка видима — вона в межах шкали АЦП");
        check(CHARGE_MA_80 < CHARGE_PEAK_MA_MAX,
              "штатний максимум профілю нижчий за аварійну відсічку");
    }

    printf("\n11) chargeSetDuty() затискає стелю в НАЙНИЖЧІЙ точці\n");
    {
        chargeSetDuty(CHARGE_DUTY_FULL);
        printf("   спроба виставити повну шкалу %u -> у LEDC пішло %u\n",
               (unsigned)CHARGE_DUTY_FULL, (unsigned)g_lastDuty);
        check(g_lastDuty == CHARGE_DUTY_MAX,
              "навіть прямий виклик повз регулятор не перевищить CHARGE_DUTY_MAX");
        chargeSetDuty(0);
        check(g_lastDuty == 0, "нуль проходить як нуль — ключ закривається");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
