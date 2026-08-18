// ===========================================================================
//  ШКАЛА ЗАРЯДУ ЗА НАПРУГОЮ ДЛЯ 2S-ПАКЕТА
//
//  Побажання власника: «перерахуй правильність показань заряду у відсотковому
//  співвідношенні для 2s батареї, ігноруючи мої початкові граничні значення»,
//  і окремо — «значення максимуму й мінімуму виходячи із заводських значень
//  для 2s li-ion акумуляторів».
//
//  Було: пряма між 6350 і 8250 мВ. Стало: таблична крива напруги спокою на
//  банку (soc.h) з паспортними межами 3.00 і 4.20 В на банку.
//
//  Тут перевіряється і сама крива, і те, що від зміни шкали не розвалилось
//  усе інше, що на неї спирається: профіль струму заряду, поріг дозаряду,
//  межі розряду й перерахунок паливоміра.
// ===========================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#define DS2438_RSENSE_OHM 0.025f
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
#define PROGMEM
#define memcpy_P memcpy
#include "settings.h"
#include "impres_format.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

// Стара лінійна шкала — рівно та, що була в коді. Потрібна, щоб показати
// РІЗНИЦЮ, а не просто стверджувати, що стало краще.
static int oldLinear(int mv) {
    long p = ((long)mv - 6350) * 100 / (8250 - 6350);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)p;
}

