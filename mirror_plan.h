#pragma once
// ===========================================================================
//  mirror_plan.h — СИНХРОНІЗАЦІЯ ДЗЕРКАЛА DS2438 -> DS2433 З ПРАВКОЮ
// ===========================================================================
//  Прохання власника: «є дані з 2438, які дублюються в 2433. Потрібна функція
//  синхронізації цих даних з 2438, а також правка даних перед синхронізацією».
//
//  Що дублюється. Рівно 26 байтів: DS2433[0x01..0x1A] ≡ DS2438[0x18..0x31].
//  Це ідентичність пакета — те, за чим рація й станція його впізнають. Копія в
//  моніторі не випадкова: саме завдяки їй пакет зі стертим DS2433 ще можна
//  відновити, і саме це робить зарядна станція WPLN4226A, коли переписує ці
//  байти назад у чип пам'яті.
//
//  ⚑ ЧОМУ НЕ ВИСТАЧАЛО ТОГО, ЩО ВЖЕ БУЛО. Синхронізація в проєкті є
//  (impresSyncMirror / «добудова заголовка»), але вона СЛІПА: копіює всі 26
//  байтів і виправляє суму. Користувач не бачить ні що саме зміниться, ні чи
//  правильне те, що прийде з монітора. А монітор буває чужий — саме так
//  виглядає більшість несправних пакетів у dumps/. Сліпе копіювання в такому
//  разі не лікує, а закріплює чужу ідентичність.
//
//  Тому тут — ПЛАН: побайтова різниця «що зараз / що прийде», прапорець на
//  кожен байт і окремо розібране поле, яке ми вміємо назвати (паспортна
//  ємність). Спершу дивимось, за потреби правимо, і лише потім пишемо.
//
//  Тут чиста арифметика без вводу-виводу — щоб її перевіряв хостовий тест.
// ===========================================================================

//  ⚑ ДРУГА ПОЛОВИНА ЗАДАЧІ — І ВОНА ГОЛОВНА. Крім 26 однакових БАЙТІВ, чипи
//  ведуть ОДНІ Й ТІ САМІ ФАКТИ різними числами, і саме вони розходяться в
//  натурі. Приклад із поля (PMNN4409B, дамп власника):
//
//      наробіток ETM у DS2438 …… 6397 діб (17 років)
//      а пакет за DS2433 …………… виготовлений 2026-08-03, уперше ввімкнений
//                                2026-08-18, тобто йому 15 діб
//      заряджено (CCA) …………… 31 еквівалентний цикл
//      розряджено (DCA) ………… 37 циклів
//      а лічильники Motorola ……… 1 цикл IMPRES + 5 не-IMPRES
//
//  Байтове дзеркало в цьому пакеті збігається ПОВНІСТЮ — тобто «синхронізація»
//  зі старим змістом сказала б «усе гаразд» і не полагодила нічого. Рація ж
//  показує «дату першого користування» як «сьогодні мінус ETM», тож бачить
//  2009 рік і рахує знос від нього.
//
//  Тому нижче — другий, ЗНАЧЕННЄВИЙ бік плану: ті самі факти в обох чипах,
//  побачені поруч, із правкою перед записом. Напрямок тут ЗВОРОТНИЙ до
//  байтового: ідентичність ми беремо з монітора, а лічильники — з ПАКЕТА,
//  бо саме DS2433 веде рація й станція, а DS2438 — це накопичувач, який
//  міняється разом із платою монітора.

#include <stdint.h>
#include <string.h>
#include "impres_format.h"
#include "impres_audit.h"   // impresEtmForeign(): чи монітор узагалі від цього пакета
#include "impres_bms.h"     // impresCcaRawFromMah(): цикли -> сирі одиниці CCA/DCA
#include "impres_crypt.h"   // impresCyclesWrite(): лічильники циклів у DS2433

// Зсув паспортної ємності ВСЕРЕДИНІ дзеркала. DS2433[0x008] — а дзеркало
// починається з 0x001, отже сьомий байт.
#define MIRROR_RATED_IDX (IMPRES_RATED_BYTE - IMPRES_MIRROR_D33_AT)

