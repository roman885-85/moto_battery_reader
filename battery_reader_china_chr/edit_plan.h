#ifndef EDIT_PLAN_H
#define EDIT_PLAN_H
// ===========================================================================
//  edit_plan.h — РУЧНЕ РЕДАГУВАННЯ ВСІХ ЗНАЧЕНЬ ПАКЕТА, ОДНИМ СПИСКОМ
// ===========================================================================
//  Скарга власника, з якої це виросло: «виконати в окремій закладці ручне
//  редагування всіх змінних, з якими зараз працюємо: усі лічильники, дати,
//  наробітки, виробітки, рівень заряду. Якщо десь вони дублюються в інших
//  вкладках — видалити звідти».
//
//  ⚑ ЧОМУ ЦЕ НЕ ПРОСТО «ЩЕ ОДНА ВКЛАДКА». Кожне з цих значень уже мало свою
//  кнопку, свій пароль, свою підказку й свій API — і жило в іншому місці:
//  ємність в одній підвкладці, знос у другій, дата в третій, цикли взагалі
//  тільки всередині планів синхронізації. Через це не було жодного місця, де
//  видно ВЕСЬ стан пакета одразу, — а саме він і потрібен, коли розбираєш,
//  чому пакет поводиться дивно. Тепер список один, і він тут: назви, одиниці,
//  межі й правила узгодженості живуть в одному файлі, а всі три клієнти лише
//  показують те, що назвав пристрій.
//
//  ⚑ ДАТИ ЖИВУТЬ ЗМІЩЕННЯМИ, А НЕ ДАТАМИ. У зашифрованому блоці лежить дата
//  ВИГОТОВЛЕННЯ, а «перший запуск», «останній заряд» і «останнє кондиціювання»
//  — це кількість діб ВІД неї. Тому дату виготовлення не можна посунути, не
//  зачепивши решту, і тому ж не можна поставити перший запуск раніше за
//  виготовлення. Раніше цих правил не було ніде: план відновлення писав дати
//  цілим набором, і суперечливий набір скласти було ніяк. Ручний редактор
//  дозволяє задати кожне поле окремо — отже, мусить сам і не пускати
//  неможливе.
//
//  ⚑ НЕМОЖЛИВИЙ СТАН — ЦЕ НЕ ПЕДАНТИЗМ. «Пакет, який уже вмикали, не міг
//  жодного разу заряджатись» — цей самий стан аудит позначає як AUD_USE_BEFORE_CHG,
//  і саме такі набори станція «лікує», повертаючи свої числа. Редактор, який
//  дозволяє його створити, працював би проти всієї решти проєкту.
// ===========================================================================

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "impres_format.h"
#include "impres_bms.h"
#include "impres_crypt.h"
#include "restore_plan.h"   // RP_ETM_MAX, RP_RS_*: межі спільні з планом еталона

// ── ПОЛЯ. Порядок = порядок у таблиці всіх трьох клієнтів ─────────────────
enum {
    EDF_MFG = 0,     // дата виготовлення,        YYYYMMDD   DS2433
    EDF_USE,         // дата першого запуску,     YYYYMMDD   DS2433
    EDF_LASTCHG,     // дата останнього заряду,   YYYYMMDD   DS2433
    EDF_LASTREC,     // дата останнього кондиц.,  YYYYMMDD   DS2433
    EDF_CYCLES,      // цикли IMPRES (гістограма)            DS2433
    EDF_NONIMP,      // цикли не-IMPRES                      DS2433
    EDF_CALCYC,      // калібрувальні цикли                  DS2433
    EDF_HISTCCA,     // наробіток у пакеті: заряд  (сирі)    DS2433
    EDF_HISTDCA,     // наробіток у пакеті: розряд (сирі)    DS2433
    EDF_RATED,       // паспортна ємність, мА·год            DS2433
    EDF_HEALTH,      // знос, %                              DS2433
    EDF_ETM,         // наробіток монітора, доби             DS2438
    EDF_MONCCA,      // лічильник заряду монітора  (сирі)    DS2438
    EDF_MONDCA,      // лічильник розряду монітора (сирі)    DS2438
    EDF_ICA,         // залишок, мА·год                      DS2438
    EDF_COUNT
};

