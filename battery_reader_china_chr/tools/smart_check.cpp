// ===========================================================================
//  РУЧНІ УСТАВКИ Й РОЗУМНИЙ ПРОФІЛЬ ЗАРЯДУ/РОЗРЯДУ ДЛЯ ЛІТІЙ-ІОННОГО 2S
//
//  Прохання власника: «на заряд і розряд зроби ручний режим вибору напруги й
//  струму, і створи розумний автоматичний режим для літій-іонної батареї 2S;
//  додати статус часу, що лишився».
//
//  ⚑ ЧОМУ ЦЕ ПЕРЕВІРЯЄТЬСЯ ТЕСТОМ, А НЕ НА ПАКЕТІ. Розумний профіль — це
//  рішення про СТРУМ У ЖИВІ БАНКИ, і помилка тут коштує пакета: завеликий
//  струм у глибоко розряджений або холодний літій дає металеве осадження на
//  аноді, і це незворотньо. Перевіряти таке «на живому» не можна.
//
//  Уся логіка навмисно винесена в чисті функції charge.h/discharge.h — саме
//  тому їх можна ганяти тут напряму, разом із МОДЕЛЛЮ ПАКЕТА (повний цикл
//  заряду за секундами), а не лише поодинокими значеннями.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>

// ── мінімальне оточення Arduino ────────────────────────────────────────────
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define INPUT 0
#define ADC_11db 3
static void pinMode(int, int) {}
static void digitalWrite(int, int) {}
static unsigned long g_ms = 1000;
static unsigned long millis() { return g_ms; }
static bool ledcAttachChannel(int, int, int, int) { return true; }
static void ledcWrite(int, uint32_t) {}
static int  analogRead(int) { return 0; }
static void analogSetPinAttenuation(int, int) {}
static class { public: void printf(const char *, ...) {} void println(const char *) {}
               void println() {} void print(const char *) {} } Serial;
#define LEDS_H
enum LedMode { LED_BOOT, LED_IDLE, LED_READ, LED_WRITE, LED_OK, LED_ERROR,
               LED_FAULT, LED_DISCHARGE, LED_CHARGE, LED_CHARGE_TAPER };
static void ledSet(LedMode) {}

#include "settings.h"
#include "discharge.h"
#include "charge.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

static ChargeSmartOut smart(uint16_t mv, uint16_t target, uint16_t rated,
                            int16_t t10 = 250, bool fresh = true) {
    ChargeSmartIn in;
    in.packMv = mv; in.targetMv = target; in.ratedMah = rated;
    in.tempC10 = t10; in.tempFresh = fresh;
    return chargeSmart(in);
}