int main() {
    printf("1) паспортні межі 2S li-ion\n");
    {
        printf("   0 %% = %.2f В/банку = %d мВ; 100 %% = %.2f В/банку = %d мВ\n",
               SOC_CELL_EMPTY_MV / 1000.0, SOC_EMPTY_MV,
               SOC_CELL_FULL_MV / 1000.0, SOC_FULL_MV);
        check(SOC_CELLS == 2, "пакет двобанковий");
        check(SOC_CELL_FULL_MV == 4200, "верх — 4.20 В на банку (паспорт літію)");
        check(SOC_CELL_EMPTY_MV == 3000, "низ — 3.00 В на банку (нижче працює захист)");
        check(SOC_EMPTY_MV == 6000 && SOC_FULL_MV == 8400, "на пакеті це 6.00 і 8.40 В");
        check(BATTERY_EMPTY_MV == SOC_EMPTY_MV && BATTERY_FULL_MV == SOC_FULL_MV,
              "шкала прошивки бере ті самі межі, а не власні числа");
        // Старі межі мусять зникнути: саме їх власник просив ігнорувати.
        check(BATTERY_EMPTY_MV != 6350 && BATTERY_FULL_MV != 8250,
              "початкові 6.35 / 8.25 В більше не використовуються");
    }

    printf("\n2) крива монотонна й замкнена на краях\n");
    {
        int bad1 = 0, bad2 = 0;
        for (int i = 1; i < SOC_POINTS; i++) {
            if (SOC_CURVE[i].cellMv <= SOC_CURVE[i - 1].cellMv) bad1++;
            if (SOC_CURVE[i].pct    <= SOC_CURVE[i - 1].pct)    bad2++;
        }
        check(bad1 == 0, "напруга у вузлах строго зростає");
        check(bad2 == 0, "відсоток у вузлах строго зростає");
        check(socPctFromMv(SOC_EMPTY_MV) == 0, "на нижньому краї рівно 0 %");
        check(socPctFromMv(SOC_FULL_MV) == 100, "на верхньому рівно 100 %");
        check(socPctFromMv(0) == 0 && socPctFromMv(-500) == 0, "нижче шкали — 0, без від'ємних");
        check(socPctFromMv(99999) == 100, "вище шкали — 100, без переповнення");
        // Монотонність САМОЇ функції, а не лише вузлів.
        int prev = -1, nonmono = 0;
        for (int mv = 5000; mv <= 9000; mv += 5) {
            int p = socPctFromMv(mv);
            if (p < prev) nonmono++;
            prev = p;
        }
        check(nonmono == 0, "функція не має жодного провалу на всьому діапазоні");
    }

    printf("\n3) вузли кривої відтворюються ТОЧНО\n");
    {
        int off = 0, worst = 0;
        for (int i = 0; i < SOC_POINTS; i++) {
            int mv = SOC_CURVE[i].cellMv * SOC_CELLS;
            int got = socPctFromMv(mv);
            int d = got - (int)SOC_CURVE[i].pct;
            if (d) { off++; if (abs(d) > abs(worst)) worst = d; }
        }
        printf("   вузлів %d, розбіжність найбільша %+d %%\n", SOC_POINTS, worst);
        check(off == 0, "у кожному вузлі відсоток збігається з таблицею");
    }

    printf("\n4) обернення: відсоток -> напруга -> відсоток\n");
    {
        int worst = 0;
        for (int pct = 0; pct <= 100; pct++) {
            int mv = socMvFromPct(pct);
            int back = socPctFromMv(mv);
            if (abs(back - pct) > abs(worst)) worst = back - pct;
        }
        printf("   найбільша похибка кругового перетворення: %+d %%\n", worst);
        check(abs(worst) <= 1, "круговий перехід сходиться з точністю до 1 %");
        // Обернена функція теж монотонна — інакше вибір цілі заряду стрибав би.
        int prev = -1, nonmono = 0;
        for (int p = 0; p <= 100; p++) { int mv = socMvFromPct(p); if (mv < prev) nonmono++; prev = mv; }
        check(nonmono == 0, "обернена функція монотонна");
        check(socMvFromPct(0) == SOC_EMPTY_MV, "0 % -> нижній край");
        check(socMvFromPct(100) == SOC_FULL_MV, "100 % -> верхній край");
    }

    printf("\n5) ЩО САМЕ ВИПРАВЛЕНО: крива проти прямої\n");
    {
        printf("   напруга   В/банку   було    стало   різниця\n");
        const int pts[] = { 6600, 6800, 7000, 7200, 7400, 7600, 7800, 8000, 8200 };
        int worstLow = 0;
        for (int i = 0; i < 9; i++) {
            int mv = pts[i], o = oldLinear(mv), n = socPctFromMv(mv);
            printf("   %5d мВ   %.2f     %3d %%   %3d %%    %+d\n",
                   mv, mv / 2000.0, o, n, n - o);
            if (mv <= 7200 && (o - n) > worstLow) worstLow = o - n;
        }
        // ⚑ Головне число всієї цієї роботи: наскільки пряма БРЕХАЛА на майже
        //  розрядженому пакеті. Саме там помилка найдорожча.
        printf("   на розрядженому пакеті (<= 7.20 В) пряма завищувала на %d %%\n", worstLow);
        check(worstLow >= 20, "пряма справді завищувала показ на розрядженому пакеті");
        check(socPctFromMv(7000) < 20, "7.00 В — це менше 20 %, а не 34 %, як казала пряма");
        check(oldLinear(7000) > 30, "…а пряма казала більше 30 % — ось і вся різниця");
    }

    printf("\n6) те, що спирається на шкалу, лишилось узгодженим\n");
    {
        // Ціль заряду й ціль розряду — у межах шкали і в правильному порядку.
        check(CHARGE_TARGET_MV <= BATTERY_FULL_MV, "ціль заряду в межах шкали");
        check(DISCHARGE_TARGET_MV > BATTERY_EMPTY_MV, "ціль розряду вище нуля шкали");
        printf("   ціль заряду %d мВ = %d %%, ціль розряду %d мВ = %d %%\n",
               (int)CHARGE_TARGET_MV, impresPercentFromMv(CHARGE_TARGET_MV),
               (int)DISCHARGE_TARGET_MV, impresPercentFromMv(DISCHARGE_TARGET_MV));
        check(impresPercentFromMv(CHARGE_TARGET_MV) == 90, "8.20 В = 90 % (4.10 В/банку)");
        check(impresPercentFromMv(DISCHARGE_TARGET_MV) == 20, "7.20 В = 20 % (3.60 В/банку)");
        // Верх розрядної лінійки — це верх шкали, а не окреме число.
        check(DISCHARGE_RAMP_HI_MV == BATTERY_FULL_MV,
              "верх розрядної лінійки збігається з верхом шкали");

        // ⚑ І НАЙВАЖЛИВІШЕ: ДОЗАРЯД НЕ ЗНИК.
        //  Ціль заряду 8.20 В — це 90 %. Якби targetPct лишався 100, поріг
        //  дозаряду (90 % від цілі) припав би рівно на кінець заряду, і
        //  дозарядна ділянка виродилась би в нуль. chargeStart() тому й
        //  перераховує targetPct із затиснутої напруги.
        int targetPct = impresPercentFromMv(CHARGE_TARGET_MV);
        int bpTaper   = CHARGE_TAPER_PCT * targetPct / 100;
        int taperMv   = impresMvFromPercent(bpTaper);
        printf("   ціль %d %%, поріг дозаряду %d %% = %d мВ -> ділянка дозаряду %d мВ\n",
               targetPct, bpTaper, taperMv, (int)CHARGE_TARGET_MV - taperMv);
        check(taperMv < CHARGE_TARGET_MV, "поріг дозаряду нижчий за ціль");
        check((int)CHARGE_TARGET_MV - taperMv >= 100,
              "ділянка дозаряду щонайменше 100 мВ — вона не виродилась");
    }

    printf("\n7) паливомір у мА·год лишається лінійним за ємністю\n");
    {
        // Відсоток тепер = справжня частка ємності, тож перерахунок у мА·год
        // мусить бути прямою. Перевіряємо на реальній ємності 2150 мА·год.
        const int rated = 2150;
        const float rs = 0.04565f;
        int bad2 = 0;
        for (int pct = 0; pct <= 100; pct += 10) {
            uint8_t ica = impresIcaFromPercentRs(pct, rated, rs);
            long mah = impresIcaToMahRs(ica, rated, rs);
            long want = (long)rated * pct / 100;
            if (labs(mah - want) > rated / 50) bad2++;   // допуск 2 % ємності
        }
        check(bad2 == 0, "відсоток -> ICA -> мА·год сходиться в межах 2 %");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
