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

// мА -> сирий відлік АЦП на шунті — оголошуємо раніше, бо потрібне в analogRead.
static int maToRaw(int ma);

static int analogRead(int pin) {
    if (pin == CHARGE_VSENSE_PIN) return g_adcVsenseRaw;
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

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
