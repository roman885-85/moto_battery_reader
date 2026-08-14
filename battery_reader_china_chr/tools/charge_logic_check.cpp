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
//    • стартова шпаруватість понижувача (точка нульового струму);
//    • вимір струму: середнє й вершина ПУЛЬСАЦІЙ струму дроселя;
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
//  ⚑ МОДЕЛЬ СТРУМУ — ПУЛЬСАЦІЇ ДРОСЕЛЯ, а не «рубанина» 0/пік.
//  Через шунт тече струм дроселя в ОБИДВІ фази (у відкритій — від живлення,
//  у закритій — через діод), тож сигнал майже неперервний: трикутна хвиля
//  навколо середнього з розмахом ΔI. Раніше тут стояла модель простого ключа
//  (peak або 0), і вона перевіряла б зовсім не ту фізику.
static int      g_adcMidMa = 0;           // середній струм дроселя, мА
static int      g_adcRippleMa = 0;        // повний розмах пульсацій ΔI, мА
static int      g_adcVsenseRaw = 0;
static int      g_adcPsuRaw = 0;          // вузол подільника ЖИВЛЕННЯ
static long     g_adcIsenseReads = 0;
static int analogRead(int pin);

#include "settings.h"

// leds.h підмінюємо — справжній тягне buzzer.h з таблицями й таймерами.
#define LEDS_H
enum LedMode { LED_BOOT, LED_IDLE, LED_READ, LED_WRITE, LED_OK, LED_ERROR,
               LED_FAULT, LED_DISCHARGE, LED_CHARGE, LED_CHARGE_TAPER };
static LedMode g_led = LED_BOOT;
static void ledSet(LedMode m) { g_led = m; }
static LedMode ledMode() { return g_led; }

#include "charge.h"

// мА -> сирий відлік АЦП на шунті — оголошуємо раніше, бо потрібне в analogRead.
static int maToRaw(int ma);

