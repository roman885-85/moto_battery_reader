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
    // ⚑ Модель РЕАЛЬНОГО тракту, а не ідеального. Тракт занижує рівно настільки,
    //  наскільки його виправляє CHARGE_PSU_CAL_X1000 — інакше тест ганяв би
    //  прошивку по бездоганному АЦП, де поправці нічого виправляти, і мовчав би
    //  саме тоді, коли її переплутали або загубили. Тепер «мВ -> відлік» і
    //  «відлік -> мВ» замикаються тільки якщо поправка на місці й правильна.
    node = (node * 1000 + CHARGE_PSU_CAL_X1000 / 2) / CHARGE_PSU_CAL_X1000;
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
        // Одразу ДО і одразу ПІСЛЯ порога дозаряду — межу перевіряємо саме
        // тут, а не на круглих числах: поріг тепер константа, і тест мусить
        // рухатись разом із нею, а не тримати зашите «95».
        uint16_t pBefore = chargeSetpointMaForPct(CHARGE_TAPER_PCT - 1, 100);
        uint16_t pAt     = chargeSetpointMaForPct(CHARGE_TAPER_PCT,     100);
        uint16_t p99 = chargeSetpointMaForPct(99,  100);
        printf("   0%%=%u 10%%=%u 50%%=%u 80%%=%u | поріг дозаряду %d%%: до=%u на=%u | 99%%=%u мА\n",
               p0, p10, p50, p80, (int)CHARGE_TAPER_PCT, pBefore, pAt, p99);
        check(p0 == CHARGE_MA_START, "старт із CHARGE_MA_START");
        check(p10 == CHARGE_MA_10 && p50 == CHARGE_MA_50 && p80 == CHARGE_MA_80,
                                     "точки перегину відтворюються точно");
        check(pBefore == CHARGE_MA_80,
                                     "до порога дозаряду тримається плато CHARGE_MA_80");
        check(pAt == CHARGE_MA_TAPER,
                                     "НА порозі струм уже дозарядний — спад ступінчастий, без «майже»");
        check(p99 == CHARGE_MA_TAPER,"і тримається таким до самої цілі");
        // Дозаряд мусить бути справді малим — інакше він лише називається так.
        check(CHARGE_MA_TAPER * 3 <= CHARGE_MA_80,
                                     "дозарядний струм щонайменше втричі менший за крейсерський");
        check(CHARGE_LED_TAPER_PCT <= CHARGE_TAPER_PCT,
                                     "індикатор перемикається не пізніше, ніж падає струм");
    }

    printf("\n1а) ГІСТЕРЕЗИС на порозі дозаряду — уставка не брязкає\n");
    {
        // Реальний слід із пристрою: відсоток гуляв 89-91-90-89-90-89, і
        // уставка стрибала 1000 <-> 100 мА ЩОСЕКУНДИ. Причина не в порозі, а в
        // тому, що відсоток рахується з напруги, а вона на межі природно
        // коливається на десятки мілівольт.
        const int seq[] = { 87, 89, 91, 90, 89, 90, 89, 90, 91, 90 };

        // БЕЗ гістерезису — рахуємо перемикання.
        int flapsPlain = 0; uint16_t prev = 0;
        for (int i = 0; i < 10; i++) {
            uint16_t ma = chargeSetpointMaForPct(seq[i], 100);
            if (i && ma != prev) flapsPlain++;
            prev = ma;
        }
        // З гістерезисом.
        bool taper = false; int flapsH = 0; prev = 0;
        for (int i = 0; i < 10; i++) {
            uint16_t ma = chargeSetpointMaForPctH(seq[i], 100, &taper);
            if (i && ma != prev) flapsH++;
            prev = ma;
        }
        printf("   послідовність 87,89,91,90,89,90,89,90,91,90 -> перемикань: "
               "без гістерезису %d, з гістерезисом %d\n", flapsPlain, flapsH);
        check(flapsPlain >= 4, "без гістерезису уставка справді брязкає (це і був дефект)");
        check(flapsH == 1,     "з гістерезисом перехід рівно один — і назад не вертається");

        // Вхід — на порозі; вихід — лише помітно нижче.
        taper = false;
        check(chargeSetpointMaForPctH(CHARGE_TAPER_PCT - 1, 100, &taper) == CHARGE_MA_80 && !taper,
              "на піввідсотка нижче порога — ще крейсерський струм");
        check(chargeSetpointMaForPctH(CHARGE_TAPER_PCT, 100, &taper) == CHARGE_MA_TAPER && taper,
              "на порозі — перехід на дозаряд");
        check(chargeSetpointMaForPctH(CHARGE_TAPER_PCT - 1, 100, &taper) == CHARGE_MA_TAPER && taper,
              "просів на 1 % — НЕ повертаємось (це і є гістерезис)");
        check(chargeSetpointMaForPctH(CHARGE_TAPER_PCT - CHARGE_TAPER_HYST_PCT, 100, &taper)
                  == CHARGE_MA_TAPER && taper,
              "рівно на межі повернення — ще тримаємось");
        check(chargeSetpointMaForPctH(CHARGE_TAPER_PCT - CHARGE_TAPER_HYST_PCT - 1, 100, &taper)
                  == CHARGE_MA_80 && !taper,
              "нижче межі повернення — назад на крейсерський");
        // Гістерезис мусить бути меншим за саму ділянку дозаряду, інакше він
        // з'їв би її цілком.
        check(CHARGE_TAPER_HYST_PCT > 0 && CHARGE_TAPER_HYST_PCT < 100 - CHARGE_TAPER_PCT,
              "гістерезис вужчий за ділянку дозаряду");
    }

    printf("\n1б) ЦІЛЬ ЗАРЯДУ ЗА НАПРУГОЮ — нижче верху шкали, і це навмисно\n");
    {
        printf("   ціль заряду %d мВ, верх шкали паливоміра %d мВ -> наприкінці заряду покаже %d %%\n",
               (int)CHARGE_TARGET_MV, (int)BATTERY_FULL_MV,
               impresPercentFromMv(CHARGE_TARGET_MV));
        check(CHARGE_TARGET_MV <= BATTERY_FULL_MV,
              "ціль не виходить за верх шкали паливоміра");
        check(CHARGE_TARGET_MV > DISCHARGE_TARGET_MV,
              "ціль заряду вище цілі розряду — режими не тягнуть пакет у різні боки");
        // ⚑ Це і є той наслідок, який легко прийняти за недозаряд, і тепер він
        //  має чесне пояснення. 8.20 В на 2S — це 4.10 В на банку, а 4.10 В за
        //  кривою літію рівно 90 %. Тобто заряд до 8.2 В свідомо недобирає
        //  останню десяту частину заради ресурсу банок, і шкала про це чесно
        //  каже. Раніше та сама зупинка показувала 97 % — але лише тому, що
        //  шкала була лінійна й верх у неї стояв на 8.25 В.
        printf("   8.20 В = %.2f В на банку -> %d %% за кривою\n",
               CHARGE_TARGET_MV / 2000.0, impresPercentFromMv(CHARGE_TARGET_MV));
        check(impresPercentFromMv(CHARGE_TARGET_MV) >= 88 &&
              impresPercentFromMv(CHARGE_TARGET_MV) <= 92,
              "наприкінці повного заряду паливомір показує ~90 % — це норма, а не недозаряд");
        // І щоб цифра не «повисла в повітрі»: 90 % тут не підібрано, а випливає
        // з того, що ціль стоїть рівно на вузлі кривої 4.10 В/банку.
        check(CHARGE_TARGET_MV == 4100 * SOC_CELLS,
              "ціль заряду стоїть рівно на вузлі кривої (4.10 В на банку)");
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

#if CHARGE_SW_IS_MOS
    printf("\n9б) тепловий і струмовий бюджет ПОЛЬОВОГО ключа (%s)\n", CHARGE_SW_NAME);
    {
        // Польовий керується НАПРУГОЮ, тож головні числа тут зовсім інші, ніж
        // у біполярника: не струм бази, а напруга на затворі; не Uнас × I, а
        // I²R; не розсмоктування заряду бази, а перезаряд Qg нашим драйвером.
        long ipeak = CHARGE_IPEAK_MA;
        long p = CHARGE_P_COND_MW + CHARGE_P_SW_MW;
        printf("   пік %ld мА (крейсер %d + ΔI/2 %lld), Id max %d мА\n",
               ipeak, CHARGE_MA_80, (long long)(CHARGE_RIPPLE_MA_EST / 2),
               (int)CHARGE_MOS_ID_MAX_MA);
        printf("   дільник затвора %d/%d Ом -> Uзатв %d мВ, |VGS| %d мВ, наскрізний %d мкА\n",
               (int)CHARGE_BASE_DRIVE_OHM, (int)CHARGE_BASE_PULLUP_OHM,
               (int)CHARGE_MOS_VG_ON_MV, (int)CHARGE_MOS_VGS_ON_MV,
               (int)CHARGE_MOS_IDIV_UA);
        printf("   |VGS| на краях допуску: %d мВ (%d В) … %d мВ (%d В), поріг %d, межа %d\n",
               (int)CHARGE_MOS_VGS_AT_MIN_PSU_MV, (int)CHARGE_PSU_MIN_MV / 1000,
               (int)CHARGE_MOS_VGS_AT_MAX_PSU_MV, (int)CHARGE_PSU_MAX_MV / 1000,
               (int)CHARGE_MOS_VGSTH_MAX_MV, (int)CHARGE_MOS_VGS_MAX_MV);
        printf("   перемикання: вмик %ld нс + вимик %ld нс = %ld нс (%ld%% періоду)\n",
               (long)CHARGE_MOS_TON_NS, (long)CHARGE_MOS_TOFF_NS, (long)CHARGE_MOS_TSW_NS,
               (long)CHARGE_MOS_TSW_NS * CHARGE_PWM_FREQ / 10000000L);
        printf("   розсіювання %ld мВт = провідні %lld (I²R) + перемикальні %lld, Pd %d\n",
               p, (long long)CHARGE_P_COND_MW, (long long)CHARGE_P_SW_MW,
               (int)CHARGE_MOS_PD_MW);

        // Відкривання: запас над порогом мусить бути на НИЖНІЙ межі живлення —
        // дільник живиться з тієї ж шини, тож просілий блок слабшає й керування.
        check(CHARGE_MOS_VGS_AT_MIN_PSU_MV >= CHARGE_MOS_VGSTH_MAX_MV * 3 / 2,
              "на просілому живленні |VGS| усе ще з запасом над порогом");
        // Запирання: абсолютна межа затвора — на ВЕРХНІЙ межі живлення.
        check(CHARGE_MOS_VGS_AT_MAX_PSU_MV <= CHARGE_MOS_VGS_MAX_MV * 4 / 5,
              "на завищеному живленні затвор не пробиває");
        // Ключ мусить бути ВІДКРИТИЙ, а не «трохи прочинений»: паспортний
        // RDS(on) нормований при своїй напрузі затвора, і нижче неї опір
        // більший за той, з яким рахувалися провідні втрати.
        check(CHARGE_MOS_VGS_ON_MV >= CHARGE_MOS_RDSON_VGS_MV,
              "|VGS| досягає точки, у якій паспорт дає RDS(on) — інакше втрати вищі за розрахункові");
        check(ipeak <= CHARGE_MOS_ID_MAX_MA, "пік у межах Id max ключа");
        check(p <= CHARGE_MOS_PD_MW * 4 / 5, "розсіювання в межах 80 % від Pd");
        // Вимикання повільніше за вмикання — угору затвор тягне лише підтяжка,
        // а вниз обидва плеча дільника. Це не дефект, це властивість схеми, і
        // саме вимикання задає перемикальні втрати.
        check(CHARGE_MOS_TOFF_NS > CHARGE_MOS_TON_NS,
              "запирання повільніше за відкривання — його й обмежує підтяжка");
        check((long)CHARGE_MOS_TSW_NS * CHARGE_PWM_FREQ <= 100000000L,
              "перемикання займає не більше 10 % періоду ШІМ");
        // І головна вигода заміни: у біполярника драйвер палив майже ват
        // постійно, тут наскрізний струм дільника на порядок менший.
        printf("   наскрізні втрати драйвера: %ld мВт (у біполярника було б %ld мВт)\n",
               (long)CHARGE_SUPPLY_MV * CHARGE_MOS_IDIV_UA / 1000000L,
               (long)CHARGE_SUPPLY_MV * CHARGE_BASE_DRIVE_UA / 1000000L);
    }
#else
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
#endif

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
        // Крок АЦП на боці живлення: один відлік = (R_в+R_н)/R_н × крок АЦП,
        // ще й розтягнутий поправкою тракту.
        long stepMv = (long)CHARGE_ADC_FULL_MV * (CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT) *
                      CHARGE_PSU_CAL_X1000 /
                      ((long)CHARGE_PSU_R_BOT * CHARGE_ADC_MAX_RAW * 1000);
        printf("   найбільша похибка перерахунку 6.0..%.1f В: %ld мВ "
               "(крок АЦП тут ≈%ld мВ)\n",
               CHARGE_PSU_MAX_READ_MV / 1000.0, worst, stepMv);
        //  Допуск: три кроки АЦП плюс ОДИН «крок вузла». Другий доданок — не
        //  запас про всяк випадок, а ціна поправки: вона додає в ланцюг ще
        //  одну точку цілочисельного ділення, а кожен загублений там мілівольт
        //  коштує (R_в+R_н)/R_н × поправка мілівольтів на боці живлення.
        long nodeStepMv = (long)(CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT) *
                          CHARGE_PSU_CAL_X1000 / ((long)CHARGE_PSU_R_BOT * 1000);
        check(worst <= stepMv * 3 + nodeStepMv,
              "перерахунок живлення в межах квантування");

        // ── ПОПРАВКА ТРАКТУ НА ЖИВИХ ЧИСЛАХ ВЛАСНИКА ─────────────────────
        //  Це не абстрактний множник: 1973 — саме той відлік, який видавав
        //  АЦП на платі, коли блок живлення стояв на 14.3 В, а прошивка
        //  показувала 12.4 В (мультиметр на самому GPIO39 показував 1.81 В
        //  при 14.4 В, тобто подільник справний — губилось усередині АЦП).
        //  Перевірка тримає весь ланцюг разом: поміняєте поправку, номінали
        //  подільника чи шкалу АЦП — і 14.3 В перестануть сходитись.
        {
            const long FIELD_RAW = 1973, FIELD_TRUE_MV = 14300;
            g_adcPsuRaw = (int)FIELD_RAW;
            long got = chargePsuReadMv();
            long raw = (long)chargeAdcMv(CHARGE_PSU_PIN) *
                       (CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT) / CHARGE_PSU_R_BOT;
            printf("   відлік %ld із плати: без поправки %ld мВ, з поправкою %ld мВ "
                   "(насправді %ld мВ)\n", FIELD_RAW, raw, got, FIELD_TRUE_MV);
            check(labs(got - FIELD_TRUE_MV) <= 200,
                  "поправка зводить показання з дійсністю на живих числах плати");
            check(raw < CHARGE_PSU_MIN_MV,
                  "…і без неї той самий відлік справді падав нижче порога (те, на що скаржився власник)");
            check(chargePsuClassify((uint16_t)got, PSU_LOW) == PSU_OK,
                  "…тож 14.3 В на цій платі більше не «занижене живлення»");
        }
        setPsu(14000);

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

#if CHARGE_SW_IS_MOS
    printf("\n13) обв'язка затвора: що дає ПЛАТА і чого вимагає розрахунок\n");
    {
        printf("   треба: R_drive %d Ом, база NPN %d Ом -> |VGS| %d мВ, Iб(NPN) %d мкА\n",
               (int)CHARGE_BASE_DRIVE_OHM, (int)CHARGE_NPN_BASE_OHM,
               (int)CHARGE_MOS_VGS_ON_MV, (int)CHARGE_NPN_IB_UA);
        printf("   на платі: %d Ом і %d Ом -> |VGS| %d мВ, Iб(NPN) %d мкА, "
               "наскрізний %d мкА\n",
               (int)CHARGE_BASE_DRIVE_ASBUILT_OHM, (int)CHARGE_NPN_BASE_ASBUILT_OHM,
               (int)CHARGE_MOS_VGS_ON_ASBUILT_MV, (int)CHARGE_ASBUILT_NPN_IB_UA,
               (int)CHARGE_MOS_IDIV_ASBUILT_UA);
        check(!CHARGE_HW_REWORK_DONE,
              "плата ЩЕ НЕ доопрацьована — прапорець це визнає");

        // ⚑ ГОЛОВНЕ, ЩО ЗМІНИЛА ЗАМІНА КЛЮЧА, і що варто бачити числами.
        //  У біполярника обидві заміни були обов'язкові: 1 кОм у базі означав
        //  ~22 мА заряду, тобто чотири доби. У польового 1 кОм дає |VGS|, якої
        //  ВЖЕ вистачає, щоб відкрити ключ, — просто з гіршим опором каналу.
        //  А ось 20 кОм у базі NPN лишається смертельним і тут.
        check(CHARGE_MOS_VGS_ON_ASBUILT_MV > CHARGE_MOS_VGSTH_MAX_MV,
              "на НИНІШНЬОМУ R_drive польовий ключ уже відкривається (біполярний — ні)");
        check(CHARGE_MOS_VGS_ON_ASBUILT_MV < CHARGE_MOS_RDSON_VGS_MV,
              "але не до паспортної точки RDS(on) — опір каналу буде більший за 0.18 Ом");
        check(CHARGE_ASBUILT_NPN_IB_UA * CHARGE_NPN_HFE_FORCED < CHARGE_MOS_IDIV_ASBUILT_UA,
              "а ось керуючий NPN при 20 кОм не пропускає навіть нинішній струм дільника");

        printf("   на межах допуску: |VGS| при %d мВ = %d мВ (треба >= %d), "
               "при %d мВ = %d мВ (межа %d)\n",
               (int)CHARGE_PSU_MIN_MV, (int)CHARGE_MOS_VGS_AT_MIN_PSU_MV,
               (int)(CHARGE_MOS_VGSTH_MAX_MV * 3 / 2),
               (int)CHARGE_PSU_MAX_MV, (int)CHARGE_MOS_VGS_AT_MAX_PSU_MV,
               (int)CHARGE_MOS_VGS_MAX_MV);
        check(CHARGE_NPN_IB_UA >= CHARGE_MOS_IDIV_AT_MAX_PSU_UA / CHARGE_NPN_HFE_FORCED,
              "на ВЕРХНІЙ межі живлення керуючий NPN ще насичується");
        check(CHARGE_MOS_IDIV_AT_MAX_PSU_UA <= (long)CHARGE_NPN_IC_MAX_MA * 1000,
              "наскрізний струм дільника у межах Ic max керуючого NPN");
        check(CHARGE_NPN_IB_UA <= CHARGE_GPIO_UA_MAX,
              "струм із піна ESP32 у безпечних межах і на новому номіналі");
    }
#else
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
#endif

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

        // А ось це і є плата з нинішньою обв'язкою. Причина недобору залежить
        // від типу ключа, число — ні: відсічка міряє СТРУМ, а не те, чому його
        // немає. Тому беремо характерне для кожного випадку.
#if CHARGE_SW_IS_MOS
        //  Польовий: керуючий NPN при 20 кОм у базі не притягує затвор,
        //  ключ ледве прочинений. Точне число тут не обчислити (NPN у
        //  лінійному режимі), тож беремо явно голодну сотню міліампер.
        const int starvedMa = 100;
#else
        //  Біполярний: стеля насичення на нинішній обв'язці рахується точно.
        const int starvedMa = CHARGE_ASBUILT_IC_MAX_MA;
#endif
        n = 0;
        int polls = 0;
        while (!chargeNoDriveTrip(CHARGE_DUTY_MAX, starvedMa, 1000, &n)) {
            if (++polls > 100) { bad("відсічка так і не спрацювала"); break; }
        }
        printf("   плата як є (%d мА при уставці 1000): зупинка на %d-му опитуванні "
               "(поріг %d, ~%lu с)\n", starvedMa, polls + 1,
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

    printf("\n16) відсічка «ПАКЕТ ВІД'ЄДНАНО»: показання вперлось у стелю АЦП\n");
    {
        // Скарга: «при досягненні максимального заряду — напруга 9.9 В,
        // аварійна зупинка, перенапруга». Спершу показуємо, ЗВІДКИ 9.9 В.
        printf("   стеля точного виміру  CHARGE_VSENSE_SAT_MV  = %d мВ\n",
               (int)CHARGE_VSENSE_SAT_MV);
        printf("   відлік 4095 дає       CHARGE_VSENSE_RAIL_MV = %d мВ  <-- «9.9 В» зі скарги\n",
               (int)CHARGE_VSENSE_RAIL_MV);
        printf("   аварійна межа         ціль+запас            = %d мВ\n",
               (int)(CHARGE_TARGET_MV + CHARGE_HARD_MAX_HEADROOM_MV));

        // Те саме число, що бачив користувач. Воно мусить виводитись із
        // конфігурації, а не бути збігом: зміните подільник чи шкалу АЦП —
        // рядок покаже нове число, а не збереже старе.
        check(CHARGE_VSENSE_RAIL_MV == CHARGE_ADC_FULL_MV * 3,
              "«9.9 В» — це відлік 4095 через подільник 20к/10к, а не напруга");

        // Порядок трьох порогів. Та сама умова стоїть #error-ом у settings.h;
        // тут вона ще й видима в звіті.
        check(CHARGE_VSENSE_SAT_MV > CHARGE_TARGET_MV + CHARGE_HARD_MAX_HEADROOM_MV,
              "поріг насичення ВИЩЕ аварійної межі — справжня перенапруга лишається перенапругою");
        check(CHARGE_VSENSE_SAT_MV < CHARGE_VSENSE_RAIL_MV,
              "поріг насичення НИЖЧЕ найбільшого можливого показання — відсічка досяжна");

        // ⚑ СКВІЗНА ПЕРЕВІРКА, а не лише арифметика на константах: женемо
        //  справжній chargePackMv() через справжній ланцюжок перерахунку і
        //  дивимось, що він видає при відліку АЦП у стелі. Саме це число й
        //  бачив користувач на екрані.
        g_adcVsenseRaw = 4095;
        uint16_t railed = chargePackMv();
        printf("   chargePackMv() при відліку 4095 -> %u мВ\n", railed);
        check(railed == (uint16_t)CHARGE_VSENSE_RAIL_MV,
              "залиплий АЦП дає рівно CHARGE_VSENSE_RAIL_MV — те саме «9.9 В»");
        check(chargeSenseSaturated(railed),
              "і саме це показання відсічка розпізнає як «пакет від'єднано»");

        // Для контрасту — повний пакет на тому ж ланцюжку: 8.25 В на клемі
        // дають вузол 2750 мВ, тобто відлік ~3412. Відсічка мовчить.
        g_adcVsenseRaw = CHARGE_TARGET_MV * CHARGE_VSENSE_R_BOT /
                         (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT) *
                         CHARGE_ADC_MAX_RAW / CHARGE_ADC_FULL_MV;
        uint16_t full = chargePackMv();
        printf("   chargePackMv() на повному пакеті -> %u мВ (відлік %d)\n",
               full, g_adcVsenseRaw);
        check(!chargeSenseSaturated(full), "повний пакет відсічку не будить");
        g_adcVsenseRaw = 0;

        // Класифікатор.
        check(chargeSenseSaturated(CHARGE_VSENSE_RAIL_MV),
              "показання зі скарги — це насичення, а не напруга пакета");
        check(chargeSenseSaturated(CHARGE_VSENSE_SAT_MV), "рівно на порозі — уже насичення");
        check(!chargeSenseSaturated(CHARGE_VSENSE_SAT_MV - 1), "на мілівольт нижче — ще вимір");
        check(!chargeSenseSaturated(CHARGE_TARGET_MV), "повний пакет (8.25 В) — не насичення");
        check(!chargeSenseSaturated(CHARGE_TARGET_MV + CHARGE_HARD_MAX_HEADROOM_MV),
              "аварійна межа — це перенапруга, а НЕ «пакет від'єднано»");

        // Витримка — як у двох сусідніх відсічок.
        uint8_t n = 0; bool ok = false; uint16_t sat = 0;
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, false, 0, &n, &ok, &sat);
        check(!chargeSatTripped(n), "одного відліку в стелі замало");
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, false, 0, &n, &ok, &sat);
        check(chargeSatTripped(n), "двох поспіль — досить");
        check(n == CHARGE_NOPACK_POLLS, "спрацьовує рівно на CHARGE_NOPACK_POLLS");

        n = 0; ok = false; sat = 0;
        for (int i = 0; i < 50; i++) {
            chargeSatWitness(8100, false, 0, &n, &ok, &sat);
            if (chargeSatTripped(n)) bad("штатний заряд зупинено як «пакет від'єднано»");
        }
        check(n == 0, "штатні 8.1 В відсічку не чіпають");

        n = 0; ok = false; sat = 0;
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, false, 0, &n, &ok, &sat);
        chargeSatWitness(8100, false, 0, &n, &ok, &sat);
        check(n == 0, "одне нормальне показання скидає лічильник");

        // І окремо — те, через що скарга виглядала саме так: у chargeTask()
        // насичення перевіряється РАНІШЕ за перенапругу. Якби порядок був
        // зворотний, повідомлення знову стало б «перенапруга».
        check(chargeSenseSaturated(CHARGE_VSENSE_RAIL_MV) &&
              CHARGE_VSENSE_RAIL_MV >= CHARGE_TARGET_MV + CHARGE_HARD_MAX_HEADROOM_MV,
              "залипле показання підходить під ОБИДВІ умови — порядок перевірок вирішує діагноз");
    }

    printf("\n17) сторож (wdt.h): за IDLE-задачами він стежити НЕ мусить\n");
    {
        // Причина скарги «під час заряду або розряду пристрій періодично
        // перезавантажується»: сторож був налаштований стежити за бездіяльними
        // задачами обох ядер, а IDLE1 у цій прошивці не отримувала процесор
        // ніколи — цикл Arduino не блокувався. Через 10 с (заряд) чи 30 с
        // (розряд) прошивка падала в паніку. Маска мусить лишатись нульовою.
        check(wdtWatchIdleMask() == 0,
              "маска IDLE на час заряду/розряду — нульова");
        check(WDT_WATCH_IDLE_MASK == 0u,
              "константа маски теж нульова (її читає wdtGuard)");
        // Найпростіша перевірка «від протилежного»: саме те значення, що
        // стояло раніше, — 0b11 — мусить відрізнятись від нинішнього.
        check(wdtWatchIdleMask() != 0x3u,
              "стара маска 0b11 (обидва ядра) більше не використовується");
        check(CHARGE_WDT_SEC > 0 && DISCHARGE_WDT_SEC > CHARGE_WDT_SEC,
              "пороги сторожа лишились: розряд опитується рідше, тож і поріг більший");
    }

    printf("\n18) пакет ЗАКРИВАЄТЬСЯ САМ на своїй межі — це завершення, а не аварія\n");
    {
        // Скарга власника: «по досягненні 8.2 В акумулятор сам відключається
        // від зарядки, а наш пристрій цього не розуміє й намагається заряджати;
        // при відсутності навантаження напруга піднімається до 9.90 В, після
        // чого спрацьовує захист по перенапрузі».
        //
        // Обидві половини поведінки були неправильні. 9.90 В — знову підпис
        // відліку 4095, а не напруга. А «пакет розімкнувся» на 8.2 В — це
        // штатне завершення заряду, і називати його аварією не можна.
        const uint16_t tgt = CHARGE_TARGET_MV;

        printf("   ціль %u мВ, допуск «повного» %d мВ, аварійна межа %u мВ\n",
               tgt, (int)CHARGE_PACKFULL_TOL_MV,
               (unsigned)(tgt + CHARGE_HARD_MAX_HEADROOM_MV));

        // Головний випадок зі скарги: клема в стелі, монітор живий і каже 8.2 В.
        check(chargeSatVerdict(true, 8200, tgt) == SATV_FULL,
              "пакет закрився сам на 8.2 В — ЗАВЕРШЕНО, а не аварія");
        // Монітор мовчить — пакета фізично немає (як і було).
        check(chargeSatVerdict(false, 0, tgt) == SATV_GONE,
              "монітор мовчить — пакета в колі немає");
        check(chargeSatVerdict(false, 8200, tgt) == SATV_GONE,
              "мовчання монітора важливіше за будь-яке старе його показання");
        // Монітор живий, але напруга далеко не цільова — несправність пакета.
        check(chargeSatVerdict(true, 7000, tgt) == SATV_OPEN,
              "розімкнувся на 7.0 В — це НЕ повний пакет, а несправність");
        // Монітор живий і бачить справжню перенапругу — уперше зсередини.
        check(chargeSatVerdict(true, tgt + CHARGE_HARD_MAX_HEADROOM_MV, tgt) == SATV_OVER,
              "рівно на аварійній межі за монітором — справжня перенапруга");
        check(chargeSatVerdict(true, 9000, tgt) == SATV_OVER,
              "9.0 В зсередини пакета — перенапруга, а не «повний»");

        // Межі допуску — рівно там, де написано, без зсуву на одиницю.
        check(chargeSatVerdict(true, tgt - CHARGE_PACKFULL_TOL_MV, tgt) == SATV_FULL,
              "рівно на нижньому краї допуску — ще «повний»");
        check(chargeSatVerdict(true, tgt - CHARGE_PACKFULL_TOL_MV - 1, tgt) == SATV_OPEN,
              "на мілівольт нижче — уже «розімкнувся не за напругою»");
        check(chargeSatVerdict(true, tgt + CHARGE_HARD_MAX_HEADROOM_MV - 1, tgt) == SATV_FULL,
              "на мілівольт нижче аварійної межі — ще «повний»");

        // ⚑ ГОЛОВНА ВЛАСТИВІСТЬ НАКОПИЧУВАЧА: одна невдала транзакція 1-Wire
        //  посеред епізоду не сміє перетворити «повний» на «пакета немає».
        //  Шина на цій платі читається нестабільно, і якби рішення бралось за
        //  ОСТАННІМ проходом, власник отримував би аварію через раз.
        uint8_t n = 0; bool ok = false; uint16_t sat = 0;
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, true,  8200, &n, &ok, &sat);  // монітор відповів
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, false,    0, &n, &ok, &sat);  // а тут змовчав
        check(chargeSatTripped(n), "витримка набралась за два проходи");
        check(ok && sat == 8200, "свідчення монітора з ПЕРШОГО проходу збереглось");
        check(chargeSatVerdict(ok, sat, tgt) == SATV_FULL,
              "збій 1-Wire посеред епізоду не перетворює «повний» на «пакета немає»");

        // І дзеркально: якщо монітор мовчав УВЕСЬ епізод — це справді «немає».
        n = 0; ok = false; sat = 0;
        for (int i = 0; i < 5; i++)
            chargeSatWitness(CHARGE_VSENSE_RAIL_MV, false, 0, &n, &ok, &sat);
        check(chargeSatVerdict(ok, sat, tgt) == SATV_GONE,
              "монітор мовчав увесь епізод — пакета немає");

        // Вихід зі стелі стирає слід: наступний епізод починається з чистого.
        chargeSatWitness(CHARGE_VSENSE_RAIL_MV, true, 8200, &n, &ok, &sat);
        chargeSatWitness(8100, false, 0, &n, &ok, &sat);
        check(n == 0 && !ok && sat == 0,
              "клема вийшла зі стелі — лічильник і свідчення обнулено");

        // «Завершено» — множина з двох, і саме тому вона питається однією
        // функцією, а не порівнянням у кожному місці окремо.
        check(chargeReasonIsDone(CHGR_TARGET),   "ціль за нашим виміром — завершено");
        check(chargeReasonIsDone(CHGR_PACKFULL), "пакет закрився сам на своїй межі — теж завершено");
        check(!chargeReasonIsDone(CHGR_PACKOPEN), "розімкнувся не за напругою — НЕ завершено");
        check(!chargeReasonIsDone(CHGR_NOPACK),  "пакета немає — НЕ завершено");
        check(!chargeReasonIsDone(CHGR_HARD_MAX), "перенапруга — НЕ завершено");
        check(!chargeReasonIsDone(CHGR_USER),    "зупинка користувачем — не «завершено» саме собою");

        // Тексти для двох нових результатів мусять існувати: порожній рядок на
        // картці заряду — це рівно те, що власник побачить замість діагнозу.
        check(chargeReasonText(CHGR_PACKFULL)[0] != '\0', "у CHGR_PACKFULL є свій текст");
        check(chargeReasonText(CHGR_PACKOPEN)[0] != '\0', "у CHGR_PACKOPEN є свій текст");

        // Вікно частого читання монітора мусить накривати весь допуск: інакше
        // пакет закриється сам раніше, ніж ми почнемо читати монітор щосекунди.
        check(CHARGE_CHIP_WATCH_MV >= CHARGE_PACKFULL_TOL_MV,
              "монітор читається щосекунди вже там, де пакет має право закритись");
        // І сам допуск не сміє дотягтись до аварійної межі — інакше «повним»
        // оголошувався б будь-який пакет, що розімкнувся з будь-якої причини.
        check(CHARGE_PACKFULL_TOL_MV < CHARGE_TARGET_MV - DISCHARGE_TARGET_MV,
              "допуск «повного» вужчий за проміжок між ціллю заряду й ціллю розряду");
    }

    printf("\n19) ручне регулювання струму заряду\n");
    {
        // Скарга-побажання власника: «додати функцію ручного регулювання
        // струму заряду». Ручна уставка накладається поверх профілю, але не
        // скасовує дозаряду — саме це тут і перевіряється.
        printf("   межі: %d..%d мА, дозаряд %d мА\n",
               (int)CHARGE_MANUAL_MA_MIN, (int)CHARGE_MANUAL_MA_MAX, (int)CHARGE_MA_TAPER);

        check(chargeApplyManual(700, 0, false) == 700, "0 — це автомат, профіль не чіпаємо");
        check(chargeApplyManual(700, 0, true)  == 700, "…і в дозаряді теж автомат");
        check(chargeApplyManual(700, 300, false) == 300, "поза дозарядом діє ручне значення");

        // ⚑ ГОЛОВНЕ: у дозаряді ручне значення не має права ПІДНЯТИ струм.
        check(chargeApplyManual(CHARGE_MA_TAPER, 800, true) == CHARGE_MA_TAPER,
              "у дозаряді ручні 800 мА НЕ піднімають струм вище дозарядного");
        check(chargeApplyManual(CHARGE_MA_TAPER, 60, true) == 60,
              "…а зменшити струм у дозаряді можна завжди");
        check(chargeApplyManual(CHARGE_MA_TAPER, CHARGE_MA_TAPER, true) == CHARGE_MA_TAPER,
              "рівно дозарядне значення проходить як є");

        // Затиск у межі — з обох боків, і рівно на межах.
        check(chargeManualClamp(1) == CHARGE_MANUAL_MA_MIN, "нижче мінімуму піднімається до нього");
        check(chargeManualClamp(60000) == CHARGE_MANUAL_MA_MAX, "вище стелі опускається до неї");
        check(chargeManualClamp(CHARGE_MANUAL_MA_MIN) == CHARGE_MANUAL_MA_MIN, "рівно мінімум — як є");
        check(chargeManualClamp(CHARGE_MANUAL_MA_MAX) == CHARGE_MANUAL_MA_MAX, "рівно стеля — як є");
        check(chargeApplyManual(700, 60000, false) == CHARGE_MANUAL_MA_MAX,
              "затиск діє й через накладання, а не лише через clamp");

        // Стеля ручного режиму не сміє перевищувати стелю профілю: увесь
        // тепловий розрахунок зроблено для CHARGE_MA_80.
        check(CHARGE_MANUAL_MA_MAX <= CHARGE_MA_80,
              "ручна стеля не вище за стелю профілю (тепловий бюджет)");

        // Уставка живе ОКРЕМО від стану сеансу: зупинка заряду її не скидає.
        chargeSetManualMa(450);
        check(chargeManualMa() == 450, "уставка запам'ятовується");
        g_chg = ChargeState{};                       // саме це робить chargeStop/Start
        check(chargeManualMa() == 450, "і переживає обнулення стану сеансу");
        chargeSetManualMa(0);
        check(chargeManualMa() == 0, "повернення в автомат");

        // І наскрізна перевірка разом із гістерезисом: ручні 800 мА тримаються
        // до порога дозаряду, а за ним автоматично падають до дозарядних.
        bool tp = false;
        int lo = 0, hi = 0;
        for (int pct = 80; pct <= 100; pct++) {
            uint16_t a = chargeSetpointMaForPctH(pct, 100, &tp);
            uint16_t m = chargeApplyManual(a, 800, tp);
            if (pct < CHARGE_TAPER_PCT) lo = m; else hi = m;
        }
        printf("   ручні 800 мА: до порога %d мА, у дозаряді %d мА\n", lo, hi);
        check(lo == 800, "до порога дозаряду тримаються ручні 800 мА");
        check(hi == CHARGE_MA_TAPER, "у дозаряді струм сам падає до дозарядного");
    }

    // ═══════════════ ПРИМУСОВЕ ПРОБУДЖЕННЯ ПАКЕТА ═══════════════════════════
    //  Скарга-побажання власника: «додай функцію примусової безпечної зарядки
    //  для випадків, коли акумулятор не читається після заміни елементів і
    //  потрібен примусовий старт заряду для запуску контролера».
    //
    //  Режим працює БЕЗ контролю температури — монітор мовчить, у цьому вся
    //  суть, — тож усе, що стоїть між ним і зіпсованим пакетом, це числа й
    //  чотири чисті функції нижче. Кожну перевіряємо і «за», і «від
    //  протилежного»: зламана межа мусить ЛАМАТИ тест, інакше вона не межа.
    printf("\n20) пробудження: СТЕЛЯ ШПАРУВАТОСТІ — вона ж стеля НАПРУГИ\n");
    {
        // Головна властивість режиму: напругу на клемах задає не вимір (його
        // під час розгону просто немає — коло розімкнене), а розрахована
        // стеля шпаруватості. Перевіряємо, що вона справді дає CHARGE_WAKE_MV.
        for (int supply = 12500; supply <= 16000; supply += 500) {
            uint16_t cap = chargeWakeDutyCapFor((uint16_t)supply);
            long mv = (long)cap * supply / CHARGE_DUTY_FULL;
            printf("   живлення %d мВ -> стеля %u/%u -> на клемі ~%ld мВ\n",
                   supply, cap, (unsigned)CHARGE_DUTY_FULL, mv);
            // Похибка округлення шпаруватості — один відлік, тобто supply/2047.
            check(mv <= (long)CHARGE_WAKE_MV,
                  "розрахункова напруга не перевищує CHARGE_WAKE_MV");
            check(mv >= (long)CHARGE_WAKE_MV - supply / CHARGE_DUTY_FULL - 1,
                  "і не занижена більше, ніж на один відлік ШІМ");
            check(cap <= CHARGE_DUTY_MAX, "стеля сеансу не перескакує заводську");
        }
        // ⚑ ВІД ПРОТИЛЕЖНОГО: якби стелі не було, струмовий контур загнав би
        //  шпаруватість у заводську межу — а це напруга, від якої 2S-пакет
        //  гине. Показуємо число, щоб різниця не лишалась абстракцією.
        long bad = (long)CHARGE_DUTY_MAX * CHARGE_SUPPLY_MV / CHARGE_DUTY_FULL;
        printf("   без стелі сеансу регулятор дійшов би до %u/%u = %ld мВ на клемі\n",
               (unsigned)CHARGE_DUTY_MAX, (unsigned)CHARGE_DUTY_FULL, bad);
        check(bad > (long)CHARGE_WAKE_MV + 2000,
              "заводська стеля справді небезпечна для 2S — стеля сеансу не декоративна");
    }

    printf("\n20а) стеля діє в chargeSetDuty(), а не лише в регуляторі\n");
    {
        // Те саме, що перевірка 11 для заводської стелі: затиск мусить діяти
        // на БУДЬ-ЯКОМУ шляху, включно з прямим викликом повз регулятор.
        chargeResetDutyCap();
        chargeSetDutyCap(100);
        check(chargeDutyCap() == 100, "стеля сеансу встановилась");
        g_lastDuty = 0xFFFF;
        chargeSetDuty(2000);                       // прямий виклик, повз регулятор
        printf("   просили 2000, у ключ пішло %lu (стеля %u)\n",
               (unsigned long)g_lastDuty, chargeDutyCap());
        check(g_lastDuty == 100, "прямий виклик теж затиснуто стелею сеансу");

        // І навпаки: зіпсована (завищена) стеля сеансу не сміє перескочити
        // заводську — другий рядок оборони лишається на місці.
        chargeSetDutyCap(60000);
        check(chargeDutyCap() == CHARGE_DUTY_MAX,
              "завищена стеля сеансу зрізається до заводської");
        g_lastDuty = 0xFFFF;
        chargeSetDuty(60000);
        check(g_lastDuty == CHARGE_DUTY_MAX, "…і в ключ іде саме заводська межа");

        // Стеля мусить повертатись сама: інакше один сеанс пробудження тихо
        // покалічив би всі наступні заряди.
        chargeSetDutyCap(100);
        g_chg = ChargeState{};
        g_chg.state = CHG_RUN;
        chargeStop(CHGR_USER);
        check(chargeDutyCap() == CHARGE_DUTY_MAX,
              "зупинка повертає заводську стелю — наступний заряд не покалічено");
        chargeResetDutyCap();
    }

    printf("\n21) регулятор пробудження: НАПРУГА вгору, СТРУМ як обмеження\n");
    {
        uint16_t cap = chargeWakeDutyCapFor(CHARGE_SUPPLY_MV);

        // Розгін від нуля: без струму (коло розімкнене) шпаруватість росте до
        // стелі й НЕ вище. Заразом рахуємо, скільки на це піде проходів.
        uint16_t d = 0; int steps = 0;
        while (d < cap && steps < 10000) { d = chargeWakeNextDuty(d, 0, cap); steps++; }
        printf("   розгін від 0 до стелі %u: %d проходів по %lu мс = %.1f с\n",
               cap, steps, (unsigned long)CHARGE_WAKE_POLL_MS,
               steps * (double)CHARGE_WAKE_POLL_MS / 1000.0);
        check(d == cap, "без струму шпаруватість доходить рівно до стелі");
        check(chargeWakeNextDuty(d, 0, cap) == cap, "…і далі не рушає ні на відлік");
        check(steps * (double)CHARGE_WAKE_POLL_MS / 1000.0 < CHARGE_WAKE_MAX_S / 4.0,
              "розгін укладається в чверть відведеного часу — режим устигає попрацювати");

        // Струм понад стелю — шпаруватість униз, і байдуже, що до стелі
        // напруги ще далеко.
        uint16_t down = chargeWakeNextDuty(cap, CHARGE_WAKE_MA * 4, cap);
        printf("   при %d мА (стеля %d) шпаруватість %u -> %u\n",
               CHARGE_WAKE_MA * 4, (int)CHARGE_WAKE_MA, cap, down);
        check(down < cap, "надлишок струму опускає шпаруватість");

        // Збіжність: модель «пакет прокинувся» — коло замкнулось, струм
        // лінійний за шпаруватістю. Контур мусить прийти до стелі струму.
        // I = (D×Uживл − Uпак) / R_кола, Uпак = 7.4 В (щойно замінені банки).
        uint16_t dd = cap; int it = 0, last = 0;
        for (; it < 400; it++) {
            long vout = (long)dd * CHARGE_SUPPLY_MV / CHARGE_DUTY_FULL;
            long ma   = (vout - 7400) * 1000 / CHARGE_LOOP_MOHM;
            if (ma < 0) ma = 0;
            last = (int)ma;
            uint16_t nd = chargeWakeNextDuty(dd, ma, cap);
            if (nd == dd) break;
            dd = nd;
        }
        printf("   пакет ожив на 7.40 В: за %d проходів струм %d мА (стеля %d), duty %u\n",
               it, last, (int)CHARGE_WAKE_MA, dd);
        check(it < 400, "контур зійшовся, а не забуксував");
        check(last <= CHARGE_WAKE_MA + CHARGE_DEADBAND_MA,
              "усталений струм не вище стелі плюс мертва зона");
        check(last <= CHARGE_WAKE_MA_ABORT,
              "і не дотягується до аварійної відсічки — вона стереже відмову, а не режим");

        // ⚑ ВІД ПРОТИЛЕЖНОГО: якби стеля шпаруватості не передавалась, той
        //  самий розгін пішов би до заводської межі.
        uint16_t d2 = 0; int s2 = 0;
        while (d2 < CHARGE_DUTY_MAX && s2 < 10000) { d2 = chargeWakeNextDuty(d2, 0, CHARGE_DUTY_MAX); s2++; }
        check(d2 == CHARGE_DUTY_MAX && d2 > cap,
              "з чужою стелею той самий контур іде вище — тобто стелю справді слухають");

        // Від'ємний струм не випрямляється: мінус — це шум або розряд, і в
        // обох випадках струму заряду немає (та сама властивість, що й у
        // штатного регулятора, перевірка 6).
        check(chargeWakeNextDuty(100, -500, cap) > 100,
              "від'ємний струм не читається як «стелю досягнуто»");
    }

    printf("\n22) вироки пробудження й порядок між ними\n");
    {
        ChargeWakeIn in;
        auto fresh = [&]() {
            in.chipOk = false; in.avgMa = 0; in.mvFresh = false; in.mv = 0;
            in.elapsedS = 0; in.mah = 0;
            in.goal = WAKE_GOAL_CHIP; in.tempFresh = false; in.tempC10 = 250;
        };
        fresh();
        check(chargeWakeVerdict(in) == WAKEV_GO, "нічого не сталося — працюємо далі");

        fresh(); in.chipOk = true;
        check(chargeWakeVerdict(in) == WAKEV_WOKE, "монітор відповів — мета досягнута");

        // ⚑ І ВІДПОВІДЬ МОНІТОРА СИЛЬНІША ЗА СТРИБОК СТРУМУ. Саме цей стрибок
        //  і означає пробудження: контролер замкнув ключ, і на місці
        //  розімкненого кола з'явилось навантаження. Назвати це аварією
        //  означало б ганяти користувача по колу, щоразу відмовляючи в тому,
        //  що вже сталося.
        fresh(); in.chipOk = true; in.avgMa = CHARGE_WAKE_MA_ABORT * 2;
        check(chargeWakeVerdict(in) == WAKEV_WOKE,
              "успіх не маскується стрибком струму в мить пробудження");

        fresh(); in.avgMa = CHARGE_WAKE_MA_ABORT + 1;
        check(chargeWakeVerdict(in) == WAKEV_OVERI, "струм понад аварійну межу — стоп");
        fresh(); in.avgMa = CHARGE_WAKE_MA_ABORT;
        check(chargeWakeVerdict(in) == WAKEV_GO, "рівно на межі ще працюємо");

        // Напруга: тільки СВІЖИЙ вимір на закритому ключі.
        fresh(); in.mv = CHARGE_WAKE_MV + CHARGE_HARD_MAX_HEADROOM_MV; in.mvFresh = false;
        check(chargeWakeVerdict(in) == WAKEV_GO, "несвіжий вимір напруги до порівняння не пускаємо");
        fresh(); in.mv = CHARGE_WAKE_MV + CHARGE_HARD_MAX_HEADROOM_MV; in.mvFresh = true;
        check(chargeWakeVerdict(in) == WAKEV_OVERV, "свіжий вимір вище межі — стоп");

        // ⚑ НАСИЧЕНИЙ ВІДЛІК — НЕ ПЕРЕНАПРУГА, А ВИХІДНИЙ СТАН РЕЖИМУ.
        //  Це найважливіша різниця зі штатним зарядом, і без неї режим був би
        //  непрацездатний: коло розімкнене з самого початку, і перша ж проба
        //  обірвала б пробудження.
        fresh(); in.mv = CHARGE_VSENSE_SAT_MV; in.mvFresh = true;
        check(chargeWakeVerdict(in) == WAKEV_GO,
              "клема в стелі — це «пакет ще не в колі», а не перенапруга");
        fresh(); in.mv = (uint16_t)(CHARGE_VSENSE_SAT_MV + 500); in.mvFresh = true;
        check(chargeWakeVerdict(in) == WAKEV_GO, "і глибше в стелі теж");

        fresh(); in.mah = CHARGE_WAKE_MAH_MAX;
        check(chargeWakeVerdict(in) == WAKEV_MAH, "стеля відданої ємності спрацьовує");
        fresh(); in.mah = CHARGE_WAKE_MAH_MAX - 1;
        check(chargeWakeVerdict(in) == WAKEV_GO, "нижче стелі — працюємо");

        fresh(); in.elapsedS = CHARGE_WAKE_MAX_S;
        check(chargeWakeVerdict(in) == WAKEV_TIMEOUT, "стеля часу спрацьовує");

        // Переклад вироків у причини зупинки — і те, що успіх справді
        // вважається завершенням, а не аварією.
        check(chargeWakeReason(WAKEV_WOKE,  WAKE_GOAL_CHIP) == CHGR_WOKE,      "WOKE -> CHGR_WOKE");
        check(chargeWakeReason(WAKEV_OVERI, WAKE_GOAL_CHIP) == CHGR_WAKE_OVERI,"OVERI -> CHGR_WAKE_OVERI");
        check(chargeWakeReason(WAKEV_OVERV, WAKE_GOAL_CHIP) == CHGR_HARD_MAX,  "OVERV -> CHGR_HARD_MAX");
        check(chargeWakeReason(WAKEV_MAH,   WAKE_GOAL_CHIP) == CHGR_WAKE_MAH,  "MAH -> CHGR_WAKE_MAH");
        check(chargeWakeReason(WAKEV_TIMEOUT, WAKE_GOAL_CHIP) == CHGR_WAKE_FAIL,
              "TIMEOUT у меті CHIP -> CHGR_WAKE_FAIL");
        // ⚑ ТОЙ САМИЙ ВИРОК, ІНША МЕТА — ІНША ПРИЧИНА. У меті RAIL монітор
        //  говорив увесь сеанс, тож «монітор так і мовчить» відправило б
        //  людину шукати геть не там.
        check(chargeWakeReason(WAKEV_TIMEOUT, WAKE_GOAL_RAIL) == CHGR_WAKE_NORAIL,
              "TIMEOUT у меті RAIL -> CHGR_WAKE_NORAIL, а не «монітор мовчить»");
        check(chargeWakeReason(WAKEV_RAIL, WAKE_GOAL_RAIL) == CHGR_WAKE_RAIL,
              "RAIL -> CHGR_WAKE_RAIL");
        check(chargeWakeReason(WAKEV_HOT, WAKE_GOAL_RAIL) == CHGR_TEMP,
              "перегрів перекладається у звичайний CHGR_TEMP, а не в окрему причину");
        check(chargeReasonIsDone(CHGR_WOKE),  "пробудження — це ЗАВЕРШЕННЯ, а не аварія");
        check(chargeReasonIsDone(CHGR_WAKE_RAIL),
              "…і оживлена клема теж завершення: інакше успіх показали б як збій");
        check(!chargeReasonIsDone(CHGR_WAKE_FAIL) && !chargeReasonIsDone(CHGR_WAKE_MAH) &&
              !chargeReasonIsDone(CHGR_WAKE_OVERI) && !chargeReasonIsDone(CHGR_WAKE_NORAIL),
              "решта вироків пробудження — не завершення");
        // Кожна нова причина мусить мати текст: порожній рядок у клієнті
        // виглядав би як «зупинилось саме по собі».
        check(chargeReasonText(CHGR_WOKE)[0] && chargeReasonText(CHGR_WAKE_FAIL)[0] &&
              chargeReasonText(CHGR_WAKE_MAH)[0] && chargeReasonText(CHGR_WAKE_OVERI)[0] &&
              chargeReasonText(CHGR_WAKE_RAIL)[0] && chargeReasonText(CHGR_WAKE_NORAIL)[0],
              "усі нові причини мають текст для інтерфейсу");
        // Два тексти причин не сміють збігатися: інакше різні події читались
        // би як одна.
        check(strcmp(chargeReasonText(CHGR_WAKE_FAIL),
                     chargeReasonText(CHGR_WAKE_NORAIL)) != 0,
              "«монітор мовчить» і «клема мертва» — різні тексти");
    }

    printf("\n22а) друга мета пробудження: пакет ЧИТАЄТЬСЯ, а клема мертва\n");
    {
        // Робочий випадок власника: захист пакета розімкнув силовий ключ.
        // 1-Wire сидить ДО нього, тож монітор говорить, а на клемі нуль. Обидві
        // двері були зачинені: пробудження казало «пакет читається», штатний
        // заряд — «подільник показує нуль».
        check(chargeWakeRefuse(true, true, false, PSU_OK, true, 0, 0) == WAKENO_OK,
              "ЧИТАЄТЬСЯ + мертва клема -> пробудження ДОЗВОЛЕНО (це і був глухий кут)");
        check(chargeWakeGoal(true)  == WAKE_GOAL_RAIL, "мета: чекаємо напругу на клемі");
        check(chargeWakeGoal(false) == WAKE_GOAL_CHIP, "мовчить -> чекаємо його відповідь");

        // ── САМ ПОРІГ «КЛЕМА ЖИВА» ─────────────────────────────────────────
        //  Він тепер керує ДВОМА рішеннями (пускати штатний заряд і пускати
        //  пробудження), тож його число мусить бути прибите, а не «яке
        //  вийшло»: з'їде вгору — глибоко розряджений, але цілком підключений
        //  пакет почнуть вважати від'єднаним; з'їде вниз — шум подільника
        //  зарахують за живу клему.
        check(!chargeRailAlive(0), "рівно нуль — клема мертва");
        check(!chargeRailAlive(500),
              "залишкові сотні мВ крізь подільник — теж мертва, а не «майже жива»");
        check(chargeRailAlive(BATTERY_EMPTY_MV),
              "порожній, але ПІД'ЄДНАНИЙ пакет — клема жива");
        check(chargeRailAlive(BATTERY_EMPTY_MV / 2) &&
              !chargeRailAlive((uint16_t)(BATTERY_EMPTY_MV / 2 - 1)),
              "поріг стоїть рівно на половині порожнього пакета");
        // ⚑ І ОСЬ ВІД ЧОГО ЗАЛЕЖИТЬ ЦІЛИЙ ШМАТОК МІРКУВАНЬ у chargeWakeRefuse():
        //  WAKENO_FULL не має доданка про мету саме тому, що «мертва клема» і
        //  «вище напруги пробудження» несумісні. Поки це співвідношення
        //  тримається, доданок не потрібен; щойно воно поїде — FULL почне
        //  відхиляти мету RAIL, і зловити це має саме тут.
        // Питаємо саме ФУНКЦІЮ, а не два макроси: інакше перевірка стерегла
        // б співвідношення констант, а поїхати може й сам поріг.
        check(chargeRailAlive((uint16_t)CHARGE_WAKE_MV),
              "усе, що дотягує до напруги пробудження, уже вважається живою клемою — "
              "тому WAKENO_FULL не потребує доданка про мету");

        ChargeWakeIn in;
        auto rail = [&]() {
            in.chipOk = true; in.avgMa = 0; in.mvFresh = false; in.mv = 0;
            in.elapsedS = 0; in.mah = 0;
            in.goal = WAKE_GOAL_RAIL; in.tempFresh = true; in.tempC10 = 250;
        };
        // ⚑ НАЙВАЖЛИВІШЕ В УСЬОМУ РОЗДІЛІ. У цій меті монітор відповідає з
        //  першої ж проби. Якби успіхом лишалось «chipOk», режим оголошував би
        //  перемогу, не зробивши НІЧОГО, — і виглядало б це як робота.
        rail();
        check(chargeWakeVerdict(in) == WAKEV_GO,
              "відповідь монітора тут НЕ успіх: він говорив і до старту");
        rail(); in.mvFresh = true; in.mv = (uint16_t)(BATTERY_EMPTY_MV / 2);
        check(chargeWakeVerdict(in) == WAKEV_RAIL, "напруга на клемі з'явилась — успіх");
        rail(); in.mvFresh = true; in.mv = (uint16_t)(BATTERY_EMPTY_MV / 2 - 1);
        check(chargeWakeVerdict(in) == WAKEV_GO, "на відлік нижче — ще чекаємо");
        rail(); in.mvFresh = false; in.mv = 8000;
        check(chargeWakeVerdict(in) == WAKEV_GO,
              "несвіжий вимір не зараховується: під напругою на клемі стоїмо МИ");
        // Стеля АЦП — не «клема ожила», а розімкнене коло: зарахувати її
        // означало б оголосити успіх саме тоді, коли пакета в колі немає.
        rail(); in.mvFresh = true; in.mv = (uint16_t)CHARGE_VSENSE_SAT_MV;
        check(chargeWakeVerdict(in) == WAKEV_GO, "насичений відлік — не успіх");

        // Температура: у цій меті вона Є, і не скористатись нею було б
        // марнуванням єдиної переваги над сліпою метою CHIP.
        rail(); in.tempC10 = (int16_t)(CHARGE_MAX_TEMP_C * 10);
        check(chargeWakeVerdict(in) == WAKEV_HOT, "гарячий пакет -> стоп");
        rail(); in.tempC10 = (int16_t)(CHARGE_MAX_TEMP_C * 10 - 1);
        check(chargeWakeVerdict(in) == WAKEV_GO, "на десяту нижче — працюємо");
        rail(); in.tempFresh = false; in.tempC10 = (int16_t)(CHARGE_MAX_TEMP_C * 10 + 100);
        check(chargeWakeVerdict(in) == WAKEV_GO, "несвіжа температура до вироку не пускається");
        // Мороз НЕ зупиняє: 8 мА·год — це сигнал контролеру, а не заряд, і
        // відмова на холоді лишила б людину без єдиного способу оживити пакет.
        rail(); in.tempC10 = -200;
        check(chargeWakeVerdict(in) == WAKEV_GO, "холод не забороняє: це сигнал, а не заряд");
        // А в сліпій меті температури немає взагалі — і вона там не діє.
        ChargeWakeIn blind;
        blind.chipOk = false; blind.avgMa = 0; blind.mvFresh = false; blind.mv = 0;
        blind.elapsedS = 0; blind.mah = 0; blind.goal = WAKE_GOAL_CHIP;
        blind.tempFresh = true; blind.tempC10 = (int16_t)(CHARGE_MAX_TEMP_C * 10 + 100);
        check(chargeWakeVerdict(blind) == WAKEV_GO,
              "у меті CHIP температура не діє — там її фізично немає");

        // Межі енергії й часу — спільні для обох мет: саме на них тримається
        // дозвіл працювати без датчика, і мета їх не послаблює.
        rail(); in.mah = CHARGE_WAKE_MAH_MAX;
        check(chargeWakeVerdict(in) == WAKEV_MAH, "стеля ємності діє й тут");
        rail(); in.elapsedS = CHARGE_WAKE_MAX_S;
        check(chargeWakeVerdict(in) == WAKEV_TIMEOUT, "стеля часу діє й тут");
        rail(); in.avgMa = CHARGE_WAKE_MA_ABORT + 1;
        check(chargeWakeVerdict(in) == WAKEV_OVERI, "аварійний струм діє й тут");

        // Рядок стану мусить бути ЧЕСНИЙ: у меті RAIL монітор не мовчить.
        check(strcmp(chargeWakeGoalText(WAKE_GOAL_RAIL, CHGR_NONE),
                     chargeWakeGoalText(WAKE_GOAL_CHIP, CHGR_NONE)) != 0,
              "підпис проб залежить від мети, а не один на два випадки");
        check(strstr(chargeWakeGoalText(WAKE_GOAL_RAIL, CHGR_NONE), "мовч") == nullptr &&
              strstr(chargeWakeGoalShort(WAKE_GOAL_RAIL, CHGR_NONE), "мовч") == nullptr,
              "…і в меті RAIL він НЕ каже «монітор мовчить» — той говорить");
        check(strcmp(chargeWakeGoalText(WAKE_GOAL_RAIL, CHGR_WAKE_RAIL),
                     chargeWakeGoalText(WAKE_GOAL_RAIL, CHGR_NONE)) != 0,
              "успіх у рядку стану видно");

        // ⚑ РІВНО ТОЙ ВИРАЗ, ЩО СТОЇТЬ У ДРАЙВЕРАХ ЕКРАНА. Самі драйвери на
        //  хості не збираються (u8g2, Adafruit GFX), тож поле стану, якого
        //  вони торкаються, більше ніде не перевіряється: додати виклик і
        //  забути додати поле — помилка, яку впіймає лише прошивка на столі.
        ChargeState cs = {};
        cs.wakeGoal = WAKE_GOAL_RAIL; cs.reason = CHGR_WAKE_RAIL;
        char row[64];
        snprintf(row, sizeof(row), "проб %u  %s", cs.wakeProbes,
                 chargeWakeGoalShort(cs.wakeGoal, cs.reason));
        check(row[0] && chargeReasonIsDone(cs.reason),
              "вираз рядка проб із драйверів збирається й дає завершення");
    }

    printf("\n23) коли пробудження НЕ дають — і чому саме так\n");
    {
        const uint16_t okMv = CHARGE_WAKE_MV - 1000;   // пакет під напругою, але нижче
        // Порядок причин: спершу «нічим керувати», потім «зайнято», потім суть.
        check(chargeWakeRefuse(false, true, false, PSU_OK, false, okMv, 0) == WAKENO_NA,
              "заряд не налаштовано");
        check(chargeWakeRefuse(true, false, false, PSU_OK, false, okMv, 0) == WAKENO_PWM,
              "ШІМ не прикріпився");
        check(chargeWakeRefuse(true, true, true, PSU_OK, false, okMv, 0) == WAKENO_BUSY,
              "заряд/розряд уже йде");
        check(chargeWakeRefuse(true, true, false, PSU_ABSENT, false, okMv, 0) == WAKENO_PSU,
              "живлення поза допуском");
        check(chargeWakeRefuse(true, true, false, PSU_LOW, false, okMv, 0) == WAKENO_PSU,
              "занижене живлення — теж відмова");

        // ⚑ ГОЛОВНА ВІДМОВА РЕЖИМУ, І УМОВА В НІЙ ПОДВІЙНА. Пробудження працює
        //  без контролю температури; єдине, що не дає перетворити його на
        //  «заряд без датчика», — відмова запускатись там, де штатний заряд
        //  ДОСТУПНИЙ. А доступний він лише тоді, коли пакет і читається, і
        //  показує напругу на клемі: без напруги штатний заряд не стартує сам.
        check(chargeWakeRefuse(true, true, false, PSU_OK, true, okMv, 0) == WAKENO_READS,
              "ЧИТАЄТЬСЯ + жива клема -> відмова: штатний заряд можливий і безпечніший");
        check(chargeWakeRefuse(true, true, false, PSU_UNKNOWN, true, okMv, 0) == WAKENO_READS,
              "…і без контролю живлення теж: причина не в блоці");
        check(chargeWakeRefuse(true, true, false, PSU_OK, true,
                               (uint16_t)(BATTERY_EMPTY_MV / 2), 0) == WAKENO_READS,
              "рівно на порозі «клема жива» — уже штатний заряд");

        // Дві ознаки пробитого ключа, обидві зняті на ЗАКРИТОМУ ключі.
        check(chargeWakeRefuse(true, true, false, PSU_OK, false,
                               (uint16_t)CHARGE_VSENSE_SAT_MV, 0) == WAKENO_RAIL,
              "живлення на клемі при закритому ключі — ключ пробитий");
        check(chargeWakeRefuse(true, true, false, PSU_OK, false, okMv,
                               CHARGE_DEADBAND_MA + 1) == WAKENO_LEAK,
              "струм при закритому ключі — ключ пробитий");
        check(chargeWakeRefuse(true, true, false, PSU_OK, false, okMv,
                               CHARGE_DEADBAND_MA) == WAKENO_OK,
              "струм у мертвій зоні — це шум АЦП, а не витік");
        // ⚑ І В НОВІЙ МЕТІ ТЕЖ. Раніше «пакет читається» відхиляло режим
        //  першим, тобто ознаки пробитого ключа при живому моніторі ніхто не
        //  дивився взагалі. Тепер дивиться — і мусить бачити.
        check(chargeWakeRefuse(true, true, false, PSU_OK, true, 0,
                               CHARGE_DEADBAND_MA + 1) == WAKENO_LEAK,
              "струм при закритому ключі ловиться й тоді, коли монітор говорить");

        // Будити нема чого, якщо напруга вже є.
        check(chargeWakeRefuse(true, true, false, PSU_OK, false,
                               (uint16_t)CHARGE_WAKE_MV, 0) == WAKENO_FULL,
              "на клемі вже напруга пробудження — показувати «сигнал зарядника» нема кому");
        check(chargeWakeRefuse(true, true, false, PSU_OK, false,
                               (uint16_t)(CHARGE_WAKE_MV - 1), 0) == WAKENO_OK,
              "на відлік нижче — уже можна");
        // ⚑ WAKENO_FULL — питання ЛИШЕ до мети CHIP. У меті RAIL клема за
        //  визначенням мертва, тож ця перевірка там ніколи б і не спрацювала,
        //  а спрацювавши — відхилила б єдиний випадок, заради якого мету й
        //  завели. Пара нижче тримає обидві половини твердження.
        check(chargeWakeRefuse(true, true, false, PSU_OK, true, 0, 0) == WAKENO_OK,
              "мертва клема при ЖИВОМУ моніторі — дозволено (мета RAIL)");
        check(chargeWakeRefuse(true, true, false, PSU_OK, false,
                               (uint16_t)CHARGE_WAKE_MV, 0) == WAKENO_FULL,
              "…а при мовчазному та сама перевірка на місці");

        // Робочий випадок власника: банки замінені (пакет тримає ~7.4 В через
        // тіло ключа), монітор мовчить, живлення справне -> дозволено.
        check(chargeWakeRefuse(true, true, false, PSU_OK, false, 7400, 0) == WAKENO_OK,
              "щойно замінені банки й мовчазний монітор — режим дозволено");
        // І та сама клема при мовчазному моніторі й РОЗІМКНЕНОМУ колі: після
        // паяння пакет може взагалі нічого не показувати.
        check(chargeWakeRefuse(true, true, false, PSU_OK, false, 0, 0) == WAKENO_OK,
              "мертва клема при мовчазному моніторі — теж дозволено");

        // Кожна відмова мусить пояснювати себе, інакше користувач бачить
        // порожнє вікно замість причини.
        for (int r = WAKENO_OK + 1; r <= WAKENO_FULL; r++)
            check(chargeWakeRefuseText((uint8_t)r)[0] != 0,
                  "кожна відмова має текст");
        check(chargeWakeRefuseText(WAKENO_OK)[0] == 0, "у «можна» тексту немає");
    }

    printf("\n24) МЕЖІ РЕЖИМУ — те, на чому тримається дозвіл працювати без датчика\n");
    {
        // Уся безпека режиму зводиться до відданої енергії. Рахуємо її явно,
        // а не покладаємось на те, що «числа виглядають малими».
        long nominalMah = (long)CHARGE_WAKE_MA * CHARGE_WAKE_MAX_S / 3600;
        double pctOfPack = 100.0 * CHARGE_WAKE_MAH_MAX / BATTERY_RATED_MAH;
        printf("   штатна віддача %ld мА·год, стеля %d мА·год = %.2f %% пакета %d мА·год\n",
               nominalMah, (int)CHARGE_WAKE_MAH_MAX, pctOfPack, (int)BATTERY_RATED_MAH);
        check(pctOfPack <= 1.0, "стеля енергії не більша за 1 % ємності пакета");
        check(CHARGE_WAKE_MAH_MAX > nominalMah,
              "ємнісна стеля вища за штатну віддачу — режим не обривається сам на собі");
        check(CHARGE_WAKE_MA <= CHARGE_MA_START,
              "струм пробудження не вищий за найобережнішу уставку штатного профілю");
        check(CHARGE_WAKE_MV <= CHARGE_TARGET_MV,
              "напруга пробудження не вища за ціль штатного заряду");
        check(CHARGE_WAKE_MA_ABORT > CHARGE_WAKE_MA &&
              CHARGE_WAKE_MA_ABORT < CHARGE_PEAK_MA_MAX,
              "аварійна відсічка струму лежить МІЖ стелею режиму й запобіжником заліза");
        check(CHARGE_WAKE_POLL_MS < CHARGE_POLL_MS,
              "крок режиму частіший за штатне опитування — мить замикання ключа коротка");
        // ── НАЙГІРША МИТЬ РЕЖИМУ, ПОРАХОВАНА, А НЕ ОПИСАНА СЛОВАМИ ────────
        //  Контролер замикає ключ, коли на клемах уже стоїть напруга
        //  пробудження, а банки розряджені вщент. Дросель виводить струм на
        //  усталений рівень за L/R ≈ 0.13 мс, тобто в межах одного кроку.
        //
        //  ⚑ І ЦЕ ЧИСЛО ЗАЛЕЖИТЬ ВІД ТИПУ КЛЮЧА — саме так тут і знайшлась
        //  різниця, коли тест прогнали в обох конфігураціях. У польового
        //  RDS(on) додає до опору контуру 180 мОм і СТРИМУЄ струм; у
        //  біполярного цього доданка немає, опір менший, а стрибок — більший.
        long worstMa = ((long)CHARGE_WAKE_MV - BATTERY_EMPTY_MV) * 1000 / CHARGE_LOOP_MOHM;
        double worstMah = worstMa * (double)CHARGE_WAKE_POLL_MS / 3600000.0;
        // Той самий стрибок з боку ШУНТА: він найслабша ланка в колі, і саме
        // його теплова стійкість задає тривалість кроку.
        double worstW = (worstMa / 1000.0) * (worstMa / 1000.0) * (CHARGE_SHUNT_MOHM / 1000.0);
        double worstJ = worstW * CHARGE_WAKE_POLL_MS / 1000.0;
        printf("   ключ %s: найгірший стрибок %ld мА протягом %lu мс = %.4f мА·год;\n"
               "   на шунті %.1f Вт (паспорт %.1f Вт) -> %.3f Дж за крок\n",
               CHARGE_SW_NAME, worstMa, (unsigned long)CHARGE_WAKE_POLL_MS, worstMah,
               worstW, CHARGE_SHUNT_MW / 1000.0, worstJ);
        check(worstMah < 1.0, "навіть найгірший стрибок віддає менше за один мА·год");

        // ⚑ ЧЕСНО ПРО ПІКОВУ ВІДСІЧКУ. З біполярним ключем стрибок ВИЩИЙ за
        //  CHARGE_PEAK_MA_MAX — і це не поломка, а спрацювання запобіжника:
        //  наступний крок закриє ключ. Тобто найгірший сценарій режиму — не
        //  «щось згорить», а «пробудження зупиниться з піковою аварією рівно
        //  тієї миті, коли пакет ожив». Стверджувати «відсічка не спрацює»
        //  було б неправдою для половини конфігурацій, тому перевіряємо те, що
        //  справді має триматись: відсічка існує й стрибок не йде в рази вище
        //  за неї (інакше вона нічого не встигала б).
        if (worstMa >= CHARGE_PEAK_MA_MAX)
            printf("   ⚑ стрибок вище відсічки %d мА — пробудження зупиниться за піком\n",
                   (int)CHARGE_PEAK_MA_MAX);
        check(worstMa < (long)CHARGE_PEAK_MA_MAX * 3 / 2,
              "стрибок лишається в межах, які пікова відсічка справді ловить");

        // Тепловий імпульс у шунт. Для плівкових і металооксидних резисторів
        // одиничний імпульс до ~5× паспортної потужності тривалістю до 100 мс
        // лежить у межах специфікації; тримаємось усередині обох умов.
        check(worstW <= 5.0 * CHARGE_SHUNT_MW / 1000.0,
              "потужність імпульсу не вище п'ятикратної паспортної для шунта");
        check(CHARGE_WAKE_POLL_MS <= 100,
              "…і триває він не довше за 100 мс, для яких це правило й писане");

        // ⚑ ВІД ПРОТИЛЕЖНОГО: на штатному кроці заряду (1 с) той самий стрибок
        //  тривав би в 40 разів довше — тобто окремий, коротший крок для
        //  пробудження не косметика.
        double slowJ = worstW * CHARGE_POLL_MS / 1000.0;
        printf("   на штатному кроці %lu мс було б %.3f Дж — у %.0f разів більше\n",
               (unsigned long)CHARGE_POLL_MS, slowJ, slowJ / (worstJ > 0 ? worstJ : 1));
        check(slowJ / (worstJ > 0 ? worstJ : 1) >= 10,
              "крок пробудження скорочує експозицію щонайменше вдесятеро");
    }

    printf("\n24а) інтеграл ємності НЕ ГУБИТЬСЯ на короткому кроці\n");
    {
        // ⚑ Пастка, знайдена при розборі готового коду: доданок дорівнює
        //  мА×мс/3600, і при кроці 25 мс він менший за одиницю на будь-якому
        //  струмі до 144 мА. Просте цілочисельне ділення з'їдало б його
        //  повністю — і стеля відданої ємності, тобто ДРУГА з двох меж, на яких
        //  тримається дозвіл працювати без датчика, ніколи б не спрацювала.
        printf("   доданок за один крок при %d мА: %d мА·мс / 3600 = %d (ціла частина)\n",
               (int)CHARGE_WAKE_MA, (int)(CHARGE_WAKE_MA * CHARGE_WAKE_POLL_MS),
               (int)(CHARGE_WAKE_MA * CHARGE_WAKE_POLL_MS / 3600));

        // Ганяємо повний сеанс на стелі струму й звіряємо з точним значенням.
        uint32_t mah = 0, rem = 0;
        long steps = (long)CHARGE_WAKE_MAX_S * 1000 / CHARGE_WAKE_POLL_MS;
        for (long i = 0; i < steps; i++)
            chargeAccumMah(&mah, &rem, CHARGE_WAKE_MA, (uint32_t)CHARGE_WAKE_POLL_MS);
        double exact = (double)CHARGE_WAKE_MA * CHARGE_WAKE_MAX_S / 3600.0;
        printf("   %ld кроків по %lu мс на %d мА -> %.3f мА·год (точно %.3f)\n",
               steps, (unsigned long)CHARGE_WAKE_POLL_MS, (int)CHARGE_WAKE_MA,
               mah / 1000.0, exact);
        check(mah / 1000.0 > exact - 0.01, "накопичене не нижче точного значення");
        check(mah / 1000.0 <= exact, "…і не вище — залишок не додає зайвого");

        // ⚑ ВІД ПРОТИЛЕЖНОГО: та сама сума БЕЗ залишку. Саме так і було
        //  написано спочатку, і саме тому лічильник стояв би на нулі.
        uint32_t naive = 0;
        for (long i = 0; i < steps; i++)
            naive += (uint32_t)CHARGE_WAKE_MA * CHARGE_WAKE_POLL_MS / 3600UL;
        printf("   без залишку вийшло б %.3f мА·год замість %.3f\n",
               naive / 1000.0, exact);
        check(naive < mah, "просте ділення справді втрачає ємність");

        // І найважливіше: на малому струмі просте ділення дає РІВНО НУЛЬ, а з
        // залишком — правильну суму.
        uint32_t m2 = 0, r2 = 0, n2 = 0;
        const int lowMa = CHARGE_DEADBAND_MA;      // 30 мА — нижче за поріг втрати
        for (long i = 0; i < steps; i++) {
            chargeAccumMah(&m2, &r2, lowMa, (uint32_t)CHARGE_WAKE_POLL_MS);
            n2 += (uint32_t)lowMa * CHARGE_WAKE_POLL_MS / 3600UL;
        }
        printf("   на %d мА: із залишком %.3f мА·год, без нього %.3f\n",
               lowMa, m2 / 1000.0, n2 / 1000.0);
        check(n2 == 0, "без залишку малий струм не накопичується ВЗАГАЛІ");
        check(m2 > 0,  "…а із залишком — накопичується");

        // Від'ємний струм рахується за модулем: розряд — теж віддана енергія.
        uint32_t m3 = 0, r3 = 0;
        chargeAccumMah(&m3, &r3, -3600, 1000);
        check(m3 == 1000, "від'ємний струм рахується за модулем");
    }

    printf("\n24б) поріг зависання пробудження — свій, а не запозичений\n");
    {
        // Штатні 5 с — це п'ять кроків секундного заряду, але двісті кроків
        // пробудження. Дозволити ключу стояти без нагляду двісті кроків
        // означало б скасувати дрібність кроку, заради якої вона й обрана.
        printf("   штатний поріг %lu мс = %lu кроків пробудження; власний %lu мс = %lu кроків\n",
               (unsigned long)CHARGE_STALL_MS,
               (unsigned long)(CHARGE_STALL_MS / CHARGE_WAKE_POLL_MS),
               (unsigned long)CHARGE_WAKE_STALL_MS,
               (unsigned long)(CHARGE_WAKE_STALL_MS / CHARGE_WAKE_POLL_MS));
        check(CHARGE_WAKE_STALL_MS < CHARGE_STALL_MS,
              "пробудження помічає зависання РАНІШЕ за штатний заряд");
        check(CHARGE_WAKE_STALL_MS > CHARGE_WAKE_POLL_MS * 4,
              "…але не настільки рано, щоб його зривала власна проба монітора");
        // Скільки енергії встигне піти в найгіршому випадку за час до
        // спрацювання сторожа — це і є ціна порога.
        long worstMa = ((long)CHARGE_WAKE_MV - BATTERY_EMPTY_MV) * 1000 / CHARGE_LOOP_MOHM;
        double stallMah  = worstMa * (double)CHARGE_WAKE_STALL_MS / 3600000.0;
        double borrowMah = worstMa * (double)CHARGE_STALL_MS      / 3600000.0;
        printf("   при зависанні віддасться %.3f мА·год замість %.3f із запозиченим порогом\n",
               stallMah, borrowMah);
        check(stallMah < CHARGE_WAKE_MAH_MAX,
              "навіть зависання не виводить сеанс за власну стелю ємності");
    }

    printf("\n25) режим видно ззовні, і жоден із трьох питальників не зайвий\n");
    {
        g_chg = ChargeState{};
        check(!chargeWaking() && !chargeWakeShown(), "у спокої пробудження немає");

        g_chg.mode = CHG_MODE_WAKE; g_chg.state = CHG_RUN;
        check(chargeRunning(), "для всіх, хто стежить за ключем, це «заряд іде»");
        check(chargeWaking() && chargeWakeShown(), "а для показу — саме пробудження");

        // Сеанс скінчився, але підсумок ще на екрані.
        g_chg.state = CHG_DONE; g_chg.reason = CHGR_WOKE;
        check(!chargeWaking(), "після зупинки нічого вже не йде");
        check(chargeWakeShown(), "…але панель мусить лишатись панеллю ПРОБУДЖЕННЯ");

        // Підсумок прибрали — панель повертається до заряду, інакше запустити
        // звичайний заряд не було б звідки.
        chargeDismiss();
        check(!chargeWakeShown(), "у спокої панель повертається до заряду");

        // І штатний заряд ніколи не показується як пробудження.
        g_chg = ChargeState{}; g_chg.mode = CHG_MODE_CHARGE; g_chg.state = CHG_RUN;
        check(!chargeWaking() && !chargeWakeShown(), "штатний заряд — не пробудження");
        g_chg = ChargeState{};
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
