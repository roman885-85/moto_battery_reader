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
    EDF_HISTCCA,     // наробіток у пакеті: заряд,  мА·год   DS2433
    EDF_HISTDCA,     // наробіток у пакеті: розряд, мА·год   DS2433
    EDF_RATED,       // паспортна ємність, мА·год            DS2433
    EDF_HEALTH,      // знос, %                              DS2433
    EDF_ETM,         // наробіток монітора, доби             DS2438
    EDF_STAMPD,      // мітка події в добах (0x32)           DS2438
    EDF_MONCCA,      // лічильник заряду монітора,  мА·год  DS2438
    EDF_MONDCA,      // лічильник розряду монітора, мА·год  DS2438
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
        case EDF_STAMPD:  return "Мітка останньої події";
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
        case EDF_MONCCA:  case EDF_MONDCA:  return "мА·год";
        case EDF_RATED:   case EDF_ICA:     return "мА·год";
        case EDF_HEALTH:  return "%";
        case EDF_ETM: case EDF_STAMPD: return "діб";
        default:          return "";
    }
}
inline int editFieldType(int i) {
    return (i == EDF_MFG || i == EDF_USE || i == EDF_LASTCHG || i == EDF_LASTREC)
           ? EDT_DATE : EDT_NUM;
}
// ── ЩО ЦЕ ЗА ЗНАЧЕННЯ Й ЩО БУДЕ, ЯКЩО ЙОГО ЗМІНИТИ ───────────────────────
//  ⚑ ОДИН ТЕКСТ НА ВСІ ТРИ КЛІЄНТИ, і причина та сама, що з назвами й межами:
//  своя копія пояснень у кожному клієнті розійшлася б на першій же правці —
//  у цьому проєкті так уже було з іменем силового ключа й описом зарядного
//  заліза.
//
//  Пояснення відповідає на ДВА питання, і саме в такому порядку: що це число
//  означає — і що станеться, коли його змінити. Друге важливіше: редактор
//  пише в пам'ять живого пакета, і «я не знав, що воно на це впливає» тут
//  коштує дорожче за будь-яку незручність.
inline const char *editFieldHelp(int i) {
    switch (i) {
        case EDF_MFG:
            return "Дата виготовлення пакета — опора всього блока: решта дат зберігається "
                   "ЗМІЩЕННЯМ у добах від неї. Посунете її — і всі похідні дати "
                   "перерахуються, щоб лишитись на місці за календарем.";
        case EDF_USE:
            return "Коли пакет увімкнули вперше. Нуль означає «не вмикали» — це законне "
                   "значення, а не порожнеча. Не може бути ані раніше за виготовлення, "
                   "ані пізніше за наробіток монітора.";
        case EDF_LASTCHG:
            return "Останній заряд. Станція звіряє його з наробітком: подія, пізніша за "
                   "весь строк роботи пакета, — саме той «побитий набір», який вона "
                   "лікує, повертаючи свої числа.";
        case EDF_LASTREC:
            return "Останнє кондиціювання (калібрувальний цикл на станції). Разом із "
                   "калібрувальними циклами показує, коли пакет востаннє вимірювали.";
        case EDF_CYCLES:
            return "Цикли IMPRES — те саме число, що фірмове ПЗ зве «Total IMPRES charge "
                   "cycles». Це СТЕЛЯ для решти лічильників: усі вони підуть за ним "
                   "униз, бо пакет не міг накопичити більше, ніж відпрацював.";
        case EDF_NONIMP:
            return "Скільки разів пакет заряджали звичайною (не-IMPRES) зарядкою. На "
                   "калібрування не впливає — це облік.";
        case EDF_CALCYC:
            return "Скільки разів станція проводила повне калібрування. Більшим за цикли "
                   "IMPRES бути не може.";
        case EDF_HISTCCA:
            return "Скільки заряду пакет прийняв за весь строк, у мА·год. Записано в "
                   "САМОМУ пакеті (DS2433) — і саме звідси станція повертає лічильники "
                   "після скидання, якщо тут лишити старе число.";
        case EDF_HISTDCA:
            return "Скільки заряду з пакета взяли за весь строк, у мА·год. Лежить поруч "
                   "із попереднім, у самому пакеті, і повертається так само.";
        case EDF_RATED:
            return "Паспортна ємність моделі. Від неї рахується знос, відсоток заряду й "
                   "струми розумного профілю — тобто змінивши її, ви зміните ПОКАЗАННЯ "
                   "всього іншого, не чіпаючи самих банок.";
        case EDF_HEALTH:
            return "Знос: скільки відсотків паспортної ємності пакет ще тримає. Пишеться "
                   "не як є, а через потенційну ємність (CTS) — саме її читає станція.";
        case EDF_ETM:
            return "Наробіток монітора: скільки діб пакет узагалі працював. Це стеля для "
                   "всіх дат подій — опустите наробіток, і пізніші за нього події "
                   "поїдуть за ним, бо статись раніше за власне життя вони не могли.";
        case EDF_STAMPD:
            return "Мітка останньої події в добах. Четверте місце, де пакет тримав "
                   "наробіток: станція бачить мітку, пізнішу за наробіток, і підтягує "
                   "наробіток до неї — саме так число й поверталось.";
        case EDF_MONCCA:
            return "Скільки заряду прийняв пакет за версією МОНІТОРА (DS2438), у мА·год. "
                   "Те саме, що «наробіток у пакеті: заряд», але з іншого чипа — і "
                   "станція звіряє їх між собою.";
        case EDF_MONDCA:
            return "Скільки заряду з пакета взято за версією монітора, у мА·год. Пара до "
                   "попереднього.";
        case EDF_ICA:
            return "Паливомір: скільки мА·год у пакеті ЗАРАЗ. Це поточний залишок, а не "
                   "історія; станція уточнить його сама при першому ж заряді.";
        default:
            return "";
    }
}