// Тип значення — щоб клієнт знав, малювати поле дати чи число.
enum { EDT_NUM = 0, EDT_DATE = 1 };

struct EditField {
    long cur;        // поточне значення; −1 — не читається
    long lo, hi;     // допустимі межі
    long want;       // −1 — не чіпати; інакше записати це
    bool avail;      // чи є що читати й куди писати
};

struct EditPlan {
    EditField f[EDF_COUNT];
    bool  have33, have38, haveKey;
    uint8_t k1, k2;
    int   ratedEff;      // паспортна ємність, за якою рахується ICA
    float rsOhm;         // шунт цього пакета
    long  today;         // YYYYMMDD; 0 — годинник не заведено
    char  err[112];      // причина відмови (лише коли виправити нема чого)
    char  fix[240];      // що саме виправлено — рядок для людини
};

// ── ІМЕНА Й ОДИНИЦІ — ОДНІ НА ВСІ ТРИ КЛІЄНТИ ─────────────────────────────
//  Три копії цього списку неминуче розійшлися б: у проєкті це вже ставалось
//  із назвою силового ключа й із причиною недоступності заряду.
inline const char *editFieldName(int i) {
    switch (i) {
        case EDF_MFG:     return "Дата виготовлення";
        case EDF_USE:     return "Дата першого запуску";
        case EDF_LASTCHG: return "Дата останнього заряду";
        case EDF_LASTREC: return "Дата останнього кондиціювання";
        case EDF_CYCLES:  return "Цикли IMPRES";
        case EDF_NONIMP:  return "Цикли не-IMPRES";
        case EDF_CALCYC:  return "Калібрувальні цикли";
        case EDF_HISTCCA: return "Наробіток у пакеті: заряд";
        case EDF_HISTDCA: return "Наробіток у пакеті: розряд";
        case EDF_RATED:   return "Паспортна ємність";
        case EDF_HEALTH:  return "Знос (здоров'я)";
        case EDF_ETM:     return "Наробіток монітора";
        case EDF_MONCCA:  return "Лічильник заряду монітора";
        case EDF_MONDCA:  return "Лічильник розряду монітора";
        case EDF_ICA:     return "Залишок (паливомір ICA)";
        default:          return "";
    }
}
inline const char *editFieldUnit(int i) {
    switch (i) {
        case EDF_MFG: case EDF_USE: case EDF_LASTCHG: case EDF_LASTREC: return "";
        case EDF_CYCLES: case EDF_NONIMP: case EDF_CALCYC: return "циклів";
        case EDF_HISTCCA: case EDF_HISTDCA:
        case EDF_MONCCA:  case EDF_MONDCA:  return "сирих одиниць";
        case EDF_RATED:   case EDF_ICA:     return "мА·год";
        case EDF_HEALTH:  return "%";
        case EDF_ETM:     return "діб";
        default:          return "";
    }
}
inline int editFieldType(int i) {
    return (i == EDF_MFG || i == EDF_USE || i == EDF_LASTCHG || i == EDF_LASTREC)
           ? EDT_DATE : EDT_NUM;
}
// 33 або 38 — у який чип поїде правка. Клієнт групує список саме за цим:
// «що записано в самому пакеті» й «що бачить монітор» — різні речі, і плутати
// їх у одному стовпчику означало б повторити помилку, з якої почалась уся
// історія з поверненням лічильників.
inline int editFieldChip(int i) { return (i >= EDF_ETM) ? 38 : 33; }