// ── ФАКТИ, ЯКІ ВЕДУТЬ ОБИДВА ЧИПИ ───────────────────────────────────────────
//  Рядків свідомо три, і кожен — не «поле», а ТВЕРДЖЕННЯ ПРО ПАКЕТ, яке кожен
//  чип записує по-своєму:
//
//   MVAL_ETM  скільки пакет пропрацював. DS2438 має лічильник секунд; DS2433
//             має дату першого вмикання, і рація рахує «сьогодні − ETM».
//   MVAL_CCA  скільки в пакет залито заряду. DS2438 накопичує мА·год; DS2433
//             веде лічильник циклів (IMPRES + не-IMPRES).
//   MVAL_DCA  скільки з пакета взято. Симетрично до CCA; окремого лічильника
//             розряду в DS2433 немає, тож пакетна сторона в нього та сама —
//             повний цикл це заряд І розряд.
//
//  ⚑ ДВА ОСТАННІ РЯДКИ ПИШУТЬ У ДРУГИЙ ЧИП. Лічильники циклів веде сам пакет
//  (DS2433), і правити їх теж треба тут — інакше виходить половина роботи:
//  монітор виправили, а рація й далі читає старе число з пакета.
//
//   MVAL_CYC     цикли IMPRES — гістограма доданого заряду в DS2433. Її ж
//                станція переписує з CCA монітора, тож «інша сторона» для
//                цього рядка — саме еквівалентні цикли монітора.
//   MVAL_NONIMP  цикли не-IMPRES — окремий лічильник у DS2433. Відповідника в
//                моніторі немає взагалі, тож рядок працює лише від руки.
//
//  Одиниці — ЛЮДСЬКІ (доби, цикли), а не сирі: правити людина буде саме їх, і
//  перерахунок у чипові одиниці мусить бути в одному місці — тут.
enum { MVAL_ETM = 0, MVAL_CCA, MVAL_DCA, MVAL_CYC, MVAL_NONIMP, MVAL_COUNT };

// Куди піде рядок: у монітор чи в пам'ять пакета.
inline bool mirrorValToMon(int i) { return i < MVAL_CYC; }

struct MirrorVal {
    bool avail;      // обидві сторони прочитались — є що порівнювати
    bool packKnown;  // пакетна сторона відома (без неї нема що пропонувати)
    bool take;       // цей рядок переносимо
    long pack;       // що каже DS2433, у людських одиницях
    long mon;        // що зараз у DS2438, у тих самих одиницях
    long user;       // вписано руками; -1 — не вписано
    long out;        // що буде в моніторі після запису
    long outRaw;     // …воно ж у чипових одиницях (секунди / сирі CCA)
};

struct MirrorPlan {
    bool     have33, have38;
    bool     srcUsable;                    // у моніторі справді є дзеркало
    bool     mirrorOkNow;                  // чипи вже збігаються
    bool     hdrSumOkNow;                  // сума заголовка ціла зараз
    uint8_t  now[IMPRES_MIRROR_LEN];       // що лежить у DS2433 зараз
    uint8_t  src[IMPRES_MIRROR_LEN];       // що каже DS2438
    uint8_t  out[IMPRES_MIRROR_LEN];       // що БУДЕ записано (з урахуванням правок)
    bool     diff[IMPRES_MIRROR_LEN];      // де now і src розходяться
    bool     take[IMPRES_MIRROR_LEN];      // які байти справді переносимо
    int      diffCount;
    int      ratedNow, ratedSrc;           // мА·год, 0 — байт не схожий на ємність
    int      ratedUser;                    // вписано вручну, 0 — ні

    // ── ЧИ ЦЕЙ МОНІТОР УЗАГАЛІ ВІД ЦЬОГО ПАКЕТА ──────────────────────────
    //  Найважливіше питання саме тут, а не на картці даних. Синхронізація
    //  переносить ідентичність З МОНІТОРА в пам'ять пакета; якщо монітор
    //  чужий, ми не полагодимо пакет, а припишемо йому чужу особу — і
    //  зробимо це «за планом», з галочками, тобто впевнено.
    //
    //  Ознака в нас уже є: наробіток ETM більший за вік пакета. Причин дві, і
    //  вони протилежні за наслідками — чужий монітор або цикл на станції
    //  (ЗП переписує ETM своїм числом), — тож вирішує людина, а не код. Наша
    //  справа — не дати натиснути «синхронізувати», не побачивши цього.
    bool     haveAge;                      // чи є з чим порівнювати (дата + годинник)
    long     etmDays, ageDays;             // наробіток і вік, доби
    bool     etmForeign;                   // наробіток більший за вік + допуск