// ── ЧОТИРИ ЛІЧИЛЬНИКИ, ЯКІ ЖИВУТЬ У СИРИХ ОДИНИЦЯХ ЧИПА ───────────────────
//  У пам'яті вони лежать не в мА·год, а в «сирих» одиницях накопичувача:
//  мА·год = 15.625 × сире / шунт. Шунт у кожного пакета СВІЙ (DS2438[56..57]),
//  тож одна й та сама сира одиниця — це від 339 до 790 мА·год залежно від
//  екземпляра (виміряно на 62 моніторах корпусу з 65).
//
//  Показувати таке число людині означає показувати ніщо: «40» не каже нічого
//  ні про заряд, ні про знос. Тому редактор працює в мА·год, а переклад робить
//  сам — за шунтом ЦЬОГО пакета.
//
//  ⚑ ЦІНА — ГРУБИЙ КРОК, І ЙОГО НЕ СХОВАТИ. Одиниця чипа велика, тож не кожне
//  число в мА·год записуване: введене значення притягується до найближчого
//  можливого, і про це кажуть уголос (див. editPlanSet). Мовчазне округлення
//  тут було б гірше за сирі одиниці: людина побачила б, що записала не те, і
//  не зрозуміла б чому.
inline bool editFieldIsCca(int i) {
    return i == EDF_HISTCCA || i == EDF_HISTDCA || i == EDF_MONCCA || i == EDF_MONDCA;
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
        // ⚑ БЕЗ ШУНТА ПОЛЕ НЕДОСТУПНЕ, А НЕ «СИРЕ». Перекласти сирі одиниці в
        //  мА·год можна лише знаючи шунт ЦЬОГО пакета; підставити спільну
        //  константу означало б показати неправильне число з упевненим
        //  виглядом — у цьому проєкті вже було, і саме на шунті (сімейство
        //  4409 занижувалось майже вдвічі). Мовчки відкотитись назад у сирі
        //  одиниці теж не можна: тоді те саме поле міряється то в одному, то
        //  в іншому. Шунт не читається в 3 моніторах корпусу з 65.
        bool haveRs = (p.rsOhm > 0.0f);
        for (int i : { EDF_HISTCCA, EDF_HISTDCA }) {
            p.f[i].avail = haveHist && haveRs;
            p.f[i].lo = 0;
            p.f[i].hi = haveRs ? impresCcaMahFromRaw(0xFFFF, p.rsOhm) : 0;
        }
        if (haveHist && haveRs) {
            p.f[EDF_HISTCCA].cur = impresCcaMahFromRaw(hC, p.rsOhm);
            p.f[EDF_HISTDCA].cur = impresCcaMahFromRaw(hD, p.rsOhm);
        }

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
        // ⚑ ТРЕТЯ МІТКА ПОДІЇ — ПОКАЗУЄМО ЇЇ ОЧИМА. Саме через неї наробіток
        //  повертався після бездоганного скидання, і доти побачити її можна
        //  було лише в hex-дампі. Тепер вона в тому самому списку: зняли
        //  значення до зарядки й після — і видно, звідки взялось число.
        p.f[EDF_STAMPD].avail = true;
        p.f[EDF_STAMPD].cur   = impresStampDays(d38);
        p.f[EDF_STAMPD].lo = 0; p.f[EDF_STAMPD].hi = (long)(RP_ETM_MAX / 86400UL);
        // Лічильники монітора — теж у мА·год, і з тією самою умовою про шунт.
        bool rsOk = (p.rsOhm > 0.0f);
        p.f[EDF_MONCCA].avail = rsOk;
        p.f[EDF_MONCCA].cur = rsOk ? impresCcaMahFromRaw(impresCca(d38), p.rsOhm) : -1;
        p.f[EDF_MONCCA].lo = 0; p.f[EDF_MONCCA].hi = rsOk ? impresCcaMahFromRaw(0xFFFF, p.rsOhm) : 0;
        p.f[EDF_MONDCA].avail = rsOk;
        p.f[EDF_MONDCA].cur = rsOk ? impresCcaMahFromRaw(impresDca(d38), p.rsOhm) : -1;
        p.f[EDF_MONDCA].lo = 0; p.f[EDF_MONDCA].hi = rsOk ? impresCcaMahFromRaw(0xFFFF, p.rsOhm) : 0;
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
    // ── ⚑ ГРУБИЙ КРОК ЛІЧИЛЬНИКІВ — УГОЛОС ────────────────────────────────
    //  Одиниця накопичувача велика (339…790 мА·год залежно від шунта), тож
    //  записуване не кожне число. Притягуємо до найближчого можливого — і
    //  КАЖЕМО про це. Мовчазне округлення тут гірше за сирі одиниці: людина
    //  побачила б у полі не те, що ввела, і не мала б жодної підказки чому.
    if (editFieldIsCca(i) && p.rsOhm > 0.0f) {
        long snapped = impresCcaMahFromRaw(impresCcaRawFromMah(v, p.rsOhm), p.rsOhm);
        if (snapped != v) {
            char why[80];
            long step = impresCcaMahFromRaw(1, p.rsOhm);
            snprintf(why, sizeof(why), "крок лічильника %ld мА·год", step);
            editFixNote(p, i, v, snapped, why);
            v = snapped;
        }
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

// ── ДОПОМІЖНЕ ДЛЯ ДВОХ ПРАВИЛ НИЖЧЕ ───────────────────────────────────────
//  Дата, віддалена від заданої на стільки діб. Потрібна там, де стелю задає
//  наробіток: він міряється В ДОБАХ, а поля — датами, і порівнювати їх можна
//  лише привівши до одного.
inline long editDatePlusDays(long ymd, long days) {
    if (ymd <= 0) return 0;
    long z = impresBmsToDays((int)(ymd / 10000), (int)((ymd / 100) % 100),
                             (int)(ymd % 100)) + days;
    int y, m, d;
    impresBmsFromDays(z, &y, &m, &d);
    return restoreDateNum(y, m, d);
}

// Стеля лічильників заряду/розряду В МА·ГОД, що відповідає заданій кількості
// циклів: стільки заряду пакет міг накопичити за N повних циклів. Повертає −1,
// коли рахувати нічим (немає паспортної ємності) — тоді правило мовчить, а не
// вигадує число.
//
//  ⚑ РАХУЄТЬСЯ В ТИХ САМИХ ОДИНИЦЯХ, У ЯКИХ ЖИВЕ ПОЛЕ. Доти стеля була в сирих
//  одиницях, бо в них були й поля; перевівши поля в мА·год і лишивши стелю
//  сирою, ми дістали б звірку числа з числом іншої розмірності — тобто
//  запобіжник, який спрацьовує будь-як, тільки не за призначенням.
inline long editCcaCapMah(const EditPlan &p, long cycles) {
    long rated = editPlanEff(p, EDF_RATED);
    if (rated <= 0) rated = p.ratedEff;
    if (cycles < 0 || rated <= 0) return -1;
    return cycles * rated;
}

// ── РОДИНА ЛІЧИЛЬНИКІВ І СТЕЛЯ КОЖНОГО ────────────────────────────────────
//  ⚑ ОДИН ПЕРЕЛІК НА ЗВІРКУ Й НА ПОЧИНКУ. Спершу тут стояло п'ять майже
//  однакових умов у звірці й такий самий цикл у починці. Звірка «від
//  протилежного» це й упіймала: прибери одну умову — і НЕ ВПАДЕ НІЧОГО, бо
//  сусідні чотири однаково доводять набір до починки, а та однаково притискає
//  всіх п'ятьох. Умова, яку неможливо повалити, нічого не стереже; тому
//  перелік один, і ламається він цілком.
//
//  Стеля у кожного своя за одиницями, але питання одне: «скільки цей пакет
//  відпрацював». Лічильники заряду/розряду міряються в сирих одиницях, тож
//  їхня стеля — те саме число циклів, переведене через ємність і шунт.
//  Повертає кількість членів; 0 означає «цикли невідомі, правило мовчить».
inline int editCycleFamily(const EditPlan &p, int *idx, long *cap) {
    long cyc = p.f[EDF_CYCLES].avail ? editPlanEff(p, EDF_CYCLES) : -1;
    if (cyc < 0) return 0;
    int n = 0;
    idx[n] = EDF_CALCYC; cap[n++] = cyc;
    long raw = editCcaCapMah(p, cyc);
    if (raw >= 0) {
        idx[n] = EDF_HISTCCA; cap[n++] = raw;
        idx[n] = EDF_HISTDCA; cap[n++] = raw;
        idx[n] = EDF_MONCCA;  cap[n++] = raw;
        idx[n] = EDF_MONDCA;  cap[n++] = raw;
    }
    return n;
}
#define EDF_FAMILY_MAX 5

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
    // ⚑ ПОДІЯ НЕ МОГЛА СТАТИСЬ ПІЗНІШЕ, НІЖ ПАКЕТ ПРАЦЮВАВ. Мітка, більша за
    //  наробіток, — це рівно той стан, який станція «лікує», підтягуючи
    //  наробіток до мітки. Саме так наробіток і повертався.
    //
    //  ⚑ АЛЕ ПИТАЄМО ЛИШЕ ПРО ЗАДАНУ РУКАМИ МІТКУ. Якщо людина просто знизила
    //  наробіток, мітку підтягне сам запис (impresSetEtm), і скаржитись тут
    //  означало б вимагати правити поле, яке й так виправиться. Умова, що
    //  забороняє законну дію, — гірша за її відсутність.
    if (p.f[EDF_STAMPD].want >= 0 && p.f[EDF_ETM].avail &&
        p.f[EDF_STAMPD].want > editPlanEff(p, EDF_ETM))
        return no("мітка події пізніша за наробіток — станція підтягне наробіток до неї");

    // ⚑ ТЕ САМЕ ПРАВИЛО, АЛЕ ПРО ДАТИ, А НЕ ПРО МІТКУ. Дати живуть ЗМІЩЕННЯМ
    //  від виготовлення, тож посунути виготовлення НАЗАД означає збільшити
    //  кожне зміщення, не торкнувшись наробітку. Виміряно на корпусі: у всіх
    //  41 дампі кожне зміщення менше за наробіток — стану «подія пізніша за
    //  весь строк роботи» немає в жодному живому пакеті, і саме його станція
    //  «лікує», повертаючи власні числа. Опора тут — наробіток: його задавала
    //  людина, а зміщення поїхало саме собою, слідом за датою виготовлення.
    if (p.f[EDF_ETM].avail && mfg > 0) {
        long lim = editDatePlusDays(mfg, editPlanEff(p, EDF_ETM));
        if (use > lim) return no("перший запуск пізніший за наробіток монітора");
        if (chg > lim) return no("останній заряд пізніший за наробіток монітора");
        if (rec > lim) return no("останнє кондиціювання пізніше за наробіток монітора");
    }

    // ⚑ ЦИКЛИ IMPRES — СТЕЛЯ ВСІЄЇ РОДИНИ ЛІЧИЛЬНИКІВ. Питання «скільки пакет
    //  відпрацював» одне, а відповідають на нього шість полів; правку, що
    //  опускає одне з них і лишає решту, станція бачить як побитий набір і
    //  відновлює своє. Співвідношення виміряне, а не припущене: у всіх 41
    //  дампі корпусу кожен брат не більший за цикли з гістограми.
    int fi[EDF_FAMILY_MAX]; long fc[EDF_FAMILY_MAX];
    int fn = editCycleFamily(p, fi, fc);
    for (int k = 0; k < fn; k++) {
        if (!p.f[fi[k]].avail || editPlanEff(p, fi[k]) <= fc[k]) continue;
        char m[112];
        snprintf(m, sizeof(m), "%s: більше, ніж дозволяють цикли IMPRES",
                 editFieldName(fi[k]));
        return no(m);
    }
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
        // Мітка пізніша за наробіток: опора тут — наробіток, бо саме його
        // людина й задавала; мітку підтягуємо до нього.
        if (p.f[EDF_STAMPD].want >= 0 && p.f[EDF_ETM].avail &&
            p.f[EDF_STAMPD].want > editPlanEff(p, EDF_ETM))
            did |= pull(EDF_STAMPD, editPlanEff(p, EDF_ETM), "пізніше за наробіток");
        // Дата події пізніша за наробіток — підтягуємо ДАТУ, а не наробіток:
        // підняти наробіток означало б зробити руками рівно те, заради чого
        // все й написано (див. 3.30 і 3.53 — станція лікує саме так).
        if (p.f[EDF_ETM].avail && mfg > 0) {
            long lim = editDatePlusDays(mfg, editPlanEff(p, EDF_ETM));
            if (use > lim) did |= pull(EDF_USE,     lim, "пізніше за наробіток монітора");
            if (chg > lim) did |= pull(EDF_LASTCHG, lim, "пізніше за наробіток монітора");
            if (rec > lim) did |= pull(EDF_LASTREC, lim, "пізніше за наробіток монітора");
        }
        // Брати лічильника циклів — під ту саму стелю, тим самим переліком,
        // що його читає звірка вище.
        int fi[EDF_FAMILY_MAX]; long fc[EDF_FAMILY_MAX];
        int fn = editCycleFamily(p, fi, fc);
        for (int k = 0; k < fn; k++)
            if (p.f[fi[k]].avail && editPlanEff(p, fi[k]) > fc[k])
                did |= pull(fi[k], fc[k], "більше за цикли IMPRES");
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
            // Назад у сирі одиниці — за шунтом цього пакета.
            uint16_t cca = impresCcaRawFromMah(editPlanEff(p, EDF_HISTCCA), p.rsOhm);
            uint16_t dca = impresCcaRawFromMah(editPlanEff(p, EDF_HISTDCA), p.rsOhm);
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
        // ⚑ ЦИКЛИ ЖИВУТЬ І ТУТ — ПРОСТО ЇХ НЕ ВИДНО. Крім гістограми, яку
        //  щойно переписав impresCyclesWrite(), ту саму історію тримають
        //  внутрішній лічильник, реверти й дозаряди в блоці CYCLE. Полями
        //  редактора вони не є (людині немає чого про них вирішувати), але
        //  лишити їх вищими за нові цикли означає віддати станції готовий
        //  «побитий набір» — рівно те, з чого почалась ця правка. Тому блок
        //  перешифровується й тоді, коли жодного зашифрованого ПОЛЯ не
        //  чіпали: змінились цикли — родина йде за стелею.
        bool capNeeded = false;
        ImpresCryptFields cf;
        if (p.haveKey) {
            impresCryptRead(d33, p.k1, p.k2, &cf);
            ImpresCryptFields probe = cf;
            capNeeded = impresCryptCapByCycles(&probe, editPlanEff(p, EDF_CYCLES));
        }
        if (p.haveKey && (anyCrypt || capNeeded)) {
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
            // Стеля родини — ОСТАННЬОЮ дією над набором: інакше її перекрила б
            // правка calCycles, яка йде вище. Правило одне на весь проєкт і
            // живе поруч із полями (impres_crypt.h), а не тут.
            if (impresCryptCapByCycles(&cf, editPlanEff(p, EDF_CYCLES)) && wrote33)
                *wrote33 = true;
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
        // ⚑ ПІСЛЯ НАРОБІТКУ, А НЕ ДО НЬОГО. impresSetEtm() сама притискає мітку
        //  до нового наробітку; якби ми писали мітку першою, ця правка тут же
        //  й затерлась би. Задане людиною значення має бути останнім словом —
        //  а щоб воно не створило «подію в майбутньому», його ще до запису
        //  притискає звірка набору.
        if (want(EDF_STAMPD)) {
            uint16_t v = (uint16_t)p.f[EDF_STAMPD].want;
            d38[IMPRES_38_STAMPD_AT]     = (uint8_t)(v & 0xFF);
            d38[IMPRES_38_STAMPD_AT + 1] = (uint8_t)(v >> 8);
            did(EDF_STAMPD, true);
        }
        if (want(EDF_MONCCA)) {
            uint16_t v = impresCcaRawFromMah(p.f[EDF_MONCCA].want, p.rsOhm);
            d38[60] = (uint8_t)(v & 0xFF); d38[61] = (uint8_t)(v >> 8);
            did(EDF_MONCCA, true);
        }
        if (want(EDF_MONDCA)) {
            uint16_t v = impresCcaRawFromMah(p.f[EDF_MONDCA].want, p.rsOhm);
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