// ── ЧИТАННЯ ───────────────────────────────────────────────────────────────
inline void editPlanBuild(EditPlan &p, const uint8_t *d33, const uint8_t *d38,
                          const uint8_t *rom33, long today) {
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < EDF_COUNT; i++) { p.f[i].cur = -1; p.f[i].want = -1; }
    p.have33 = (d33 != nullptr);
    p.have38 = (d38 != nullptr);
    p.today  = today;
    p.rsOhm  = 0.0f;

    ImpresBms b;
    bool ok = d33 && impresBmsParse(d33, d38, rom33, 0.0f, &b) && b.ok;
    if (ok && !b.haveKey) impresBmsFindKey(d33, d38, &b);   // ROM може бути невідомий
    p.haveKey = ok && b.haveKey;
    if (ok) {
        p.rsOhm = b.rsense;
        char model[16] = "";
        impresModelName(d33, model, sizeof(model));
        p.ratedEff = impresRatedMahFor(d33, model);
    }

    // ── DS2433: незашифроване ─────────────────────────────────────────────
    if (d33) {
        int rated = impresRatedFromDump(d33);
        p.f[EDF_RATED].avail = true;
        p.f[EDF_RATED].cur   = rated > 0 ? rated : -1;
        p.f[EDF_RATED].lo    = IMPRES_RATED_MIN_MAH;
        p.f[EDF_RATED].hi    = IMPRES_RATED_MAX_MAH;

        uint16_t hC = 0, hD = 0;
        bool haveHist = impresBmsHistCounters(d33, &hC, &hD);
        for (int i : { EDF_HISTCCA, EDF_HISTDCA }) {
            p.f[i].avail = haveHist; p.f[i].lo = 0; p.f[i].hi = 0xFFFF;
        }
        if (haveHist) { p.f[EDF_HISTCCA].cur = hC; p.f[EDF_HISTDCA].cur = hD; }

        if (ok) {
            p.f[EDF_CYCLES].avail = (b.cycles >= 0);
            p.f[EDF_CYCLES].cur   = b.cycles;
            p.f[EDF_CYCLES].lo = 0; p.f[EDF_CYCLES].hi = 0xFFFF;
            p.f[EDF_NONIMP].avail = (b.nonImpresCycles >= 0);
            p.f[EDF_NONIMP].cur   = b.nonImpresCycles;
            p.f[EDF_NONIMP].lo = 0; p.f[EDF_NONIMP].hi = 0xFFFF;
        }
    }

    // ── DS2433: зашифроване (дати, калібрувальні цикли, знос) ─────────────
    if (p.haveKey) {
        p.k1 = b.key1; p.k2 = b.key2;
        ImpresCryptFields cf;
        impresCryptRead(d33, b.key1, b.key2, &cf);
        if (cf.haveDat && impresBmsDateSane(cf.mfgY, cf.mfgM, cf.mfgD)) {
            long mfgDays = impresBmsToDays(cf.mfgY, cf.mfgM, cf.mfgD);
            p.f[EDF_MFG].avail = true;
            p.f[EDF_MFG].cur   = restoreDateNum(cf.mfgY, cf.mfgM, cf.mfgD);
            p.f[EDF_MFG].lo = 20000101; p.f[EDF_MFG].hi = 20991231;
            // Похідні дати: зміщення від виготовлення. Нуль означає «події не
            // було», і це НЕ те саме, що «сталась у день виготовлення», —
            // тому нуль лишається порожнім значенням, а не датою.
            auto derived = [&](int idx, uint16_t off) {
                p.f[idx].avail = true;
                p.f[idx].lo = p.f[EDF_MFG].cur; p.f[idx].hi = 20991231;
                if (!off) { p.f[idx].cur = 0; return; }        // події не було
                int y, m, d; impresBmsFromDays(mfgDays + off, &y, &m, &d);
                p.f[idx].cur = restoreDateNum(y, m, d);
            };
            derived(EDF_USE,     cf.dayInitialUse);
            derived(EDF_LASTCHG, cf.dayLastCharge);
        }
        if (cf.haveRec) {
            p.f[EDF_CALCYC].avail = true;
            p.f[EDF_CALCYC].cur   = cf.calCycles;
            p.f[EDF_CALCYC].lo = 0; p.f[EDF_CALCYC].hi = 0xFFFF;
            // Знос у відсотках, а не сирим CTS: сире число нічого не
            // означає нікому, а перерахунок уже є й живе в одному місці.
            p.f[EDF_HEALTH].avail = (p.ratedEff > 0 && p.rsOhm > 0.0f);
            p.f[EDF_HEALTH].cur   = restoreHealthFromCts(cf.cts, p.ratedEff, p.rsOhm);
            if (p.f[EDF_HEALTH].cur <= 0) p.f[EDF_HEALTH].cur = -1;
            p.f[EDF_HEALTH].lo = 1; p.f[EDF_HEALTH].hi = 100;
            if (p.f[EDF_MFG].avail) {
                long mfgDays = impresBmsToDays(cf.mfgY, cf.mfgM, cf.mfgD);
                p.f[EDF_LASTREC].avail = true;
                p.f[EDF_LASTREC].lo = p.f[EDF_MFG].cur; p.f[EDF_LASTREC].hi = 20991231;
                if (!cf.dayLastRecond) p.f[EDF_LASTREC].cur = 0;
                else {
                    int y, m, d;
                    impresBmsFromDays(mfgDays + cf.dayLastRecond, &y, &m, &d);
                    p.f[EDF_LASTREC].cur = restoreDateNum(y, m, d);
                }
            }
        }
    }

    // ── DS2438: монітор ───────────────────────────────────────────────────
    if (d38) {
        p.f[EDF_ETM].avail = true;
        p.f[EDF_ETM].cur   = (long)(impresEtm(d38) / 86400UL);
        p.f[EDF_ETM].lo = 0; p.f[EDF_ETM].hi = (long)(RP_ETM_MAX / 86400UL);
        p.f[EDF_MONCCA].avail = true; p.f[EDF_MONCCA].cur = impresCca(d38);
        p.f[EDF_MONCCA].lo = 0; p.f[EDF_MONCCA].hi = 0xFFFF;
        p.f[EDF_MONDCA].avail = true; p.f[EDF_MONDCA].cur = impresDca(d38);
        p.f[EDF_MONDCA].lo = 0; p.f[EDF_MONDCA].hi = 0xFFFF;
        p.f[EDF_ICA].avail = true;
        p.f[EDF_ICA].lo = 0;
        p.f[EDF_ICA].hi = (p.ratedEff > 0) ? p.ratedEff : 5000;
        p.f[EDF_ICA].cur = (p.ratedEff > 0)
                         ? impresIcaToMahRs(d38[12], p.ratedEff, p.rsOhm)
                         : -1;
    }
}

