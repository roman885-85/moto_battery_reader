#include <cstdio>
#include <cstdint>
#include <cstdlib>

#define CHARGE_MA_START   200
#define CHARGE_MA_10      500
#define CHARGE_MA_50      1000
#define CHARGE_MA_80      1500
#define CHARGE_MA_TAPER   100
#define CHARGE_DEADBAND_MA   30
#define CHARGE_OUT_STEP_MV   20

#define CHARGE_CAL_CTRL_MV  {1760, 1770, 1780, 1800, 1860, 1880, 1930}
#define CHARGE_CAL_OUT_MV   {   0, 2500, 4100, 5200, 7200, 7600, 8600}
#define CHARGE_CAL_POINTS   7
#define CHARGE_CAL_OUT_MAX  8600

// --- скопійовано дослівно з charge.h (щоб перевірити ізольовано) ----------
uint16_t chargeSetpointMaForPct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == 0)   return CHARGE_MA_START;
    if (pct < 10)   return (uint16_t)(CHARGE_MA_START +
                        (long)(CHARGE_MA_10 - CHARGE_MA_START) * pct / 10);
    if (pct == 10)  return CHARGE_MA_10;
    if (pct < 50)   return (uint16_t)(CHARGE_MA_10 +
                        (long)(CHARGE_MA_50 - CHARGE_MA_10) * (pct - 10) / 40);
    if (pct < 80)   return (uint16_t)(CHARGE_MA_50 +
                        (long)(CHARGE_MA_80 - CHARGE_MA_50) * (pct - 50) / 30);
    if (pct < 95)   return CHARGE_MA_80;
    return CHARGE_MA_TAPER;
}
uint16_t chargeCtrlMvForOutputMv(uint16_t outMv) {
    static const uint16_t calCtrl[] = CHARGE_CAL_CTRL_MV;
    static const uint16_t calOut[]  = CHARGE_CAL_OUT_MV;
    const int n = CHARGE_CAL_POINTS;
    if (outMv <= calOut[0])     return calCtrl[0];
    if (outMv >= calOut[n - 1]) return calCtrl[n - 1];
    for (int i = 1; i < n; i++) {
        if (outMv <= calOut[i]) {
            uint16_t o0 = calOut[i - 1],  o1 = calOut[i];
            uint16_t c0 = calCtrl[i - 1], c1 = calCtrl[i];
            return (uint16_t)(c0 + (uint32_t)(c1 - c0) * (outMv - o0) / (o1 - o0));
        }
    }
    return calCtrl[n - 1];
}
uint16_t chargeNextOutMv(uint16_t outMv, int16_t measuredMa, uint16_t setMa) {
    int16_t meas = measuredMa < 0 ? -measuredMa : measuredMa;
    int32_t err  = (int32_t)setMa - meas;
    if (err > CHARGE_DEADBAND_MA) {
        if (outMv + CHARGE_OUT_STEP_MV <= CHARGE_CAL_OUT_MAX) outMv = (uint16_t)(outMv + CHARGE_OUT_STEP_MV);
        else outMv = CHARGE_CAL_OUT_MAX;
    } else if (err < -CHARGE_DEADBAND_MA) {
        outMv = (outMv > CHARGE_OUT_STEP_MV) ? (uint16_t)(outMv - CHARGE_OUT_STEP_MV) : 0;
    }
    if (outMv > CHARGE_CAL_OUT_MAX) outMv = CHARGE_CAL_OUT_MAX;
    return outMv;
}
// ---------------------------------------------------------------------------

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