    // ── ЗНАЧЕННЯ, ОБЛІКОВАНІ ДВІЧІ (див. шапку файла) ────────────────────
    MirrorVal val[MVAL_COUNT];
    bool      haveVals;                    // хоч один рядок є з чим порівняти
    int       ratedMah;                    // паспортна ємність пакета, мА·год
    float     rsense;                      // шунт монітора, Ом
    bool      rsFromChip;                  // шунт узято з чипа, а не з налаштувань
};

// Перерахунок «байт <-> мА·год» — той самий крок 25 мА·год, що й у решті
// проєкту (impresRatedFromDump). Окремо, бо тут працюємо з байтом дзеркала, а
// не з дампом цілком.
inline int mirrorRatedFromByte(uint8_t b) {
    int mah = (int)b * IMPRES_RATED_STEP;
    return (mah >= IMPRES_RATED_MIN_MAH && mah <= IMPRES_RATED_MAX_MAH) ? mah : 0;
}
inline uint8_t mirrorByteFromRated(int mah) {
    long b = ((long)mah + IMPRES_RATED_STEP / 2) / IMPRES_RATED_STEP;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint8_t)b;
}

// Перерахувати out[] за поточними прапорцями take[] і ручною ємністю.
//  Викликається після КОЖНОЇ зміни плану — щоб «що буде записано» ніколи не
//  розходилось із тим, що показано користувачу.
inline void mirrorPlanRecalc(MirrorPlan &p) {
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.out[i] = p.take[i] ? p.src[i] : p.now[i];
    // Ручна ємність — ПОВЕРХ усього: користувач вписав її саме тому, що ні
    // чипу, ні монітору тут не довіряє (типово після заміни банок).
    if (p.ratedUser > 0)
        p.out[MIRROR_RATED_IDX] = mirrorByteFromRated(p.ratedUser);
}

// Побудувати план. d38 може бути nullptr — тоді переносити нічого, але план
// усе одно показує, що зараз лежить у DS2433.
inline void mirrorPlanBuild(MirrorPlan &p, const uint8_t *d33, const uint8_t *d38) {
    memset(&p, 0, sizeof(p));
    // «Не вписано» — це −1, а не 0: нуль тут повноцінне значення («монітор як
    // новий»), і сплутати їх означало б мовчки писати нулі в чип.
    for (int i = 0; i < MVAL_COUNT; i++) p.val[i].user = -1;
    p.have33 = (d33 != nullptr);
    p.have38 = (d38 != nullptr);
    if (!p.have33) return;

    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.now[i] = d33[IMPRES_MIRROR_D33_AT + i];
    p.hdrSumOkNow = impresHeaderOk(d33);
    p.ratedNow    = mirrorRatedFromByte(p.now[MIRROR_RATED_IDX]);

    if (p.have38) {
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
            p.src[i] = d38[IMPRES_MIRROR_D38_AT + i];
        p.srcUsable   = impresMirrorUsable(d38);
        p.mirrorOkNow = impresMirrorOk(d33, d38);
        p.ratedSrc    = mirrorRatedFromByte(p.src[MIRROR_RATED_IDX]);
    }

    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
        p.diff[i] = p.have38 && (p.now[i] != p.src[i]);
        if (p.diff[i]) p.diffCount++;
        // ⚑ ТИПОВО БЕРЕМО ЛИШЕ ТЕ, ЩО СПРАВДІ РІЗНЕ, І ЛИШЕ З ПРИДАТНОГО
        //  ДЖЕРЕЛА. Умикати все підряд означало б «перезаписати ідентичність
        //  байтами монітора» — а монітор буває чужий. Однакові байти писати
        //  теж нема сенсу: це зайвий цикл запису EEPROM без жодної користі.
        p.take[i] = p.diff[i] && p.srcUsable;
    }
    mirrorPlanRecalc(p);
}