// ── ЗАДАТИ ЗНАЧЕННЯ ───────────────────────────────────────────────────────
//  ⚑ ВІДМОВА НАЗИВАЄ СВОЮ ПРИЧИНУ, І ПРИЧИНИ РІЗНІ. «Не можна» без пояснення
//  тут найгірше: людина бачить поле, вписує число, нічого не стається — і
//  вирішує, що редактор не працює. Так уже було з узгодженням наробітку.
//  ⚑ ПОМИЛКУ ВВЕДЕННЯ ВИПРАВЛЯЄМО, А НЕ ВІДХИЛЯЄМО. Так попросив власник, і
//  прохання по суті: у кожної помилки тут є ОДНА очевидна правильна відповідь.
//  Вписали 6000 туди, де стеля 255, — малось на увазі 255; вписали дату
//  наступного року — малось на увазі «сьогодні»; вписали 45-те число —
//  малось на увазі останнє число місяця. Відмова змушувала гадати, яке ж
//  число підійде, і набирати наново; виправлення дає результат одразу.
//
//  ⚑ АЛЕ НІКОЛИ МОВЧКИ. Кожна правка записується в p.fix і доїжджає до
//  людини разом із результатом: «виправлено» без пояснення нічим не краще за
//  «не можна» — і те, і те лишає людину без розуміння, що ж сталось із її
//  числом. Тому виправляємо і кажемо, що саме виправили.
inline void editFixNote(EditPlan &p, int i, long from, long to, const char *why) {
    size_t n = strlen(p.fix);
    if (n + 16 >= sizeof(p.fix)) return;          // не влізло — краще обрізати
    if (n) { snprintf(p.fix + n, sizeof(p.fix) - n, "; "); n = strlen(p.fix); }
    snprintf(p.fix + n, sizeof(p.fix) - n, "%s: %ld → %ld (%s)",
             editFieldName(i), from, to, why);
}