int main() {
    printf("1) контрольні точки профілю струму\n");
    struct { int pct; int want; } pts[] = {
        {0,200},{5,350},{10,500},{30,750},{50,1000},{65,1250},{80,1500},
        {85,1500},{94,1500},{95,100},{97,100},{100,100}
    };
    for (auto &p : pts) {
        int got = chargeSetpointMaForPct(p.pct);
        printf("   %3d%% -> %d мА (очікую %d)\n", p.pct, got, p.want);
        if (got != p.want) bad("не збігається з очікуваним профілем");
    }

    printf("\n2) монотонність профілю струму (крім ступінчастого падіння на 95%%)\n");
    int prev = chargeSetpointMaForPct(0);
    for (int pct = 1; pct < 95; pct++) {
        int cur = chargeSetpointMaForPct(pct);
        if (cur < prev) bad("струм впав там, де мав лише зростати/триматись (0..94%)");
        prev = cur;
    }
    if (chargeSetpointMaForPct(95) >= chargeSetpointMaForPct(94))
        bad("на 95% мав бути ступінчастий СПАД, а не зростання/плато");
    for (int pct = 95; pct <= 100; pct++)
        if (chargeSetpointMaForPct(pct) != CHARGE_MA_TAPER)
            bad("95..100% мають триматися рівно на CHARGE_MA_TAPER");

    printf("\n3) вихід профілю струму ніколи не виходить за межі [CHARGE_MA_TAPER..CHARGE_MA_80]\n");
    for (int pct = 0; pct <= 100; pct++) {
        int v = chargeSetpointMaForPct(pct);
        if (v < CHARGE_MA_TAPER || v > CHARGE_MA_80) bad("вихід поза розумними межами");
    }

    printf("\n4) інтерполяція калібрувальної таблиці — точний збіг у ВСІХ 7 точках\n");
    {
        static const uint16_t calCtrl[] = CHARGE_CAL_CTRL_MV;
        static const uint16_t calOut[]  = CHARGE_CAL_OUT_MV;
        for (int i = 0; i < CHARGE_CAL_POINTS; i++) {
            uint16_t got = chargeCtrlMvForOutputMv(calOut[i]);
            printf("   вихід %u мВ -> керування %u мВ (очікую %u)\n", calOut[i], got, calCtrl[i]);
            if (got != calCtrl[i]) bad("калібрувальна точка не відтворюється точно");
        }
    }

    printf("\n5) інтерполяція монотонна й НЕ екстраполює за межі таблиці\n");
    {
        uint16_t prevC = chargeCtrlMvForOutputMv(0);
        for (uint32_t o = 0; o <= CHARGE_CAL_OUT_MAX; o += 37) {   // непарний крок — щоб зачепити міжточкові
            uint16_t c = chargeCtrlMvForOutputMv((uint16_t)o);
            if (c < prevC) bad("керуюча напруга впала там, де вихід лише зростав — крива має бути монотонною");
            prevC = c;
        }
        uint16_t below = chargeCtrlMvForOutputMv(0);
        uint16_t above = chargeCtrlMvForOutputMv(60000);   // явно поза таблицею (переповнення теж перевіряє затиск)
        printf("   нижче таблиці (0 мВ) -> %u мВ, вище (>стелі) -> %u мВ\n", below, above);
        if (below != 1760) bad("нижня межа має дорівнювати першій калібрувальній точці (1760 мВ)");
        if (above != 1930) bad("верхня межа має дорівнювати останній калібрувальній точці (1930 мВ), без екстраполяції");
    }

    printf("\n6) регулятор вихідної напруги: soft-start з нуля, збіжність до уставки\n");
    {
        uint16_t outMv = 0;
        uint16_t setMa = 1000;
        int16_t measured = 0;   // проста модель "плант": струм ~ outMv (спрощено, не калібрувальна крива)
        int steps = 0;
        for (; steps < 2000; steps++) {
            measured = (int16_t)(outMv / 4);   // умовний коефіцієнт плант-моделі
            uint16_t next = chargeNextOutMv(outMv, measured, setMa);
            if (next == outMv) break;   // збіглись (у межах мертвої зони)
            outMv = next;
        }
        printf("   збіжність за %d кроків, outMv=%u мВ, струм~%d мА (ціль %u)\n", steps, outMv, measured, setMa);
        if (steps >= 2000) bad("регулятор не збігся за розумний час");
        if (outMv == 0) bad("вихідна напруга застрягла на нулі — заряд не піде взагалі");
        int16_t err = (int16_t)setMa - measured;
        if (err < 0) err = -err;
        if (err > CHARGE_DEADBAND_MA + CHARGE_OUT_STEP_MV / 4 + 5) bad("збіжність далеко за межами мертвої зони");
    }

    printf("\n7) регулятор ніколи не перевищує CHARGE_CAL_OUT_MAX, навіть якщо струму завжди мало\n");
    {
        uint16_t outMv = 0;
        for (int i = 0; i < 1000; i++) outMv = chargeNextOutMv(outMv, 0, 5000);  // недосяжна уставка
        printf("   outMv після 1000 кроків недосяжної уставки: %u мВ (стеля %d)\n", outMv, CHARGE_CAL_OUT_MAX);
        if (outMv > CHARGE_CAL_OUT_MAX) bad("вихідна напруга перевищила апаратну стелю!");
        if (outMv != CHARGE_CAL_OUT_MAX) bad("мала впертись рівно у стелю при постійно недосяжній уставці");
    }

    printf("\n8) регулятор опускає вихідну напругу до нуля, якщо струм завжди забагато\n");
    {
        uint16_t outMv = 5000;
        for (int i = 0; i < 1000; i++) outMv = chargeNextOutMv(outMv, 9000, 100);  // завжди сильно забагато
        printf("   outMv після 1000 кроків надлишкового струму: %u мВ\n", outMv);
        if (outMv != 0) bad("вихідна напруга мала впасти рівно до нуля при постійному надструмі");
    }

    printf("\n9) найкрутіша ділянка таблиці (1760->1780 мВ, 0->4100 мВ виходу) — крок керування\n"
           "   не має бути грубішим за роздільність ШІМ (перевіряємо лише, що інтерполяція\n"
           "   в цій зоні дає РІЗНІ значення на дрібних кроках виходу, а не «сходинку в один стрибок»)\n");
    {
        int distinct = 0;
        uint16_t last = 0xFFFF;
        for (uint16_t o = 0; o <= 4100; o += 100) {
            uint16_t c = chargeCtrlMvForOutputMv(o);
            if (c != last) distinct++;
            last = c;
        }
        printf("   %d різних значень керування на 42 кроках виходу по 100 мВ у крутій зоні\n", distinct);
        if (distinct < 10) bad("замало розрізнення в крутій зоні — регулювання там буде занадто грубим");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