// Додати в план свідчення про наробіток. Числа рахує викликач: дату
// виготовлення треба спершу розшифрувати, а сьогоднішню взяти з годинника —
// того й того в чистому заголовку немає.
//  ⚑ «НЕМА З ЧИМ ПОРІВНЮВАТИ» — ЦЕ ВІД'ЄМНЕ ageDays, А НЕ НУЛЬ. Нуль діб —
//  законний вік пакета, якому щойно вписали сьогоднішню дату виготовлення, і
//  плутати його з «немає дати / годинник не заведений» не можна: саме через
//  цю плутанину найгрубіше розходження (тисячі діб у моніторі проти нуля в
//  пакеті) не помічалось узагалі.
inline void mirrorPlanSetEtm(MirrorPlan &p, long etmDays, long ageDays) {
    p.etmDays = etmDays;
    p.ageDays = ageDays;
    p.haveAge = ageDays >= 0;
    p.etmForeign = impresEtmForeign(etmDays, ageDays);
}

// ── ЗНАЧЕННЄВА ЧАСТИНА ПЛАНУ ────────────────────────────────────────────────

// Перерахунок «цикли <-> сирі одиниці CCA/DCA». Один еквівалентний цикл — це
// паспортна ємність пакета; далі мА·год перетворює impresCcaRawFromMah() за
// шунтом цього монітора.
//
//  ⚑ ОКРУГЛЕННЯ ПІДТЯГУЄМО ВГОРУ, доки зворотний перерахунок не дасть рівно те
//  саме число циклів. Показ рахує цикли з відкиданням дробової частини
//  (impresBmsParse), тож просте округлення інколи давало б на цикл менше, ніж
//  вписали, — і людина тиснула б «синхронізувати» знову й знову, щоразу
//  бачачи ту саму розбіжність.
inline long mirrorCyclesFromRaw(uint16_t raw, int ratedMah, float rsOhm) {
    if (ratedMah <= 0 || rsOhm <= 0.0f) return -1;
    return impresCcaMahFromRaw(raw, rsOhm) / ratedMah;
}
inline uint16_t mirrorRawFromCycles(long cycles, int ratedMah, float rsOhm) {
    if (cycles <= 0 || ratedMah <= 0 || rsOhm <= 0.0f) return 0;
    uint16_t raw = impresCcaRawFromMah(cycles * (long)ratedMah, rsOhm);
    for (int guard = 0; guard < 64 && raw < 65535; guard++) {
        if (mirrorCyclesFromRaw(raw, ratedMah, rsOhm) >= cycles) break;
        raw++;
    }
    return raw;
}

// Перерахувати «що буде в моніторі» для значеннєвих рядків.
inline void mirrorValsRecalc(MirrorPlan &p) {
    p.haveVals = false;
    for (int i = 0; i < MVAL_COUNT; i++) {
        MirrorVal &v = p.val[i];
        if (v.avail) p.haveVals = true;
        // Вписане руками сильніше за пакет: його вписують саме тоді, коли
        // жодному з чипів не вірять (типово після заміни банок або монітора).
        // ⚑ ДЛЯ РЯДКІВ, ЩО ПИШУТЬ У ПАКЕТ, ДЖЕРЕЛО — МОНІТОР, а не навпаки:
        //  цикли IMPRES станція однаково перерахує з CCA, тож «як має бути» тут
        //  каже саме монітор. Напрямок протилежний — і колонки теж міняються
        //  місцями, тому «зараз» для них це pack, а «звідки брати» — mon.
        bool toMon = mirrorValToMon(i);
        long src   = toMon ? v.pack : v.mon;
        long now   = toMon ? v.mon  : v.pack;
        long want  = (v.user >= 0) ? v.user : src;
        v.out    = v.take ? want : now;
        v.outRaw = (i == MVAL_ETM) ? v.out * 86400L
                 : (i == MVAL_CCA || i == MVAL_DCA)
                       ? (long)mirrorRawFromCycles(v.out, p.ratedMah, p.rsense)
                       : v.out;                    // лічильники пакета — як є
    }
}

// Вхідні числа для значеннєвої частини. Рахує їх викликач: щоб дістати дату
// першого вмикання, треба ключ і розшифрування, а щоб дату «сьогодні» —
// годинник; ні того, ні того в чистій арифметиці немає.
struct MirrorValIn {
    bool  have33, have38;         // що саме прочитано
    bool  packEtmKnown;           // у DS2433 є читана дата першого вмикання
    long  packEtmDays;            // …і скільки діб від неї до сьогодні
    bool  packCycKnown;           // гістограма циклів ціла
    long  packCycles;             // цикли IMPRES + не-IMPRES
    long  packCycImpres;          // з них IMPRES (гістограма) — свій рядок
    long  packNonImpres;          // і не-IMPRES — теж свій
    long  monEtmDays;             // наробіток монітора, доби
    uint16_t monCca, monDca;      // сирі лічильники монітора
    int   ratedMah;               // паспортна ємність пакета
    float rsense;                 // шунт монітора, Ом
    bool  rsFromChip;
};

