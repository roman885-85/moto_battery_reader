#include <cstdio>
#include <cstdint>
#include <cstdlib>

#define CHARGE_MA_START   200
#define CHARGE_MA_10      500
#define CHARGE_MA_50      1000
#define CHARGE_MA_80      1500
#define CHARGE_MA_TAPER   100
#define CHARGE_DUTY_STEP_PCT 1
#define CHARGE_DEADBAND_MA   30
#define CHARGE_DUTY_MAX_PCT  75

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
uint8_t chargeNextDuty(uint8_t duty, int16_t measuredMa, uint16_t setMa) {
    int16_t meas = measuredMa < 0 ? -measuredMa : measuredMa;
    int32_t err  = (int32_t)setMa - meas;
    if (err > CHARGE_DEADBAND_MA) {
        if (duty < CHARGE_DUTY_MAX_PCT) duty = (uint8_t)(duty + CHARGE_DUTY_STEP_PCT);
    } else if (err < -CHARGE_DEADBAND_MA) {
        if (duty > 0) duty = (uint8_t)(duty - (duty < CHARGE_DUTY_STEP_PCT ? duty : CHARGE_DUTY_STEP_PCT));
    }
    if (duty > CHARGE_DUTY_MAX_PCT) duty = CHARGE_DUTY_MAX_PCT;
    return duty;
}
// ---------------------------------------------------------------------------

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

int main() {
    printf("1) контрольні точки профілю\n");
    struct { int pct; int want; } pts[] = {
        {0,200},{5,350},{10,500},{30,750},{50,1000},{65,1250},{80,1500},
        {85,1500},{94,1500},{95,100},{97,100},{100,100}
    };
    for (auto &p : pts) {
        int got = chargeSetpointMaForPct(p.pct);
        printf("   %3d%% -> %d мА (очікую %d)\n", p.pct, got, p.want);
        if (got != p.want) bad("не збігається з очікуваним профілем");
    }

    printf("\n2) монотонність (крім ступінчастого падіння на 95%%)\n");
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

    printf("\n3) вихід ніколи не виходить за межі [CHARGE_MA_START..CHARGE_MA_80] за модулем\n");
    for (int pct = 0; pct <= 100; pct++) {
        int v = chargeSetpointMaForPct(pct);
        if (v < CHARGE_MA_TAPER || v > CHARGE_MA_80) bad("вихід поза розумними межами");
    }

    printf("\n4) регулятор шпаруватості: soft-start з нуля, збіжність до уставки\n");
    {
        uint8_t duty = 0;
        uint16_t setMa = 1000;
        int16_t measured = 0;   // емулюємо: струм лінійно росте з духом ~ duty (спрощено)
        int steps = 0;
        for (; steps < 500; steps++) {
            // проста модель "плант": струм = duty * 20 (мА на % шпаруватості)
            measured = (int16_t)(duty * 20);
            uint8_t next = chargeNextDuty(duty, measured, setMa);
            if (next == duty) break;   // збіглись (у межах мертвої зони)
            duty = next;
        }
        printf("   збіжність за %d кроків, duty=%u%%, струм~%d мА (ціль %u)\n", steps, duty, measured, setMa);
        if (steps >= 500) bad("регулятор не збігся за розумний час");
        if (duty == 0) bad("шпаруватість застрягла на нулі — заряд не піде взагалі");
        int16_t err = (int16_t)setMa - measured;
        if (err < 0) err = -err;
        if (err > CHARGE_DEADBAND_MA + 20) bad("збіжність далеко за межами мертвої зони");
    }

    printf("\n5) регулятор ніколи не перевищує CHARGE_DUTY_MAX_PCT, навіть якщо струму завжди мало\n");
    {
        uint8_t duty = 0;
        for (int i = 0; i < 200; i++) duty = chargeNextDuty(duty, 0, 5000);  // недосяжна уставка
        printf("   duty після 200 кроків недосяжної уставки: %u%% (стеля %d)\n", duty, CHARGE_DUTY_MAX_PCT);
        if (duty > CHARGE_DUTY_MAX_PCT) bad("шпаруватість перевищила апаратну стелю!");
        if (duty != CHARGE_DUTY_MAX_PCT) bad("мала впертись рівно у стелю при постійно недосяжній уставці");
    }

    printf("\n6) регулятор опускає шпаруватість до нуля, якщо струм завжди забагато\n");
    {
        uint8_t duty = 50;
        for (int i = 0; i < 200; i++) duty = chargeNextDuty(duty, 9000, 100);  // завжди сильно забагато
        printf("   duty після 200 кроків надлишкового струму: %u%%\n", duty);
        if (duty != 0) bad("шпаруватість мала впасти рівно до нуля при постійному надструмі");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