// Скільки діб у місяці — щоб «45-те» стало останнім числом, а не сміттям.
inline int editDaysInMonth(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 31;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

inline bool editPlanSet(EditPlan &p, int i, long v) {
    auto no = [&](const char *m) { snprintf(p.err, sizeof(p.err), "%s", m); return false; };
    p.err[0] = '\0';
    // Виправити НЕМА ЧОГО, коли писати нікуди: поля з такою назвою немає або
    // блок, у якому воно живе, не читається. Це не помилка введення.
    if (i < 0 || i >= EDF_COUNT)  return no("такого поля немає");
    if (!p.f[i].avail)            return no("це поле в пакеті не читається — писати нікуди");
    if (v < 0)                    { p.f[i].want = -1; return true; }   // «не чіпати»

    const long asked = v;
    if (editFieldType(i) == EDT_DATE) {
        // Нуль — законне значення для похідних дат: «події не було». Для дати
        // виготовлення це не так: без неї решті нема від чого рахуватись, і
        // очевидне виправлення — лишити ту, що вже стоїть.
        if (v == 0) {
            if (i != EDF_MFG) { p.f[i].want = 0; return true; }
            if (p.f[i].cur <= 0) return no("дату виготовлення прибрати не можна, а замінити нема на що");
            editFixNote(p, i, asked, p.f[i].cur, "дату виготовлення прибрати не можна");
            v = p.f[i].cur;
        } else {
            int y = (int)(v / 10000), m = (int)((v / 100) % 100), d = (int)(v % 100);
            int y0 = y, m0 = m, d0 = d;
            if (y < 2005) y = 2005;
            if (y > 2035) y = 2035;
            if (m < 1) m = 1;
            if (m > 12) m = 12;
            if (d < 1) d = 1;
            if (d > editDaysInMonth(y, m)) d = editDaysInMonth(y, m);
            if (y != y0 || m != m0 || d != d0) {
                long fixed = restoreDateNum(y, m, d);
                editFixNote(p, i, asked, fixed, "це не схоже на дату");
                v = fixed;
            }
            // Дата в майбутньому: очевидна відповідь — сьогодні. Пакета, який
            // почав працювати завтра, не буває.
            if (p.today > 0 && v > p.today) {
                editFixNote(p, i, v, p.today, "дата в майбутньому");
                v = p.today;
            }
        }
    }
    if (v < p.f[i].lo || v > p.f[i].hi) {
        long fixed = (v < p.f[i].lo) ? p.f[i].lo : p.f[i].hi;
        char why[64];
        snprintf(why, sizeof(why), "межі поля %ld…%ld", p.f[i].lo, p.f[i].hi);
        editFixNote(p, i, v, fixed, why);
        v = fixed;
    }
    p.f[i].want = v;
    return true;
}

// Скільки правок було виправлено (для клієнта: показувати чи ні).
inline bool editPlanFixed(const EditPlan &p) { return p.fix[0] != '\0'; }

// Значення, яке буде після застосування плану: задане, а як не задане — поточне.
inline long editPlanEff(const EditPlan &p, int i) {
    return (p.f[i].want >= 0) ? p.f[i].want : p.f[i].cur;
}

// ── ЧИ НЕ СУПЕРЕЧИТЬ НАБІР САМ СОБІ ───────────────────────────────────────
//  Перевіряємо РЕЗУЛЬТАТ, а не окрему правку: людина може посунути дату
//  виготовлення й дату запуску одним заходом, і кожна з них окремо виглядає
//  безглуздою рівно доти, доки не побачиш другу.
inline bool editPlanConsistent(const EditPlan &p, char *why, size_t whyN) {
    auto no = [&](const char *m) { if (why && whyN) snprintf(why, whyN, "%s", m); return false; };
    if (why && whyN) why[0] = '\0';
    long mfg = editPlanEff(p, EDF_MFG);
    long use = editPlanEff(p, EDF_USE);
    long chg = editPlanEff(p, EDF_LASTCHG);
    long rec = editPlanEff(p, EDF_LASTREC);
    if (mfg > 0) {
        if (use > 0 && use < mfg) return no("перший запуск раніший за виготовлення");
        if (chg > 0 && chg < mfg) return no("останній заряд раніший за виготовлення");
        if (rec > 0 && rec < mfg) return no("останнє кондиціювання раніше за виготовлення");
    }
    // ⚑ ТОЙ САМИЙ НЕМОЖЛИВИЙ СТАН, ЩО Й У АУДИТІ (AUD_USE_BEFORE_CHG). Пакет,
    //  який уже вмикали, не міг жодного разу заряджатись — і саме такі набори
    //  станція «лікує», повертаючи свої числа.
    if (use > 0 && chg == 0) return no("пакет уже вмикали, але жодного разу не заряджали — "
                                       "станція вважає такий набір побитим");
    if (use > 0 && chg > 0 && chg < use) return no("останній заряд раніший за перший запуск");
    return true;
}

// ── ВИПРАВИТИ СУПЕРЕЧЛИВИЙ НАБІР ──────────────────────────────────────────
//  ⚑ У КОЖНОЇ ТУТЕШНЬОЇ СУПЕРЕЧНОСТІ Є ОДНА ОЧЕВИДНА ПОЧИНКА, І ВОНА
//  МІНІМАЛЬНА: підтягнути пізнішу подію до ранішої, а не навпаки. Дата
//  виготовлення — опора всього блока (решта зберігається зміщенням ВІД неї),
//  тому рухаємо не її, а те, що з нею не сходиться. Так само робить і
//  impresCryptNormalize() перед записом: «останній заряд не може бути раніший
//  за перший запуск» — правило одне на весь проєкт, і воно не роздвоюється.
//
//  ⚑ ПОЧИНКА ЙДЕ В ЦИКЛ. Одна правка створює наступну: підтягнули запуск до
//  виготовлення — тепер із запуском не сходиться заряд. Трьох проходів
//  вистачає з запасом (ланцюжок тут завдовжки два), а верхня межа стоїть, щоб
//  помилка в правилах не дала вічного циклу замість відповіді.
inline bool editPlanRepair(EditPlan &p) {
    auto pull = [&](int idx, long to, const char *why) -> bool {
        if (!p.f[idx].avail) return false;      // нема куди писати — і чинити нічим
        editFixNote(p, idx, editPlanEff(p, idx), to, why);
        p.f[idx].want = to;
        return true;
    };
    for (int pass = 0; pass < 3; pass++) {
        char why[128];
        if (editPlanConsistent(p, why, sizeof(why))) return true;
        long mfg = editPlanEff(p, EDF_MFG);
        long use = editPlanEff(p, EDF_USE);
        long chg = editPlanEff(p, EDF_LASTCHG);
        long rec = editPlanEff(p, EDF_LASTREC);
        bool did = false;
        if (mfg > 0) {
            if (use > 0 && use < mfg) did |= pull(EDF_USE,     mfg, "раніше за виготовлення");
            if (chg > 0 && chg < mfg) did |= pull(EDF_LASTCHG, mfg, "раніше за виготовлення");
            if (rec > 0 && rec < mfg) did |= pull(EDF_LASTREC, mfg, "раніше за виготовлення");
        }
        // «Вмикали, але жодного разу не заряджали» — найменша правда, яка це
        // розв'язує: заряджали принаймні в день першого запуску.
        if (use > 0 && chg == 0)              did |= pull(EDF_LASTCHG, use, "пакет уже вмикали");
        else if (use > 0 && chg > 0 && chg < use)
                                              did |= pull(EDF_LASTCHG, use, "раніше за перший запуск");
        if (!did) return false;               // правило є, а полагодити нічим
    }
    char why[128];
    return editPlanConsistent(p, why, sizeof(why));
}

// Скільки полів справді буде записано.
inline int editPlanCount(const EditPlan &p, int chip) {
    int n = 0;
    for (int i = 0; i < EDF_COUNT; i++)
        if (p.f[i].want >= 0 && p.f[i].want != p.f[i].cur &&
            (chip == 0 || editFieldChip(i) == chip)) n++;
    return n;
}

// ── ЗАПИС ─────────────────────────────────────────────────────────────────
//  Пише В ПАМ'ЯТЬ (дампи), а не в чип: чипом займається той, хто нас кличе, —
//  так само, як у плані еталона. Повертає кількість застосованих полів;
//  wrote33/wrote38 кажуть, який дамп треба заливати.
inline int editPlanApply(EditPlan &p, uint8_t *d33, uint8_t *d38,
                         bool *wrote33, bool *wrote38) {
    int done = 0;
    if (wrote33) *wrote33 = false;
    if (wrote38) *wrote38 = false;
    auto want = [&](int i) { return p.f[i].want >= 0 && p.f[i].want != p.f[i].cur; };
    auto did  = [&](int i, bool ok33) {
        if (!ok33) return;
        done++;
        if (editFieldChip(i) == 33) { if (wrote33) *wrote33 = true; }
        else                        { if (wrote38) *wrote38 = true; }
    };

    if (d33) {
        if (want(EDF_RATED)) {
            // Байт 0x008 = ємність / 25 (див. impres_format.h). Записується
            // прямо: контрольної суми в цього байта немає, він поза записами.
            d33[IMPRES_RATED_BYTE] =
                (uint8_t)(p.f[EDF_RATED].want / IMPRES_RATED_STEP);
            did(EDF_RATED, true);
        }
        if (want(EDF_CYCLES))  did(EDF_CYCLES,  impresCyclesWrite(d33, (int)p.f[EDF_CYCLES].want));
        if (want(EDF_NONIMP))  did(EDF_NONIMP,  impresNonImpresWrite(d33, (int)p.f[EDF_NONIMP].want));
        // Обидва наробітки лежать в одному блоці й пишуться однією дією:
        // писати їх по черзі означало б двічі перерахувати ту саму суму.
        if (want(EDF_HISTCCA) || want(EDF_HISTDCA)) {
            uint16_t cca = (uint16_t)editPlanEff(p, EDF_HISTCCA);
            uint16_t dca = (uint16_t)editPlanEff(p, EDF_HISTDCA);
            if (impresHistCountersWrite(d33, cca, dca)) {
                if (want(EDF_HISTCCA)) did(EDF_HISTCCA, true);
                if (want(EDF_HISTDCA)) did(EDF_HISTDCA, true);
            }
        }
        // Зашифровані поля — теж одним заходом: читаємо весь набір, правимо
        // потрібне, пишемо назад. Інакше кожна правка перешифровувала б блок
        // заново, і сусідні поля довелось би відновлювати з пам'яті.
        bool anyCrypt = want(EDF_MFG) || want(EDF_USE) || want(EDF_LASTCHG) ||
                        want(EDF_LASTREC) || want(EDF_CALCYC) || want(EDF_HEALTH);
        if (p.haveKey && anyCrypt) {
            ImpresCryptFields cf;
            impresCryptRead(d33, p.k1, p.k2, &cf);
            long mfg = editPlanEff(p, EDF_MFG);
            if (mfg > 0) {
                cf.mfgY = (int)(mfg / 10000);
                cf.mfgM = (int)((mfg / 100) % 100);
                cf.mfgD = (int)(mfg % 100);
            }
            long mfgDays = impresBmsToDays(cf.mfgY, cf.mfgM, cf.mfgD);
            // ⚑ ПОХІДНІ ДАТИ ПЕРЕРАХОВУЮТЬСЯ ЗАВЖДИ, А НЕ ЛИШЕ КОЛИ ЇХ
            //  ЗАДАЛИ. Вони зберігаються ЗМІЩЕННЯМ від виготовлення: посунув
            //  виготовлення — і незаймана «дата останнього заряду» тихо
            //  поїхала б разом із ним.
            auto offOf = [&](int idx, uint16_t old) -> uint16_t {
                long v = editPlanEff(p, idx);
                if (v <= 0) return 0;                       // події не було
                if (p.f[idx].want < 0 && mfg == p.f[EDF_MFG].cur) return old;
                long days = impresBmsToDays((int)(v / 10000), (int)((v / 100) % 100),
                                            (int)(v % 100)) - mfgDays;
                return (uint16_t)(days > 0 ? days : 0);
            };
            uint16_t offUse = offOf(EDF_USE,     cf.dayInitialUse);
            cf.dayLastCharge = offOf(EDF_LASTCHG, cf.dayLastCharge);
            cf.dayLastRecond = offOf(EDF_LASTREC, cf.dayLastRecond);
            cf.dayInitialUse = offUse;
            cf.dayInitialUse2 = offUse;      // поле-близнюк: див. impres_crypt.h
            if (want(EDF_CALCYC)) cf.calCycles = (uint16_t)p.f[EDF_CALCYC].want;
            if (want(EDF_HEALTH))
                cf.cts = restoreCtsFromHealth((int)p.f[EDF_HEALTH].want, p.ratedEff, p.rsOhm);
            impresCryptWrite(d33, p.k1, p.k2, &cf);
            if (want(EDF_MFG))     did(EDF_MFG, true);
            if (want(EDF_USE))     did(EDF_USE, true);
            if (want(EDF_LASTCHG)) did(EDF_LASTCHG, true);
            if (want(EDF_LASTREC)) did(EDF_LASTREC, true);
            if (want(EDF_CALCYC))  did(EDF_CALCYC, true);
            if (want(EDF_HEALTH))  did(EDF_HEALTH, true);
        }
        if (wrote33 && *wrote33) impresFixHeader(d33);
    }

    if (d38) {
        if (want(EDF_ETM)) {
            impresSetEtm(d38, (uint32_t)(p.f[EDF_ETM].want * 86400L));
            did(EDF_ETM, true);
        }
        if (want(EDF_MONCCA)) {
            uint16_t v = (uint16_t)p.f[EDF_MONCCA].want;
            d38[60] = (uint8_t)(v & 0xFF); d38[61] = (uint8_t)(v >> 8);
            did(EDF_MONCCA, true);
        }
        if (want(EDF_MONDCA)) {
            uint16_t v = (uint16_t)p.f[EDF_MONDCA].want;
            d38[62] = (uint8_t)(v & 0xFF); d38[63] = (uint8_t)(v >> 8);
            did(EDF_MONDCA, true);
        }
        if (want(EDF_ICA) && p.ratedEff > 0) {
            // Паливомір — байт 0x0C DS2438 (див. impres_format.h).
            d38[12] = impresIcaFromMahRs(p.f[EDF_ICA].want, p.ratedEff, p.rsOhm);
            did(EDF_ICA, true);
        }
    }
    return done;
}

#endif // EDIT_PLAN_H