// Чи розходяться сторони. Для наробітку — з допуском в одну добу: пакетна
// сторона рахується цілими добами від дати, а лічильник монітора йде
// безперервно, тож розбіжність «пів доби» тут — не розбіжність, а спосіб
// запису. Без допуску картка вимагала б синхронізації щодня.
#define MVAL_ETM_SLACK_D 1
inline bool mirrorValDiffers(const MirrorVal &v, int i) {
    if (!v.avail) return false;
    long d = v.pack - v.mon;
    if (d < 0) d = -d;
    return d > ((i == MVAL_ETM) ? MVAL_ETM_SLACK_D : 0);
}

inline void mirrorPlanSetVals(MirrorPlan &p, const MirrorValIn &in) {
    // ⚑ ПЕРШЕ ЗАПОВНЕННЯ ЧИ ПОВТОРНЕ. Числа перераховуються й тоді, коли
    //  клієнт приніс дату (див. web_server.h), тобто вже ПІСЛЯ того, як людина
    //  розставила галочки. Тому типові значення ставимо лише один раз, а далі
    //  чіпаємо самі числа — вибір лишається людський.
    bool first = !p.haveVals;

    p.ratedMah   = in.ratedMah;
    p.rsense     = in.rsense;
    p.rsFromChip = in.rsFromChip;

    long monCyc  = mirrorCyclesFromRaw(in.monCca, in.ratedMah, in.rsense);
    long monDcyc = mirrorCyclesFromRaw(in.monDca, in.ratedMah, in.rsense);

    // Рядок доступний, коли є куди писати (монітор прочитано) І є що
    // пропонувати — або з пакета, або вписане людиною. Друге не рідкість:
    // «монітор бреше, а скільки насправді — я знаю».
    MirrorVal &e = p.val[MVAL_ETM];
    e.packKnown = in.packEtmKnown;
    e.pack      = in.packEtmKnown ? in.packEtmDays : 0;
    e.mon       = in.monEtmDays;
    e.avail     = in.have38 && (in.packEtmKnown || e.user >= 0);

    MirrorVal &c = p.val[MVAL_CCA];
    c.packKnown = in.packCycKnown;
    c.pack      = in.packCycKnown ? in.packCycles : 0;
    c.mon       = monCyc < 0 ? 0 : monCyc;
    c.avail     = in.have38 && monCyc >= 0 && (in.packCycKnown || c.user >= 0);

    MirrorVal &d = p.val[MVAL_DCA];
    d.packKnown = in.packCycKnown;
    d.pack      = in.packCycKnown ? in.packCycles : 0;
    d.mon       = monDcyc < 0 ? 0 : monDcyc;
    d.avail     = in.have38 && monDcyc >= 0 && (in.packCycKnown || d.user >= 0);

    // ── рядки, що пишуть у ПАКЕТ ─────────────────────────────────────────
    //  Тут «зараз» — у DS2433, а «звідки брати» — монітор: станція однаково
    //  перерахує цикли IMPRES із CCA, тож саме його число і є ціллю.
    MirrorVal &y = p.val[MVAL_CYC];
    y.packKnown = in.packCycKnown;
    y.pack      = in.packCycImpres;
    y.mon       = monCyc < 0 ? 0 : monCyc;
    y.avail     = in.have33 && in.packCycKnown && (monCyc >= 0 || y.user >= 0);

    // Не-IMPRES відповідника в моніторі не має взагалі — лише руками.
    MirrorVal &z = p.val[MVAL_NONIMP];
    z.packKnown = in.packCycKnown;
    z.pack      = in.packNonImpres;
    z.mon       = 0;
    z.avail     = in.have33 && in.packCycKnown && z.user >= 0;

    for (int i = 0; i < MVAL_COUNT; i++) {
        MirrorVal &v = p.val[i];
        if (first) {
            v.user = -1;
            // Типово беремо те саме, що й на байтовому боці: лише те, що
            // СПРАВДІ розходиться. Однакове переписувати нема сенсу, а
            // вмикати все підряд — означало б тихо переписати монітор.
            v.take = mirrorValDiffers(v, i);
        } else {
            v.take = v.take && v.avail;
        }
        if (!v.avail) v.take = false;
    }
    mirrorValsRecalc(p);            // тут же перераховується й haveVals
}