static int analogRead(int pin) {
    if (pin == CHARGE_VSENSE_PIN) return g_adcVsenseRaw;
#ifdef CHARGE_PSU_PIN
    if (pin == CHARGE_PSU_PIN) return g_adcPsuRaw;
#endif
    // Пін струму: трикутна хвиля навколо g_adcMidMa з розмахом g_adcRippleMa.
    // Період беремо 16 відліків — серія зі 128 покриває рівно 8 повних
    // періодів, тож середнє по серії має точно збігтися з g_adcMidMa.
    long i = g_adcIsenseReads++ % 16;
    long tri = (i < 8) ? i : (15 - i);          // 0..7..0
    // Нормуємо трикутник у діапазон [-ΔI/2, +ΔI/2].
    long ma = g_adcMidMa + (long)g_adcRippleMa * (2 * tri - 7) / 14;
    if (ma < 0) ma = 0;
    return maToRaw((int)ma);
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
// напруга ЖИВЛЕННЯ, мВ -> сирий відлік АЦП у вузлі свого подільника.
// Затискаємо на повній шкалі — саме так поводиться реальний АЦП, і саме це
// робить перевірку діапазону подільника осмисленою.
static int psuMvToRaw(int mv) {
    long node = (long)mv * CHARGE_PSU_R_BOT / (CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT);
    long raw  = node * CHARGE_ADC_MAX_RAW / CHARGE_ADC_FULL_MV;
    return (int)(raw > CHARGE_ADC_MAX_RAW ? CHARGE_ADC_MAX_RAW : raw);
}
// Виставити живлення й перечитати його — як це робить прошивка.
static uint8_t setPsu(int mv) { g_adcPsuRaw = psuMvToRaw(mv); return chargePsuPoll(); }

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

    printf("\n3) регулятор шпаруватості: збіжність до уставки\n");
    {
        // Модель «плант» понижувача: I = (D×Uживл − Uпакета)/R, тобто струм
        // лінійний за шпаруватістю вище порога провідності. Беремо спрощено —
        // важлива саме лінійність і крутість, а не абсолютні числа.
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
        check(steps > 1,                 "регулятор іде кроками, а не стрибком");
        check(steps < 5000,              "регулятор збігається, а не крутиться вічно");
        check(labs(finalMa - setMa) <= CHARGE_DEADBAND_MA + CHARGE_DUTY_STEP_MAX * K,
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

    printf("\n6б) стартова шпаруватість: цілиться в ПОТРІБНИЙ струм, а не в «нуль»\n");
    {
        // ⚠️ Тут перевіряємо саме те, на чому спершу була помилка: точка
        // D = Uпак/Uживл — це НЕ нульовий струм, а МЕЖА режимів, і середній
        // струм у ній дорівнює половині розмаху пульсацій.
        uint16_t bnd = chargeBoundaryMa(8250);
        uint16_t dCcm = chargeDutyForMv(8250);
        printf("   межа режимів при 8.25 В: duty %u, струм там %u мА (це НЕ нуль)\n",
               dCcm, bnd);
        check(bnd > 300, "на межі CCM струм справді значний — стартувати звідти не можна");

        // Переривчаста гілка: струм ~D², тож подвоєння струму дає ~×1.41 duty.
        uint16_t d200 = chargeStartDuty(8250, 200);
        uint16_t d400 = chargeStartDuty(8250, 400);
        printf("   DCM: 200 мА -> duty %u; 400 мА -> duty %u (відношення %.2f, очікую ~1.41)\n",
               d200, d400, d200 ? (double)d400 / d200 : 0.0);
        check(d200 > 0 && d400 > d200, "більший струм вимагає більшої шпаруватості");
        {
            double r = d200 ? (double)d400 / d200 : 0.0;
            check(r > 1.30 && r < 1.55, "у переривчастому режимі струм росте як D² (звідси √2)");
        }
        check(d200 < dCcm, "стартова точка нижча за межу режимів — струм менший за 650 мА");

        // Неперервна гілка: вище межі струм лінійний за шпаруватістю.
        uint16_t dHi = chargeStartDuty(8250, 1500);
        printf("   CCM: 1500 мА -> duty %u (вище межі %u)\n", dHi, dCcm);
        check(dHi > dCcm, "за межею режимів потрібна БІЛЬША шпаруватість, ніж на межі");
        check(dHi <= CHARGE_DUTY_MAX, "стартова оцінка затиснута стелею");

        check(chargeStartDuty(8250, 0) == 0,          "нульова уставка -> закритий ключ");
        check(chargeStartDuty(CHARGE_SUPPLY_MV, 500) == 0,
              "пакет на рівні живлення -> заряджати нічим, ключ закритий");
    }

    printf("\n6в) крок регулятора ПРОПОРЦІЙНИЙ похибці\n");
    {
        // Саме це робить контур байдужим до похибки стартової оцінки: L і R
        // відомі приблизно, і фіксований дрібний крок коштував би хвилин.
        // Найменша похибка, яка взагалі рухає шпаруватість, — на 1 мА більша
        // за мертву зону. Саме там крок мусить бути мінімальним.
        uint16_t small = chargeNextDuty(500, 500, 500 + CHARGE_DEADBAND_MA + 1);
        uint16_t mid   = chargeNextDuty(500, 500, 500 + 200);
        uint16_t big   = chargeNextDuty(500, 0,   1500);
        printf("   похибка %d мА -> крок %d; 200 мА -> %d; 1500 мА -> %d (стеля %d)\n",
               CHARGE_DEADBAND_MA + 1, (int)(small - 500), (int)(mid - 500),
               (int)(big - 500), (int)CHARGE_DUTY_STEP_MAX);
        check((int)(small - 500) == CHARGE_DUTY_STEP,
              "щойно за мертвою зоною крок мінімальний");
        check((int)(mid - 500) > CHARGE_DUTY_STEP && (int)(mid - 500) < CHARGE_DUTY_STEP_MAX,
              "середня похибка -> проміжний крок (саме це й означає «пропорційний»)");
        check((int)(big - 500) == CHARGE_DUTY_STEP_MAX, "велика похибка -> крок упирається в стелю");
        check(chargeNextDuty(500, 500, 500) == 500, "у мертвій зоні шпаруватість не рухається");
        check(chargeNextDuty(500, 500, 500 + CHARGE_DEADBAND_MA) == 500,
              "рівно на межі мертвої зони — теж стоїмо");

        // Скільки опитувань треба, щоб закрити типовий промах моделі.
        uint16_t d = chargeStartDuty(8250, 200);
        int polls = 0;
        const int K = 3;                       // мА на відлік (спрощений плант)
        for (; polls < 500; polls++) {
            int32_t meas = (int32_t)d * K;
            uint16_t nx = chargeNextDuty(d, meas, 1000);
            if (nx == d) break;
            d = nx;
        }
        printf("   від стартової точки до уставки 1000 мА: %d опитувань\n", polls);
        check(polls < 60, "промах моделі закривається за десятки секунд, а не за хвилини");
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

    printf("\n8) вимір струму: СЕРЕДНЄ і ВЕРШИНА ПУЛЬСАЦІЙ струму дроселя\n");
    {
        struct { int mid, ripple; } cases[] = { {300,200}, {1000,800}, {1500,1355}, {100,80} };
        for (auto &c : cases) {
            g_adcMidMa = c.mid; g_adcRippleMa = c.ripple;
            g_adcIsenseReads = 0;
            uint16_t pk = 0;
            uint16_t avg = chargeMeasureMa(&pk);
            int wantPk = c.mid + c.ripple / 2;
            printf("   Iсер %4d мА, ΔI %4d -> виміряно середнє %4u, вершина %4u (очікую ~%d)\n",
                   c.mid, c.ripple, avg, pk, wantPk);
            if (abs((int)avg - c.mid) > c.mid / 20 + 10)
                bad("середнє по серії розійшлося із середнім струмом дроселя");
            if (abs((int)pk - wantPk) > c.ripple / 5 + 10)
                bad("вершина не дорівнює Iсер + ΔI/2");
        }
        check(true, "усереднення пульсацій і вершина рахуються окремо");

        // Головне, заради чого вершина й потрібна: якщо дросель випав із кола
        // (обрив, насичення, пробитий ключ), пульсації стають величезними —
        // середнє ще в нормі, а вершина вже за аварійною межею.
        g_adcMidMa = 1200; g_adcRippleMa = 4000;
        g_adcIsenseReads = 0;
        uint16_t pk2 = 0;
        uint16_t avg2 = chargeMeasureMa(&pk2);
        printf("   «дроселя немає»: середнє %u мА (ще в нормі), вершина %u (межа %u)\n",
               avg2, pk2, (unsigned)CHARGE_PEAK_MA_MAX);
        check(avg2 < CHARGE_PEAK_MA_MAX && pk2 > CHARGE_PEAK_MA_MAX,
              "саме той випадок, який середнє приховує, а вершина ловить");
    }

    printf("\n9) серія справді перекриває кілька періодів пульсацій\n");
    {
        g_adcMidMa = 1000; g_adcRippleMa = 400;
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

    printf("\n9б) тепловий і струмовий бюджет БІПОЛЯРНОГО ключа\n");
    {
        // Біполярник керується струмом і помітно гріється — на відміну від
        // MOSFET, це і є головне обмеження стелі профілю струму.
        long ipeak = CHARGE_IPEAK_MA;
        long ibNeed = ipeak / CHARGE_BJT_HFE_FORCED;
        long p = CHARGE_P_COND_MW + CHARGE_P_SW_MW;
        printf("   пік %ld мА (крейсер %d + ΔI/2 %lld)\n",
               ipeak, CHARGE_MA_80, (long long)(CHARGE_RIPPLE_MA_EST / 2));
        printf("   струм бази %d мА, потрібно >= %ld (β_форс %d)\n",
               (int)CHARGE_IB_MA, ibNeed, (int)CHARGE_BJT_HFE_FORCED);
        printf("   розсіювання %ld мВт = провідні %lld + перемикальні %lld, Pc %d\n",
               p, (long long)CHARGE_P_COND_MW, (long long)CHARGE_P_SW_MW,
               (int)CHARGE_BJT_PC_MW);
        check(CHARGE_IB_MA >= ibNeed,
              "струму бази вистачає на насичення НА ВЕРШИНІ пульсацій");

        // ⚑ ДРАЙВЕРНИЙ КАСКАД. Пін не керує силовим ключем напряму — між ними
        // керуючий NPN, через колектор якого тече ВЕСЬ струм бази PNP. Якщо
        // він сам не насичується, відмова каскадна: на NPN падає вольт
        // замість 0.2 В -> струм бази PNP просідає -> з насичення виходить
        // силовий ключ. Тому перевіряємо обидві ланки.
        long npnNeed = (long)CHARGE_IB_MA * 1000 / CHARGE_NPN_HFE_FORCED;
        printf("   драйвер: R %d Ом -> Iб NPN %d мкА, треба >= %ld (β_форс %d)\n",
               (int)CHARGE_NPN_BASE_OHM, (int)CHARGE_NPN_IB_UA, npnNeed,
               (int)CHARGE_NPN_HFE_FORCED);
        check(CHARGE_NPN_IB_UA >= npnNeed,
              "керуючий NPN теж насичується (інакше відмова каскадна)");
        check(CHARGE_NPN_IB_UA <= CHARGE_GPIO_UA_MAX,
              "струм із піна ESP32 у безпечних межах");
        check(ipeak <= CHARGE_BJT_IC_MAX_MA, "пік у межах Ic max ключа");
        check(p <= CHARGE_BJT_PC_MW * 4 / 5, "розсіювання в межах 80 % від Pc");
        // Перемикальні втрати біполярника домінують — саме тому частота тут
        // обмежена зверху, попри те, що дросель хоче її вище.
        check(CHARGE_P_SW_MW > CHARGE_P_COND_MW,
              "перемикальні втрати переважають провідні — це і є ціна повільного вимикання");
    }

    printf("\n9в) режим перетворювача на крейсерському струмі\n");
    {
        // Межа неперервного режиму — ΔI/2, а не ΔI. Помилка вдвічі тут коштує
        // хибного спрацювання перевірки (на цьому вже спіткнулись).
        long half = CHARGE_RIPPLE_MA_EST / 2;
        printf("   ΔI %lld мА, межа CCM = ΔI/2 = %ld, крейсер %d -> %s\n",
               (long long)CHARGE_RIPPLE_MA_EST, half, CHARGE_MA_80,
               CHARGE_MA_80 > half ? "неперервний" : "переривчастий");
        check(CHARGE_MA_80 > half, "на крейсері перетворювач у неперервному режимі");
        // А на фінальному спаді — законно переривчастий, і це нормально.
        check(CHARGE_MA_TAPER < half,
              "на фінальному спаді режим переривчастий — так і має бути");
    }

    printf("\n10) запас вимірювального кола — те, що стереже #error у settings.h\n");
    {
        // ⚑ Дві РІЗНІ умови, а не одна. Штатно на клемі стоїть напруга
        // ПАКЕТА — вона мусить лягати в робочу стелю АЦП. Напруга ЖИВЛЕННЯ
        // опиняється там лише у нештатному випадку (пакет від'єднали посеред
        // заряду), і там питання не в стелі, а в СТРУМІ через захисний діод.
        long vread = (long)CHARGE_VSENSE_MAX_READ_MV * CHARGE_VSENSE_R_BOT /
                     (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT);
        long ipeak = (long)CHARGE_PEAK_MA_MAX * CHARGE_SHUNT_MOHM / 1000;
        printf("   діапазон виміру: %d мВ -> %ld мВ на АЦП (стеля %d)\n",
               (int)CHARGE_VSENSE_MAX_READ_MV, vread, (int)CHARGE_ADC_MAX_MV);
        printf("   нештатно (живлення на клемі): вузол %d мВ, R_дж %d Ом, "
               "струм у діод %d мкА (межа %d)\n",
               (int)CHARGE_VSENSE_NODE_AT_SUPPLY_MV, (int)CHARGE_VSENSE_SRC_OHM,
               (int)CHARGE_VSENSE_CLAMP_UA, (int)CHARGE_ADC_CLAMP_UA_MAX);
        printf("   шунт: пікова відсічка %d мА -> %ld мВ на АЦП (межа %d)\n",
               (int)CHARGE_PEAK_MA_MAX, ipeak, (int)CHARGE_ADC_MAX_MV);
        check(vread <= CHARGE_ADC_MAX_MV,
              "робочий діапазон виміру лягає в стелю АЦП");
        check(CHARGE_VSENSE_CLAMP_UA <= CHARGE_ADC_CLAMP_UA_MAX,
              "у нештатному випадку струм у захисний діод входу безпечний");
        check((long)CHARGE_TARGET_MV + CHARGE_HARD_MAX_HEADROOM_MV < CHARGE_VSENSE_MAX_READ_MV,
              "аварійна межа напруги — усередині діапазону, який АЦП ще розрізняє");
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

    printf("\n12) контроль живлення +14 В: класифікація й перерахунок\n");
    {
        // Перерахунок туди-назад через подільник живлення.
        long worst = 0;
        for (int mv = 6000; mv <= CHARGE_PSU_MAX_READ_MV; mv += 250) {
            g_adcPsuRaw = psuMvToRaw(mv);
            long back = chargePsuReadMv();
            long d = back > mv ? back - mv : mv - back;
            if (d > worst) worst = d;
        }
        // Крок АЦП на боці живлення: один відлік = (R_в+R_н)/R_н × крок АЦП.
        long stepMv = (long)CHARGE_ADC_FULL_MV * (CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT) /
                      (CHARGE_PSU_R_BOT * CHARGE_ADC_MAX_RAW);
        printf("   найбільша похибка перерахунку 6.0..%.1f В: %ld мВ "
               "(крок АЦП тут ≈%ld мВ)\n",
               CHARGE_PSU_MAX_READ_MV / 1000.0, worst, stepMv);
        check(worst <= stepMv * 3, "перерахунок живлення в межах квантування");

        printf("   пороги: немає <%d, занижено <%d, норма, завищено >%d мВ\n",
               (int)CHARGE_PSU_ABSENT_MV, (int)CHARGE_PSU_MIN_MV, (int)CHARGE_PSU_MAX_MV);
        check(setPsu(0)     == PSU_ABSENT, "блок не під'єднано -> PSU_ABSENT");
        check(setPsu(3000)  == PSU_ABSENT, "залишкова напруга без блока -> PSU_ABSENT");
        // 8 В — саме той випадок, заради якого поріг «немає» піднято над нулем:
        // пакет підживлює шину через перехід база-колектор силового PNP.
        check(setPsu(8000)  == PSU_LOW,    "підживлення від пакета читається як «занижено», а не «норма»");
        check(setPsu(12000) == PSU_LOW,    "12-вольтовий блок -> PSU_LOW (не той блок)");
        check(setPsu(14000) == PSU_OK,     "штатні 14 В -> норма");
        check(setPsu(19000) == PSU_HIGH,   "19-вольтовий блок від ноутбука -> PSU_HIGH");
        setPsu(14000);
        check(setPsu(CHARGE_PSU_MAX_MV - CHARGE_PSU_HYST_MV) == PSU_OK,
              "верхня межа з запасом — норма");
        check(setPsu(CHARGE_PSU_MIN_MV + CHARGE_PSU_HYST_MV) == PSU_OK,
              "нижня межа з запасом — норма");

        // ГІСТЕРЕЗИС. Без нього блок, що стоїть рівно на порозі, блимав би
        // «помилка/норма» на кожному опитуванні — і шум АЦП тут не гіпотеза,
        // а десятки мілівольт.
        setPsu(14000);
        check(setPsu(CHARGE_PSU_MIN_MV - 50) == PSU_LOW,
              "трохи нижче порога — помилка піднімається за ЧИСТИМ порогом");
        check(setPsu(CHARGE_PSU_MIN_MV + 50) == PSU_LOW,
              "трохи вище порога помилка ЩЕ ТРИМАЄТЬСЯ — це і є гістерезис");
        check(setPsu(CHARGE_PSU_MIN_MV + CHARGE_PSU_HYST_MV + 50) == PSU_OK,
              "знімається лише при поверненні в діапазон із запасом");
        setPsu(14000);
        check(setPsu(CHARGE_PSU_MAX_MV + 50) == PSU_HIGH, "дзеркально для верхньої межі");
        check(setPsu(CHARGE_PSU_MAX_MV - 50) == PSU_HIGH, "і там теж не відпускає одразу");
        check(setPsu(CHARGE_PSU_MAX_MV - CHARGE_PSU_HYST_MV - 50) == PSU_OK,
              "верхня межа теж відпускає лише із запасом");
        setPsu(14000);
        // Головне, заради чого пороги й існують: несправність мусить бути
        // ВИДИМОЮ як несправність, а не лише «не норма».
        setPsu(12000);
        check(chargePsuFault(), "занижене живлення піднімає ознаку несправності");
        setPsu(14000);
        check(!chargePsuFault(), "справне живлення ознаку знімає");
        check(psuMvToRaw(19000) < CHARGE_ADC_MAX_RAW,
              "помилковий блок 19 В читається числом, а не «залипає» на 4095");
    }

    printf("\n12б) розрахунок іде на ВИМІРЯНОМУ живленні, а не на константі\n");
    {
        // Це і є сенс усього подільника живлення: та сама уставка на різних
        // блоках вимагає різної шпаруватості, і стартова оцінка мусить це
        // враховувати, інакше контур витрачає десятки опитувань на надолуження.
        setPsu(14000);
        uint16_t d14 = chargeStartDuty(8000, 400);
        uint16_t b14 = chargeBoundaryMa(8000);
        setPsu(12500);
        uint16_t d12 = chargeStartDuty(8000, 400);
        uint16_t b12 = chargeBoundaryMa(8000);
        printf("   пакет 8.0 В, уставка 400 мА: при 14.0 В -> duty %u (межа CCM %u мА), "
               "при 12.5 В -> duty %u (межа CCM %u мА)\n", d14, b14, d12, b12);
        check(d12 > d14, "на просілому живленні стартова шпаруватість БІЛЬША");
        check(b12 < b14, "межа неперервного режиму теж рахується на живому вимірі");

        // Запасний шлях: якщо живлення не читається (немає блока), розрахунок
        // мусить впасти на номінал, а не ділити на майже нуль.
        setPsu(0);
        printf("   без блока живлення chargeSupplyMv() -> %u мВ (номінал %d)\n",
               chargeSupplyMv(), (int)CHARGE_SUPPLY_MV);
        check(chargeSupplyMv() == CHARGE_SUPPLY_MV,
              "неправдоподібний вимір до розрахунків не пускається");
        // Не «те саме число», а «те саме з точністю до квантування»: вимір
        // 14.000 В повертається як 13.985 (крок АЦП на цій шині ~6 мВ), і
        // вимагати побітового збігу означало б перевіряти АЦП, а не логіку.
        int dfb = (int)chargeStartDuty(8000, 400) - (int)d14;
        printf("   запасний шлях: duty %u проти %u (різниця %d відліків)\n",
               chargeStartDuty(8000, 400), d14, dfb);
        check(dfb > -4 && dfb < 4,
              "запасний шлях дає ту саму робочу точку, що й номінальне живлення");
        setPsu(14000);
    }

    printf("\n13) обв'язка бази: що дає ПЛАТА і чого вимагає розрахунок\n");
    {
        // Тут перевіряється не прошивка, а ЧЕСНІСТЬ звіту про залізо: числа,
        // які пристрій друкує при старті, мусять збігатися з арифметикою.
        printf("   треба: база PNP %d Ом, база NPN %d Ом -> Iб(PNP) %d мА, Iб(NPN) %d мкА\n",
               (int)CHARGE_BASE_DRIVE_OHM, (int)CHARGE_NPN_BASE_OHM,
               (int)CHARGE_IB_MA, (int)CHARGE_NPN_IB_UA);
        printf("   на платі: %d Ом і %d Ом -> Iб(PNP) %d мА, Iб(NPN) %d мкА, "
               "стеля ключа ~%d мА замість %d мА\n",
               (int)CHARGE_BASE_DRIVE_ASBUILT_OHM, (int)CHARGE_NPN_BASE_ASBUILT_OHM,
               (int)CHARGE_ASBUILT_IB_MA, (int)CHARGE_ASBUILT_NPN_IB_UA,
               (int)CHARGE_ASBUILT_IC_MAX_MA, (int)CHARGE_IPEAK_MA);
        check(!CHARGE_HW_REWORK_DONE,
              "плата ЩЕ НЕ доопрацьована — прапорець це визнає");
        check(CHARGE_ASBUILT_IC_MAX_MA < CHARGE_IPEAK_MA,
              "нинішня обв'язка не тягне робочий пік — саме тому потрібна заміна");
        check(CHARGE_ASBUILT_NPN_IB_UA * CHARGE_NPN_HFE_FORCED < CHARGE_ASBUILT_DRIVE_UA,
              "вужче місце — керуючий NPN: він не пропускає навіть нинішній струм драйвера");
        // Найгірші кути діапазону живлення — те, на чому 150/470 Ом і провалились.
        printf("   на межах допуску: Iб(PNP) при %d мВ = %d мА (треба %d), "
               "струм драйвера при %d мВ = %d мкА (NPN дає %d)\n",
               (int)CHARGE_PSU_MIN_MV, (int)CHARGE_IB_AT_MIN_PSU_MA,
               (int)(CHARGE_IPEAK_MA / CHARGE_BJT_HFE_FORCED),
               (int)CHARGE_PSU_MAX_MV, (int)CHARGE_DRIVE_AT_MAX_PSU_UA,
               (int)(CHARGE_NPN_IB_UA * CHARGE_NPN_HFE_FORCED));
        check(CHARGE_IB_AT_MIN_PSU_MA >= CHARGE_IPEAK_MA / CHARGE_BJT_HFE_FORCED,
              "на НИЖНІЙ межі живлення силовий ключ ще насичується");
        check(CHARGE_NPN_IB_UA >= CHARGE_DRIVE_AT_MAX_PSU_UA / CHARGE_NPN_HFE_FORCED,
              "на ВЕРХНІЙ межі живлення керуючий NPN ще насичується");
        check(CHARGE_DRIVE_AT_MAX_PSU_UA <= (long)CHARGE_NPN_IC_MAX_MA * 1000,
              "струм через керуючий NPN у межах його Ic max");
        check(CHARGE_NPN_IB_UA <= CHARGE_GPIO_UA_MAX,
              "струм із піна ESP32 у безпечних межах і на новому номіналі");
    }

    printf("\n14) відсічка за ЖИВЛЕННЯМ: рахує СТАЛИЙ стан, а не поодинокий провал\n");
    {
        uint8_t n = 0;
        bool tripped = false;
        for (int i = 1; i < CHARGE_PSU_BAD_POLLS; i++)
            tripped |= chargePsuTrip(PSU_LOW, &n);
        printf("   %d опитувань поспіль «занижено» -> %s (поріг %d)\n",
               CHARGE_PSU_BAD_POLLS - 1, tripped ? "ЗУПИНКА" : "ще терпимо",
               (int)CHARGE_PSU_BAD_POLLS);
        check(!tripped, "менше за поріг — заряд не зупиняється");
        check(chargePsuTrip(PSU_LOW, &n), "на порозі — зупинка");

        // Одна нормальна відповідь скидає лічильник: просадка на кидку струму
        // не мусить накопичуватись до аварії протягом усього заряду.
        n = 0;
        for (int i = 0; i < 50; i++) {
            chargePsuTrip(PSU_LOW, &n);      // провал...
            if (chargePsuTrip(PSU_OK, &n)) { bad("норма не скинула лічильник"); break; }
        }
        check(n == 0, "поодинокі провали між нормальними вимірами не накопичуються");

        n = 0;
        check(!chargePsuTrip(PSU_UNKNOWN, &n),
              "плата без контролю живлення (PSU_UNKNOWN) заряд не зупиняє");
    }

    printf("\n15) відсічка «КЛЮЧ НЕ ТЯГНЕ»: стеля шпаруватості без струму\n");
    {
        uint8_t n = 0;
        // Робочий режим: струм на уставці, шпаруватість не в стелі.
        for (int i = 0; i < 20; i++)
            if (chargeNoDriveTrip(CHARGE_DUTY_MAX / 2, 1000, 1000, &n)) bad("штатний режим зупинено");
        check(n == 0, "штатний режим відсічку не чіпає");

        // Стеля шпаруватості, але струм у нормі — теж не привід зупинятись:
        // так виглядає кінець заряду на просілому живленні.
        n = 0;
        for (int i = 0; i < 20; i++)
            if (chargeNoDriveTrip(CHARGE_DUTY_MAX, 1000, 1000, &n)) bad("стеля зі струмом зупинена");
        check(n == 0, "сама лише стеля шпаруватості — не аварія, поки струм є");

        // Недобір струму, але шпаруватість ще НЕ в стелі — регулятору є куди
        // рости, зупинятись зарано.
        n = 0;
        for (int i = 0; i < 20; i++)
            if (chargeNoDriveTrip(CHARGE_DUTY_MAX - 1, 0, 1000, &n)) bad("зупинено до виходу на стелю");
        check(n == 0, "поки є запас шпаруватості — даємо регулятору працювати");

        // А ось це і є плата з нинішньою обв'язкою: стеля й ~34 мА замість 1000.
        n = 0;
        int polls = 0;
        while (!chargeNoDriveTrip(CHARGE_DUTY_MAX, CHARGE_ASBUILT_IC_MAX_MA, 1000, &n)) {
            if (++polls > 100) { bad("відсічка так і не спрацювала"); break; }
        }
        printf("   плата як є (%d мА при уставці 1000): зупинка на %d-му опитуванні "
               "(поріг %d, ~%lu с)\n", (int)CHARGE_ASBUILT_IC_MAX_MA, polls + 1,
               (int)CHARGE_NODRIVE_POLLS,
               (unsigned long)((polls + 1) * CHARGE_POLL_MS / 1000));
        check(polls + 1 == CHARGE_NODRIVE_POLLS, "спрацьовує рівно на CHARGE_NODRIVE_POLLS");
        check((polls + 1) * CHARGE_POLL_MS <= 30000UL,
              "зупинка настає за десятки секунд, а не за години");

        // Межа: рівно половина уставки — ще НЕ голодування (поріг строгий).
        n = 0;
        check(!chargeNoDriveTrip(CHARGE_DUTY_MAX, 500, 1000, &n),
              "рівно на порозі CHARGE_NODRIVE_PCT ще не рахується");
        // Один нормальний вимір скидає лічильник.
        n = 0;
        chargeNoDriveTrip(CHARGE_DUTY_MAX, 0, 1000, &n);
        chargeNoDriveTrip(CHARGE_DUTY_MAX, 1000, 1000, &n);
        check(n == 0, "нормальний струм скидає лічильник голодування");
        // Від'ємний струм читається як нуль, а не як «уставку перевищено».
        n = 0;
        chargeNoDriveTrip(CHARGE_DUTY_MAX, -500, 1000, &n);
        check(n == 1, "від'ємний струм — це голодування, а не надлишок");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