int main() {
    printf("=== РУЧНІ УСТАВКИ Й РОЗУМНИЙ ПРОФІЛЬ ===\n");
    const uint16_t TGT = CHARGE_TARGET_MV;

    printf("\n1) струм рахується від ЄМНОСТІ ПАКЕТА, але не вище заліза\n");
    {
        printf("   пакет 2500: CC %u, перед %u, кінець %u мА\n",
               chargeSmartCMa(2500, CHARGE_SMART_CC_PCT),
               chargeSmartCMa(2500, CHARGE_SMART_PRE_PCT),
               chargeSmartEndMa(2500));
        printf("   пакет 1300: CC %u, перед %u, кінець %u мА\n",
               chargeSmartCMa(1300, CHARGE_SMART_CC_PCT),
               chargeSmartCMa(1300, CHARGE_SMART_PRE_PCT),
               chargeSmartEndMa(1300));
        // 0.5C від 2500 — це 1250 мА, а тепловий бюджет ключа дозволяє 1000.
        check(chargeSmartCMa(2500, CHARGE_SMART_CC_PCT) == CHARGE_MA_80,
              "великий пакет затиснуто апаратною стелею, а не заряджено 0.5C наосліп");
        check(chargeSmartCMa(1300, CHARGE_SMART_CC_PCT) == 650,
              "малому пакету дістається саме 0.5C, а не та сама заводська 1000 мА");
        check(chargeSmartCMa(1300, CHARGE_SMART_PRE_PCT) == 130, "передзаряд — 0.1C");
        check(chargeSmartEndMa(1300) == 65, "поріг завершення — 0.05C");
        // Дуже маленький пакет: 0.05C нижче підлоги, і підлога його підхоплює.
        check(chargeSmartEndMa(500) == CHARGE_SMART_MA_MIN,
              "для крихітного пакета поріг лягає на підлогу уставки, а не в нуль");
        check(chargeSmartCMa(50, CHARGE_SMART_CC_PCT) == CHARGE_SMART_MA_MIN,
              "нижче підлоги уставка не опускається взагалі");
    }

    printf("\n2) фази йдуть у правильному порядку за напругою\n");
    {
        check(smart(5000, TGT, 2500).phase == CHG_PH_PRE,  "глибокий розряд -> ПЕРЕДЗАРЯД");
        check(smart(5000, TGT, 2500).setMa == chargeSmartCMa(2500, CHARGE_SMART_PRE_PCT),
              "…і саме струмом передзаряду");
        check(smart(CHARGE_SMART_PRE_MV - 1, TGT, 2500).phase == CHG_PH_PRE,
              "рівно під порогом — ще передзаряд");
        check(smart(CHARGE_SMART_PRE_MV, TGT, 2500).phase == CHG_PH_CC,
              "на порозі — вже CC");
        check(smart(7500, TGT, 2500).phase == CHG_PH_CC,   "середина -> CC");
        check(smart(7500, TGT, 2500).setMa == CHARGE_MA_80, "…повним дозволеним струмом");
        check(smart(TGT - CHARGE_SMART_CV_BAND_MV, TGT, 2500).phase == CHG_PH_CC,
              "на межі зони CV ще CC");
        check(smart(TGT - CHARGE_SMART_CV_BAND_MV + 1, TGT, 2500).phase == CHG_PH_CV,
              "на крок вище — уже CV");
        check(smart(TGT, TGT, 2500).phase == CHG_PH_CV, "на цілі — CV");
    }

    printf("\n3) у фазі CV уставка спадає, і спадає ДО ПОРОГА ЗАВЕРШЕННЯ\n");
    {
        uint16_t endMa = chargeSmartEndMa(2500);
        uint16_t prev = 0xFFFF;
        bool mono = true;
        for (uint16_t mv = TGT - CHARGE_SMART_CV_BAND_MV; mv <= TGT; mv += 10) {
            uint16_t s = smart(mv, TGT, 2500).setMa;
            if (s > prev) mono = false;
            prev = s;
        }
        printf("   від межі зони до цілі: %u -> %u мА (поріг завершення %u)\n",
               smart(TGT - CHARGE_SMART_CV_BAND_MV + 1, TGT, 2500).setMa,
               smart(TGT, TGT, 2500).setMa, endMa);
        check(mono, "уставка в зоні CV монотонно спадає");
        // ⚑ Монотонності МАЛО: стала уставка теж «не росте». Вимагаємо саме
        //  СПАДУ — інакше фази CV немає, є просто CC, доведений до цілі.
        check(smart(TGT - CHARGE_SMART_CV_BAND_MV + 1, TGT, 2500).setMa >
              smart(TGT, TGT, 2500).setMa + 50,
              "…і спадає СУТТЄВО: на початку зони струм помітно більший, ніж на цілі");
        // ⚑ І ЦЬОГО МАЛО. Рівно на цілі уставку віддає ОКРЕМА гілка (packMv >=
        //  targetMv -> endMa), тож пара «межа зони / ціль» лишалась би різною
        //  навіть тоді, коли всередині зони уставка стоїть колом, тобто коли
        //  фази CV насправді немає — є CC, доведений до цілі, і обрив у ній.
        //  Тому міряємо спад САМЕ ВСЕРЕДИНІ зони. Спіймано звіркою від
        //  протилежного: плоска уставка проходила попередню перевірку.
        uint16_t sHi  = smart(TGT - CHARGE_SMART_CV_BAND_MV + 1, TGT, 2500).setMa;
        uint16_t sMid = smart(TGT - CHARGE_SMART_CV_BAND_MV / 2, TGT, 2500).setMa;
        uint16_t sLo  = smart(TGT - 10, TGT, 2500).setMa;
        printf("   усередині зони: %u -> %u -> %u мА\n", sHi, sMid, sLo);
        check(sMid + 50 < sHi && sLo + 50 < sMid,
              "спад іде ВСЕРЕДИНІ зони, а не одним стрибком на цілі");
        check(sLo <= endMa + (uint16_t)((sHi - endMa) / 8),
              "…і біля цілі уставка вже майже дорівнює порогу завершення");
        check(smart(TGT, TGT, 2500).setMa == endMa,
              "рівно на цілі уставка = поріг завершення");
        // ⚑ ГОЛОВНИЙ ІНВАРІАНТ РЕЖИМУ. Якби підлога уставки була ВИЩЕ за поріг
        //  завершення, регулятор тримав би підлогу, струм ніколи не впав би
        //  нижче порога, і заряд ішов би до стелі часу. Тобто «розумний» режим
        //  не завершувався б узагалі.
        check(smart(TGT + 500, TGT, 2500).setMa == endMa,
              "вище цілі уставка не росте — лишається порогом");
        check(chargeSmartEndMa(2500) >= CHARGE_SMART_MA_MIN &&
              chargeSmartEndMa(50)   >= CHARGE_SMART_MA_MIN,
              "поріг завершення ніколи не нижчий за підлогу уставки");
    }

    printf("\n4) температурне вікно\n");
    {
        check(smart(7500, TGT, 2500, -1, true).phase == CHG_PH_HOLD,
              "-0.1 °C -> ПАУЗА (а не «нуль градусів, можна»)");
        check(smart(7500, TGT, 2500, -1, true).setMa == 0, "…і струм рівно нуль");
        check(smart(7500, TGT, 2500, (int16_t)(CHARGE_SMART_T_MAX_C * 10 + 1), true).phase == CHG_PH_HOLD,
              "вище верхньої межі -> ПАУЗА");
        check(smart(7500, TGT, 2500, 0, true).phase == CHG_PH_CC, "рівно 0 °C — можна");
        uint16_t cold = smart(7500, TGT, 2500, 50, true).setMa;      // 5 °C
        uint16_t warm = smart(7500, TGT, 2500, 250, true).setMa;     // 25 °C
        printf("   при 5 °C %u мА, при 25 °C %u мА\n", cold, warm);
        check(cold < warm, "на холоді струм зменшено");
        check(cold == chargeSmartClamp((uint32_t)warm * CHARGE_SMART_COOL_PCT / 100),
              "…рівно на задану частку, а не «трохи»");
        check(smart(7500, TGT, 2500, -300, false).phase == CHG_PH_CC,
              "без свіжого виміру температури вікно не діє (інакше заряд стояв би завжди)");
    }

    printf("\n5) завершення — за СТРУМОМ, і не з першого відліку\n");
    {
        uint16_t endMa = chargeSmartEndMa(2500);
        ChargeSmartOut atTgt = smart(TGT, TGT, 2500);
        uint8_t polls = 0;
        bool doneEarly = false;
        for (int i = 0; i < CHARGE_SMART_END_POLLS - 1; i++)
            if (chargeSmartFull(atTgt, endMa - 5, endMa, &polls)) doneEarly = true;
        check(!doneEarly, "менше за END_POLLS відліків — ще не «повний»");
        check(chargeSmartFull(atTgt, endMa - 5, endMa, &polls), "на END_POLLS — повний");
        // Сплеск струму скидає лічильник: інакше «повний» ставився б за
        // випадковий провал під час виміру на закритому ключі.
        polls = 0;
        for (int i = 0; i < CHARGE_SMART_END_POLLS - 1; i++)
            chargeSmartFull(atTgt, endMa - 5, endMa, &polls);
        chargeSmartFull(atTgt, endMa + 500, endMa, &polls);
        check(polls == 0, "сплеск струму скидає лічильник завершення");
        // У фазі CC «повний» не ставиться навіть при нульовому струмі: нульовий
        // струм у CC — це несправність, а не повний пакет.
        polls = 0;
        ChargeSmartOut cc = smart(7500, TGT, 2500);
        bool ccDone = false;
        for (int i = 0; i < 20; i++) if (chargeSmartFull(cc, 0, endMa, &polls)) ccDone = true;
        check(!ccDone, "у фазі CC нульовий струм НЕ вважається завершенням");
        // ⚑ У великого пакета уставка CC далеко вище за поріг, тож попередню
        //  перевірку тримає не фаза, а сама уставка — і викидання фази з умови
        //  лишалось би непоміченим. Беремо ДРІБНИЙ пакет: у нього і уставка CC,
        //  і поріг завершення впираються в ту саму підлогу, тож єдине, що
        //  відрізняє «йде заряд» від «пакет повний», — це фаза.
        const uint16_t TINY = 50;
        uint16_t tinyEnd = chargeSmartEndMa(TINY);
        ChargeSmartOut tinyCc = smart(7500, TGT, TINY);
        printf("   дрібний пакет %u мА·год: уставка CC %u мА, поріг %u мА\n",
               TINY, tinyCc.setMa, tinyEnd);
        check(tinyCc.phase == CHG_PH_CC && tinyCc.setMa <= tinyEnd + CHARGE_DEADBAND_MA,
              "у дрібного пакета уставка CC сама лежить на порозі (умова на струм тут не рятує)");
        polls = 0;
        bool tinyDone = false;
        for (int i = 0; i < 20; i++)
            if (chargeSmartFull(tinyCc, 0, tinyEnd, &polls)) tinyDone = true;
        check(!tinyDone, "…і все одно фаза CC не дає оголосити його повним");
    }

    printf("\n6) повний цикл на моделі пакета (2500 мА·год від 6.50 В)\n");
    {
        const uint16_t RATED = 2500;
        double mah = 0.0;
        // Стартова ємність, що відповідає 6.50 В за кривою SoC.
        int startPct = impresPercentFromMv(6500);
        mah = (double)RATED * startPct / 100.0;
        uint8_t polls = 0;
        uint16_t endMa = chargeSmartEndMa(RATED);
        int sawPre = 0, sawCc = 0, sawCv = 0;
        uint16_t maxMv = 0;
        long s = 0;
        bool done = false;
        for (; s < (long)CHARGE_MAX_MIN * 60; s++) {
            int pct = (int)(mah * 100.0 / RATED);
            if (pct > 100) pct = 100;
            uint16_t mv = (uint16_t)impresMvFromPercent(pct);
            if (mv > maxMv) maxMv = mv;
            ChargeSmartOut o = smart(mv, TGT, RATED);
            if (o.phase == CHG_PH_PRE) sawPre++;
            if (o.phase == CHG_PH_CC)  sawCc++;
            if (o.phase == CHG_PH_CV)  sawCv++;
            if (chargeSmartFull(o, (int32_t)o.setMa, endMa, &polls)) { done = true; break; }
            mah += (double)o.setMa / 3600.0;      // регулятор ідеально тримає уставку
        }
        printf("   завершено за %ld с (%.1f год), залито %.0f мА·год, "
               "найвища напруга %u мВ\n", s, s / 3600.0,
               mah - (double)RATED * startPct / 100.0, maxMv);
        printf("   фази: передзаряд %d с, CC %d с, CV %d с\n", sawPre, sawCc, sawCv);
        check(done, "цикл завершується сам, а не впирається в стелю часу");
        check(sawCc > 0 && sawCv > 0, "пройдено обидві фази — і CC, і CV");
        check(maxMv <= TGT, "напруга НІ РАЗУ не перевищила ціль");
        check(s > 600, "цикл не «проскочив» миттєво (це була б помилка моделі або логіки)");
    }

    printf("\n7) ручні уставки: межі й те, що ручне не піднімає струм у кінці\n");
    {
        check(chargeManualClamp(1) == CHARGE_MANUAL_MA_MIN, "струм заряду затискається знизу");
        check(chargeManualClamp(9999) == CHARGE_MANUAL_MA_MAX, "…і згори");
        check(chargeSetManualMa(0) == 0, "нуль — це «автомат», а не затиснутий мінімум");
        check(chargeSetManualMa(400) == 400, "звичайне значення проходить як є");
        check(chargeApplyManual(1000, 400, false) == 400, "поза дозарядом діє ручне");
        check(chargeApplyManual(100, 400, true) == 100,
              "у дозаряді ручне НЕ піднімає струм");
        check(chargeApplyManual(400, 100, true) == 100, "…але зменшити дозволено");
        chargeSetManualMa(0);

        check(chargeManualMvClamp(1000) == CHARGE_MANUAL_MV_MIN, "ціль у вольтах — знизу");
        check(chargeManualMvClamp(9999) == CHARGE_MANUAL_MV_MAX, "…і згори");
        check(CHARGE_MANUAL_MV_MAX <= CHARGE_TARGET_MV,
              "ручна ціль не може бути вищою за заводську (інакше аварійна межа поїде)");
        check(chargeSetManualMv(7400) == 7400, "напруга зберігання проходить як є");
        check(chargeSetManualMv(0) == 0, "нуль повертає ціль за відсотком");

        check(dischargeManualClamp(1) == DISCHARGE_MANUAL_MA_MIN, "струм розряду — знизу");
        check(dischargeManualClamp(9999) == DISCHARGE_MANUAL_MA_MAX, "…і згори");
        check(dischargeApplyManual(300, 900, true) == 300,
              "біля цілі розряду ручне теж не піднімає струм");
    }

    printf("\n8) єдина точка уставки перемикається профілем\n");
    {
        bool taper = false; uint8_t ph = 0;
        chargeSetProfile(CHG_PROF_FACTORY);
        uint16_t f = chargeSetpointFor(50, 100, 7500, TGT, 250, true, &taper, &ph);
        chargeSetProfile(CHG_PROF_SMART);
        chargeSetRatedMah(1300);
        taper = false;
        uint16_t s = chargeSetpointFor(50, 100, 7500, TGT, 250, true, &taper, &ph);
        printf("   на 7.50 В: заводський %u мА, розумний (пакет 1300) %u мА\n", f, s);
        check(f != s, "профілі дають різні уставки — перемикач справді діє");
        check(s == chargeSmartCMa(1300, CHARGE_SMART_CC_PCT), "розумний бере 0.5C пакета");
        check(ph == CHG_PH_CC, "…і повідомляє фазу");
        // Пауза за температурою мусить доходити до самої машини як НУЛЬ струму.
        uint16_t h = chargeSetpointFor(50, 100, 7500, TGT, -50, true, &taper, &ph);
        check(h == 0 && ph == CHG_PH_HOLD, "пауза за холодом віддає нульову уставку");
        chargeSetProfile(CHG_PROF_FACTORY);
        chargeSetRatedMah(0);
    }

    printf("\n9) розумний РОЗРЯД\n");
    {
        uint8_t ph = 0;
        uint16_t tgt = DISCHARGE_TARGET_MV;
        check(dischargeSmartMa(8200, tgt, 2500, 250, true, &ph) == 500,
              "0.2C від 2500 — це 500 мА");
        check(ph == DIS_PH_CC, "…і це фаза постійного струму");
        check(dischargeSmartMa(8200, tgt, 1300, 250, true, &ph) == DISCHARGE_MA_LO,
              "0.2C від 1300 нижче нижньої межі лінійки — беремо межу");
        check(dischargeSmartMa(tgt, tgt, 2500, 250, true, &ph) == DISCHARGE_MA_LO,
              "на цілі — найменший струм");
        check(ph == DIS_PH_TAPER, "…і це фаза спаду");
        uint16_t mid = dischargeSmartMa(tgt + DISCHARGE_SMART_BAND_MV / 2, tgt, 2500, 250, true, &ph);
        printf("   на пів-зони до цілі: %u мА (CC 500, кінець %u)\n", mid, (unsigned)DISCHARGE_MA_LO);
        check(mid > DISCHARGE_MA_LO && mid < 500, "у зоні спаду струм між межами");
        check(dischargeSmartMa(8200, tgt, 2500,
                               (int16_t)(DISCHARGE_SMART_T_MAX_C * 10 + 1), true, &ph) == 0,
              "перегрів -> нуль струму");
        check(ph == DIS_PH_HOLD, "…і фаза «пауза»");
    }

    printf("\n10) скільки ще лишилось\n");
    {
        // Заряд: 2500 мА·год, від 50 % до 100 % при 1000 мА -> 1250 мА·год -> 1.25 год.
        uint32_t e = chargeEtaS(CHG_PH_CC, 50, 100, 2500, 1000);
        printf("   заряд 50->100 %% при 1000 мА: %lu с (%.2f год)\n",
               (unsigned long)e, e / 3600.0);
        check(e == 4500, "оцінка = залишок ємності / струм");
        check(chargeEtaS(CHG_PH_CV, 50, 100, 2500, 1000) == 2 * e,
              "у фазі CV оцінка подвоюється — струм там спадає сам");
        check(chargeEtaS(CHG_PH_CC, 50, 100, 2500, 0) == 0,
              "без струму оцінки немає (нуль = «не знаю», а не «зараз кінець»)");
        check(chargeEtaS(CHG_PH_HOLD, 50, 100, 2500, 1000) == 0,
              "на паузі оцінки теж немає");
        check(chargeEtaS(CHG_PH_CC, 100, 100, 2500, 1000) == 0, "нічого не лишилось — нуль");
        check(chargeEtaS(CHG_PH_CC, 1, 100, 2500, CHARGE_DEADBAND_MA + 1) == 0,
              "неправдоподібно велика оцінка не показується взагалі");
        uint32_t d = dischargeEtaS(100, 50, 2500, 500);
        printf("   розряд 100->50 %% при 500 мА: %lu с (%.2f год)\n",
               (unsigned long)d, d / 3600.0);
        check(d == 9000, "розряд рахується так само");
        check(dischargeEtaS(50, 100, 2500, 500) == 0, "ціль вище поточного — оцінки немає");
        check(dischargeEtaS(100, 50, 2500, 0) == 0, "без струму — немає");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