// Увімкнути/вимкнути рядок. Недоступний увімкнути не можна: брати нема звідки.
inline void mirrorValTake(MirrorPlan &p, int i, bool on) {
    if (i < 0 || i >= MVAL_COUNT) return;
    p.val[i].take = on && p.val[i].avail;
    mirrorValsRecalc(p);
}

// Вписати число руками (доби або цикли); -1 — скасувати ручне значення.
//  Повертає збережене число — щоб клієнт показав саме те, що піде в чип.
//  ⚑ Ручне число ДОЗВОЛЯЄ ввімкнути рядок навіть тоді, коли пакетна сторона
//  невідома: «монітор бреше, а скільки насправді — я знаю» — це законний і
//  типовий випадок (пакет із заміненими банками, історія відома майстрові).
inline long mirrorValSetUser(MirrorPlan &p, int i, long v) {
    if (i < 0 || i >= MVAL_COUNT) return -1;
    MirrorVal &r = p.val[i];
    if (v < 0) {
        r.user = -1;
        if (!r.packKnown) r.take = false;   // пропала єдина підстава
        // Рядок лишається доступним лише коли є що пропонувати з пакета.
        r.avail = r.avail && r.packKnown;
    } else {
        // Стеля свідома: 20 років наробітку й 9999 циклів — уже не показник
        // пакета, а сміття (ті самі межі, що в майстрі відновлення).
        long lim = (i == MVAL_ETM) ? 20L * 365L : 9999L;
        if (v > lim) v = lim;
        r.user  = v;
        r.avail = r.avail || p.have38;      // є що писати — рядок оживає
        r.take  = r.avail;
    }
    mirrorValsRecalc(p);
    return r.user;
}

// Скільки значеннєвих рядків справді зміниться. «Зараз» у рядка залежить від
// того, який чип він пише, — див. mirrorValsRecalc.
inline int mirrorValChanges(const MirrorPlan &p, bool onlyMon = false, bool onlyPack = false) {
    int n = 0;
    for (int i = 0; i < MVAL_COUNT; i++) {
        bool toMon = mirrorValToMon(i);
        if (onlyMon && !toMon) continue;
        if (onlyPack && toMon) continue;
        long now = toMon ? p.val[i].mon : p.val[i].pack;
        if (p.val[i].take && p.val[i].out != now) n++;
    }
    return n;
}

// Записати значеннєву частину в дамп монітора. Повертає кількість змінених
// полів. Байти беремо ті самі, що й решта проєкту: ETM 0x08..0x0B (LE),
// CCA 0x3C..0x3D, DCA 0x3E..0x3F.
inline int mirrorPlanApply38(const MirrorPlan &p, uint8_t *d38) {
    if (!d38) return 0;
    int n = 0;
    if (p.val[MVAL_ETM].take) {
        uint32_t s = (uint32_t)p.val[MVAL_ETM].outRaw;
        // ⚑ Через impresSetEtm, а не байтами: разом із наробітком мусять
        //  підтягнутись мітки подій, інакше станція поверне старе число з них.
        uint32_t was  = impresEtm(d38);
        uint32_t was1 = impres38U32(d38, IMPRES_38_STAMP1_AT);
        uint32_t was2 = impres38U32(d38, IMPRES_38_STAMP2_AT);
        impresSetEtm(d38, s);
        if (was != s || was1 != impres38U32(d38, IMPRES_38_STAMP1_AT)
                     || was2 != impres38U32(d38, IMPRES_38_STAMP2_AT)) n++;
    }
    const int at[2] = { 0x3C, 0x3E };
    for (int k = 0; k < 2; k++) {
        const MirrorVal &v = p.val[MVAL_CCA + k];
        if (!v.take) continue;
        uint8_t lo = (uint8_t)(v.outRaw & 0xFF), hi = (uint8_t)((v.outRaw >> 8) & 0xFF);
        if (d38[at[k]] != lo || d38[at[k] + 1] != hi) {
            d38[at[k]] = lo; d38[at[k] + 1] = hi; n++;
        }
    }
    return n;
}

// Записати значеннєві рядки, що належать ПАКЕТУ (лічильники циклів у DS2433).
//  Повертає кількість змінених рядків. Обидва записи самі лагодять суму свого
//  блока; заголовка вони не торкаються.
inline int mirrorPlanApply33Vals(const MirrorPlan &p, uint8_t *d33) {
    if (!d33) return 0;
    int n = 0;
    if (p.val[MVAL_CYC].take) {
        uint16_t a = impresBmsVector(d33, BMS_V_ADDED);
        long was = impresBmsCyclesFromHist(d33, a);
        if (was != p.val[MVAL_CYC].out && impresCyclesWrite(d33, (int)p.val[MVAL_CYC].out)) n++;
    }
    if (p.val[MVAL_NONIMP].take) {
        uint16_t a = impresBmsVector(d33, BMS_V_NONSMART);
        long was = (a == BMS_INVALID) ? -1 : (long)bmsBE(d33, a + 7);
        if (was != p.val[MVAL_NONIMP].out &&
            impresNonImpresWrite(d33, (int)p.val[MVAL_NONIMP].out)) n++;
    }
    return n;
}

// Увімкнути/вимкнути перенесення всіх РІЗНИХ байтів одразу.
inline void mirrorPlanTakeAll(MirrorPlan &p, bool on) {
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.take[i] = on && p.diff[i] && p.srcUsable;
    mirrorPlanRecalc(p);
}

// Один байт — руками. Поза межами дзеркала мовчки нічого не робимо.
//
//  ⚑ ТУТ, НА ВІДМІНУ ВІД «ВЗЯТИ ВСЕ», ОДНАКОВІ БАЙТИ ТЕЖ МОЖНА ВІДМІТИТИ.
//  Типовий вибір свідомо звужений до різних байтів — не варто без потреби
//  переписувати EEPROM. Але коли людина вказує байт ПАЛЬЦЕМ, вона робить це
//  навмисно, і забороняти їй нічого. Єдина умова лишається — щоб було звідки
//  брати: без придатного джерела «перенести» нема чого.
inline void mirrorPlanTakeOne(MirrorPlan &p, int idx, bool on) {
    if (idx < 0 || idx >= IMPRES_MIRROR_LEN) return;
    p.take[idx] = on && p.have38 && p.srcUsable;
    mirrorPlanRecalc(p);
}

// Ручна паспортна ємність, мА·год. 0 — скасувати ручне значення.
//  Повертає ФАКТИЧНО збережене число (після округлення до кроку 25) — щоб
//  клієнт показав те, що буде записано, а не те, що набрали.
inline int mirrorPlanSetRated(MirrorPlan &p, int mah) {
    if (mah <= 0) { p.ratedUser = 0; mirrorPlanRecalc(p); return 0; }
    if (mah < IMPRES_RATED_MIN_MAH) mah = IMPRES_RATED_MIN_MAH;
    if (mah > IMPRES_RATED_MAX_MAH) mah = IMPRES_RATED_MAX_MAH;
    p.ratedUser = mirrorRatedFromByte(mirrorByteFromRated(mah));
    mirrorPlanRecalc(p);
    return p.ratedUser;
}

// Скільки байтів справді зміниться, якщо застосувати план ЗАРАЗ.
inline int mirrorPlanChanges(const MirrorPlan &p) {
    int n = 0;
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) if (p.out[i] != p.now[i]) n++;
    return n;
}

// Застосувати план до дампа DS2433. Повертає кількість змінених байтів.
//  Суму заголовка виправляємо ЗАВЖДИ, навіть коли жоден байт не змінився:
//  саме заради неї цю операцію часто й запускають після зарядної станції —
//  вона переписує дзеркало, але суму не чіпає, і заголовок лишається
//  структурно невалідним при правильних даних.
inline int mirrorPlanApply(const MirrorPlan &p, uint8_t *d33) {
    if (!d33 || !p.have33) return 0;
    int n = 0;
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
        if (d33[IMPRES_MIRROR_D33_AT + i] != p.out[i]) {
            d33[IMPRES_MIRROR_D33_AT + i] = p.out[i];
            n++;
        }
    }
    impresFixHeader(d33);
    return n;
}
