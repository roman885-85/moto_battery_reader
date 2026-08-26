// ===========================================================================
//  edit_plan_check — РУЧНИЙ РЕДАКТОР: ЧИТАЄ ТЕ, ЩО Є, І НЕ ПУСКАЄ НЕМОЖЛИВЕ
// ===========================================================================
//  Редактор пише В САМ ПАКЕТ, полями, які до нього правились лише планами.
//  Ціна помилки тут — зіпсований акумулятор, тому перевіряємо не «функція
//  повертає true», а РЕЗУЛЬТАТ на справжніх дампах: прочитали → змінили →
//  прочитали ще раз і звірили.
//
//  ⚑ ГОЛОВНЕ ТУТ — НЕ ЗАПИС, А ВІДМОВА. Дати в пакеті зберігаються зміщенням
//  від дати виготовлення, і суперечливий набір («вмикали, але не заряджали»,
//  «запуск раніший за виготовлення») — це рівно те, що станція потім «лікує»,
//  повертаючи свої числа. Редактор, який дозволяє такий набір скласти,
//  працював би проти всієї решти проєкту.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#include "edit_plan.h"

// Сума заголовка DS2433 — та сама, що її рахує прошивка (web_server.h): байти
// 0x00..0x1F мають давати ≡0x41. Паспортна ємність лежить у 0x008, тобто
// ВСЕРЕДИНІ цього діапазону, — і правка без перерахунку суми віддала б рації
// пакет, який вона вважає побитим.
static bool headerChecksumOk(const uint8_t *d) {
    int s = 0;
    for (int i = 0; i <= 0x1F; i++) s += d[i];
    return (s & 0xFF) == 0x41;
}

static int fails = 0;
static void check(bool ok, const char *m) {
    printf(ok ? "   ок    %s\n" : "   ЗБІЙ  %s\n", m);
    if (!ok) fails++;
}

static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t r = fread(b, 1, n, f);
    fclose(f);
    return r == n;
}

static void collect(std::vector<std::string> &out) {
    DIR *d = opendir("dumps");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string dir = "dumps/" + n + "/files";
        DIR *f = opendir(dir.c_str());
        if (!f) continue;
        struct dirent *g;
        while ((g = readdir(f))) {
            std::string m = g->d_name;
            if (m.find("2433") == std::string::npos) continue;
            out.push_back(dir + "/" + m);
        }
        closedir(f);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

// ROM-ів чипів у корпусі немає, а без ключа зашифровані поля не читаються.
// Ключ підбираємо зі змісту й складаємо ROM, який його дає: розбір бере
// key1/key2 саме з rom[1] і rom[6].
static bool romFor(const uint8_t *d33, const uint8_t *d38, uint8_t *rom8) {
    ImpresBms b;
    if (!impresBmsParse(d33, d38, nullptr, 0.0f, &b) || !b.ok) return false;
    if (impresBmsFindKey(d33, d38, &b) != 1) return false;
    memset(rom8, 0, 8);
    rom8[0] = 0x23; rom8[1] = b.key1; rom8[6] = b.key2;
    return true;
}

int main() {
    std::vector<std::string> all;
    collect(all);

    printf("1) список полів описаний повністю\n");
    {
        bool named = true, sane = true;
        for (int i = 0; i < EDF_COUNT; i++) {
            if (!editFieldName(i)[0]) named = false;
            int c = editFieldChip(i);
            if (c != 33 && c != 38) sane = false;
        }
        check(named, "кожне поле має назву — клієнти своїх не вигадують");
        // ⚑ І ПОЯСНЕННЯ — ТЕЖ У КОЖНОГО. Редактор пише в пам'ять живого
        //  пакета; поле без пояснення означає, що людина міняє число, не
        //  знаючи ні що воно, ні що з цього вийде. Порожній рядок тут — не
        //  «поки не написали», а дірка, яку видно лише на живому приладі.
        bool helped = true, helpFull = true;
        for (int i = 0; i < EDF_COUNT; i++) {
            const char *h = editFieldHelp(i);
            if (!h[0]) helped = false;
            //  Коротке «а це лічильник» користі не дає — вимагаємо саме
            //  пояснення: що означає І що буде, якщо змінити.
            if (strlen(h) < 60) helpFull = false;
        }
        check(helped, "…і пояснення: що це значення означає");
        check(helpFull, "…причому пояснення, а не два слова");
        // ⚑ ОДИНИЦІ ЛІЧИЛЬНИКІВ — САМЕ МА·ГОД. Сира одиниця чипа не каже
        //  людині нічого: «40» — це і не заряд, і не знос, і не цикли. Саме з
        //  цієї скарги правка й почалась, тож перевіряємо не «одиниця є», а
        //  яка саме.
        bool mah = true, raw = false;
        for (int i = 0; i < EDF_COUNT; i++) {
            if (!editFieldIsCca(i)) continue;
            if (strcmp(editFieldUnit(i), "мА·год") != 0) mah = false;
            if (strstr(editFieldUnit(i), "сир")) raw = true;
        }
        check(mah,  "лічильники заряду/розряду міряються в мА·год");
        check(!raw, "…і сирих одиниць чипа людині більше не показують");
        check(sane,  "…і кожне знає, у який чип пише");
        // ⚑ Межа між чипами — за НОМЕРОМ поля, тож порядок у переліку не
        //  косметика: перемішавши їх, ми мовчки відправили б правку не туди.
        bool split = true;
        for (int i = 0; i < EDF_COUNT; i++)
            if ((i < EDF_ETM) != (editFieldChip(i) == 33)) split = false;
        check(split, "поля DS2433 і DS2438 не перемішані в переліку");
        check(editFieldType(EDF_MFG) == EDT_DATE && editFieldType(EDF_CYCLES) == EDT_NUM,
              "дати позначені датами, числа — числами");
    }

    printf("\n2) на живому корпусі читається те, що там справді лежить\n");
    {
        int seen = 0, withKey = 0, withHist = 0, withEtm = 0;
        for (auto &p33 : all) {
            uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE], rom8[8];
            if (!load(p33.c_str(), a33, DUMP_SIZE)) continue;
            std::string p38 = p33;
            size_t q = p38.find("2433");
            if (q != std::string::npos) p38.replace(q, 4, "2438");
            bool has38 = load(p38.c_str(), a38, DS2438_MEM_SIZE);
            bool haveRom = romFor(a33, has38 ? a38 : nullptr, rom8);
            EditPlan p;
            editPlanBuild(p, a33, has38 ? a38 : nullptr, haveRom ? rom8 : nullptr, 20260824);
            seen++;
            if (p.haveKey) withKey++;
            if (p.f[EDF_HISTCCA].avail) withHist++;
            if (has38 && p.f[EDF_ETM].avail && p.f[EDF_ETM].cur >= 0) withEtm++;
            // Прочитане мусить збігтися з тим, що каже штатний розбір — але
            // ТЕПЕР У МА·ГОД: редактор більше не показує сирих одиниць чипа,
            // бо «40» не каже людині нічого. Переклад робиться за шунтом ЦЬОГО
            // пакета, тож і звіряти треба з ним, а не з сирим числом.
            if (has38 && p.f[EDF_MONCCA].avail &&
                p.f[EDF_MONCCA].cur != impresCcaMahFromRaw(impresCca(a38), p.rsOhm)) {
                check(false, "лічильник заряду монітора прочитано не так, як його читає решта");
                break;
            }
        }
        printf("   дампів %d: із ключем %d, з наробітком у пакеті %d, з монітором %d\n",
               seen, withKey, withHist, withEtm);
        check(seen >= 40, "корпус зчитався");
        // Без цих трьох рядків розділи нижче доводили б порожнечу.
        check(withKey  > 20, "зашифровані поля справді читаються — є на чому перевіряти дати");
        check(withHist > 20, "наробіток у пакеті теж читається");
        check(withEtm  > 20, "монітор теж");
    }

    // Робочий пакет для решти розділів: беремо перший, у якого читається все.
    uint8_t w33[DUMP_SIZE], w38[DS2438_MEM_SIZE], wrom[8];
    bool haveW = false;
    for (auto &p33 : all) {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE], rom8[8];
        if (!load(p33.c_str(), a33, DUMP_SIZE)) continue;
        std::string p38 = p33;
        size_t q = p38.find("2433");
        if (q != std::string::npos) p38.replace(q, 4, "2438");
        if (!load(p38.c_str(), a38, DS2438_MEM_SIZE)) continue;
        if (!romFor(a33, a38, rom8)) continue;
        EditPlan p;
        editPlanBuild(p, a33, a38, rom8, 20260824);
        if (!p.haveKey || !p.f[EDF_MFG].avail || !p.f[EDF_HISTCCA].avail ||
            !p.f[EDF_CYCLES].avail || p.f[EDF_MFG].cur <= 0) continue;
        memcpy(w33, a33, DUMP_SIZE);
        memcpy(w38, a38, DS2438_MEM_SIZE);
        memcpy(wrom, rom8, 8);
        haveW = true;
        printf("\n   робочий пакет: %s\n", p33.c_str() + 6);
        break;
    }
    if (!haveW) {
        printf("\n   ЗБІЙ  у корпусі немає пакета, на якому читається все — перевірки нижче порожні\n");
        printf("\nЄ ПОМИЛКИ (помилок: %d)\n", fails + 1);
        return 1;
    }

    printf("\n3) записали — і прочитали назад те саме\n");
    {
        struct Try { int f; long v; const char *nm; };
        Try tries[] = {
            { EDF_CYCLES,  123,      "цикли IMPRES" },
            { EDF_NONIMP,  7,        "цикли не-IMPRES" },
            // ⚑ ЛІЧИЛЬНИКИ ЗАРЯДУ/РОЗРЯДУ — ЗНАЧЕННЯ ЗАВІДОМО НИЖЧІ ЗА ПОТОЧНІ.
            //  Раніше тут стояли 4321/1234/5555/4444 — числа, узяті «щоб було
            //  видно», і вони перевищували стелю родини (цикли IMPRES) у
            //  десятки разів. Тобто перевірка вимагала записати стан, якого
            //  немає в жодному з 41 живого пакета корпусу, — і сама ж
            //  доводила б, що редактор такий стан пропускає.
            { EDF_HISTCCA, 0,        "наробіток у пакеті: заряд" },
            { EDF_HISTDCA, 0,        "наробіток у пакеті: розряд" },
            { EDF_RATED,   2500,     "паспортна ємність" },
            { EDF_CALCYC,  9,        "калібрувальні цикли" },
            { EDF_ETM,     42,       "наробіток монітора" },
            { EDF_MONCCA,  0,        "лічильник заряду монітора" },
            { EDF_MONDCA,  0,        "лічильник розряду монітора" },
        };
        for (auto &t : tries) {
            uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
            memcpy(b33, w33, DUMP_SIZE);
            memcpy(b38, w38, DS2438_MEM_SIZE);
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            if (!p.f[t.f].avail) { printf("   —     %s: у цьому пакеті немає\n", t.nm); continue; }
            // ⚑ ЛІЧИЛЬНИКИ ЖИВУТЬ У МА·ГОД, АЛЕ КРОК У НИХ ГРУБИЙ (сотні
            //  мА·год, і в кожного пакета свій). Тому число для них береться
            //  НЕ з голови, а з самої сітки: інакше перевірка вимагала б
            //  записати значення, якого чип не вміє, і падала б на чесній
            //  роботі притягування.
            if (editFieldIsCca(t.f)) t.v = impresCcaMahFromRaw(20, p.rsOhm);
            // ⚑ ЧЕРЕЗ ПОЧИНКУ, А НЕ ЧЕРЕЗ ГОЛУ ЗВІРКУ. Саме так кличуть
            //  редактор веб і USB (editPlanRepair -> editPlanApply): звірка
            //  лише КАЖЕ, що набір суперечливий, а рішення про очевидну
            //  правку ухвалює починка. Перевіряти шлях, якого немає в
            //  прошивці, означає стерегти не те.
            bool set = editPlanSet(p, t.f, t.v);
            bool cons = set && editPlanRepair(p);
            bool w3 = false, w8 = false;
            int done = cons ? editPlanApply(p, b33, b38, &w3, &w8) : 0;
            EditPlan q;
            editPlanBuild(q, b33, b38, wrom, 20260824);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: %ld -> %ld (прочитано %ld)",
                     t.nm, p.f[t.f].cur, t.v, q.f[t.f].cur);
            check(set && cons && done >= 1 && q.f[t.f].cur == t.v, msg);
            // Правка в один чип не сміє «забруднювати» другий.
            //
            // ⚑ НАРОБІТОК — ЗАКОННИЙ ВИНЯТОК, І САМЕ ЧЕРЕЗ НЬОГО ПРАВИЛО
            //  ЗВУЖЕНЕ, А НЕ СКАСОВАНЕ. Дати подій лежать у DS2433 ЗМІЩЕННЯМ
            //  від виготовлення, а стелю їм задає наробіток із DS2438. Опустив
            //  наробіток — і подія опинилась пізніше за весь строк роботи
            //  пакета; лишити її там означає віддати станції рівно той
            //  «побитий набір», з якого вона відновлює свої числа. Тож правка
            //  наробітку МУСИТЬ дійти й до другого чипа. Що саме вона там
            //  зробила — перевіряється окремо, нижче.
            if (t.f == EDF_ETM)
                check(w8, "…і дійшла до монітора");
            else
                check(w3 == (editFieldChip(t.f) == 33) && w8 == (editFieldChip(t.f) == 38),
                      "…і торкнулась рівно того чипа, якому належить");
            // ⚑ І НЕ СМІЄ ЗАЧЕПИТИ СУСІДА. Обидва наробітки лежать в ОДНОМУ
            //  блоці й пишуться однією дією — тож правка лише одного з них
            //  легко обнуляє другий, і помітити це можна лише отак, дивлячись
            //  на нього. Решта полів перевіряється тим самим правилом задарма.
            bool intact = true;
            for (int k = 0; k < EDF_COUNT; k++) {
                if (k == t.f || !p.f[k].avail) continue;
                // ⚑ МІТКА ПОДІЇ — ЗАКОННИЙ ВИНЯТОК, І ЛИШЕ ДЛЯ НАРОБІТКУ.
                //  Вона МУСИТЬ піти за ним униз: лишити її більшою означає
                //  створити «подію в майбутньому», з якої станція й відновлює
                //  старе число. Перевіряємо це окремо, нижче.
                if (k == EDF_STAMPD && t.f == EDF_ETM) continue;
                // ⚑ ДАТИ ПОДІЙ — ТОЙ САМИЙ ВИНЯТОК І З ТІЄЇ Ж ПРИЧИНИ.
                //  Наробіток задає їм стелю, тож опущений наробіток тягне їх
                //  за собою. Перевіряємо це окремо, нижче, — і саме як
                //  «опустило», а не як «щось змінило».
                if (t.f == EDF_ETM &&
                    (k == EDF_USE || k == EDF_LASTCHG || k == EDF_LASTREC)) continue;
                if (q.f[k].cur != p.f[k].cur) { intact = false; break; }
            }
            check(intact, "…і не зачепила жодного сусіднього значення");
            if (t.f == EDF_ETM) {
                check(q.f[EDF_STAMPD].cur <= t.v,
                      "…а мітку події підтягнуло за наробітком, а не лишило в майбутньому");
                // ⚑ І ДАТИ ТЕЖ — ІНАКШЕ СТАНЦІЯ ПІДТЯГНЕ НАРОБІТОК НАЗАД.
                //  Межа рахується в тих самих одиницях, у яких зберігається
                //  саме поле: доба від дати виготовлення.
                long lim = editDatePlusDays(q.f[EDF_MFG].cur, t.v);
                bool datesPulled = true;
                for (int k : { EDF_USE, EDF_LASTCHG, EDF_LASTREC })
                    if (q.f[k].avail && q.f[k].cur > lim) datesPulled = false;
                check(datesPulled,
                      "…і жодна дата події не лишилась пізнішою за наробіток");
            }
            // ⚑ ЗАГОЛОВОК DS2433 МУСИТЬ ЛИШИТИСЬ ЦІЛИМ. Паспортна ємність
            //  лежить у байті 0x008, тобто ВСЕРЕДИНІ заголовка: не перерахувати
            //  його суму — це віддати рації пакет, який вона вважає побитим.
            if (editFieldChip(t.f) == 33)
                check(headerChecksumOk(b33), "…а заголовок DS2433 лишився цілим");
        }
    }

    printf("\n4) знос ходить у ВІДСОТКАХ, а не сирим числом\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        if (!p.f[EDF_HEALTH].avail) {
            printf("   —     у цьому пакеті знос не рахується (немає шунта чи ємності)\n");
        } else {
            check(editPlanSet(p, EDF_HEALTH, 88) && !editPlanFixed(p),
                  "88 % приймається як є");
            bool w3 = false, w8 = false;
            editPlanApply(p, b33, b38, &w3, &w8);
            EditPlan q;
            editPlanBuild(q, b33, b38, wrom, 20260824);
            // Одиниця зносу — 0.4882 мВ·год/шунт, тож точного попадання
            // у відсоток немає; допуск — одна одиниця сирого CTS.
            char m[120];
            snprintf(m, sizeof(m), "прочитано %ld %% (просили 88)", q.f[EDF_HEALTH].cur);
            check(q.f[EDF_HEALTH].cur >= 86 && q.f[EDF_HEALTH].cur <= 90, m);
        }
    }

    printf("\n5) паливомір: мА·год туди й назад\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        long half = p.f[EDF_ICA].hi / 2;
        // Перебір притискається до стелі — і саме тому його перевіряємо на
        // ОКРЕМОМУ плані: інакше він переписав би half і решта розділу міряла б
        // уже не те, що просили.
        {
            EditPlan over;
            editPlanBuild(over, b33, b38, wrom, 20260824);
            check(editPlanSet(over, EDF_ICA, over.f[EDF_ICA].hi + 500) &&
                  over.f[EDF_ICA].want == over.f[EDF_ICA].hi,
                  "…а більше за паспортну притискається до паспортної");
        }
        check(editPlanSet(p, EDF_ICA, half), "половина ємності приймається");
        bool w3 = false, w8 = false;
        editPlanApply(p, b33, b38, &w3, &w8);
        EditPlan q;
        editPlanBuild(q, b33, b38, wrom, 20260824);
        long d = q.f[EDF_ICA].cur - half;
        if (d < 0) d = -d;
        char m[120];
        snprintf(m, sizeof(m), "прочитано %ld мА·год (просили %ld)", q.f[EDF_ICA].cur, half);
        // Паливомір — ОДИН байт на всю ємність, тож крок шкали великий.
        check(d <= (p.f[EDF_ICA].hi / 255 + 1), m);
    }

    printf("\n6) дати: зміщення перераховуються, а не «їдуть» за виготовленням\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p0;
        editPlanBuild(p0, b33, b38, wrom, 20260824);
        long useWas = p0.f[EDF_USE].cur, chgWas = p0.f[EDF_LASTCHG].cur;

        // Посуваємо ЛИШЕ дату виготовлення. Похідні дати мусять лишитись на
        // місці: вони живуть зміщенням, і без перерахунку поїхали б слідом.
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        long newMfg = 20240115;
        char why[128];
        if (useWas > 0 && newMfg > useWas) newMfg = useWas - 10000;   // не пізніше запуску
        check(editPlanSet(p, EDF_MFG, newMfg), "нову дату виготовлення прийнято");
        check(editPlanConsistent(p, why, sizeof(why)), "набір не суперечить сам собі");
        bool w3 = false, w8 = false;
        editPlanApply(p, b33, b38, &w3, &w8);
        EditPlan q;
        editPlanBuild(q, b33, b38, wrom, 20260824);
        char m[180];
        snprintf(m, sizeof(m), "виготовлення %ld -> %ld", p0.f[EDF_MFG].cur, q.f[EDF_MFG].cur);
        check(q.f[EDF_MFG].cur == newMfg, m);
        snprintf(m, sizeof(m), "перший запуск лишився %ld (був %ld)", q.f[EDF_USE].cur, useWas);
        check(q.f[EDF_USE].cur == useWas, m);
        snprintf(m, sizeof(m), "останній заряд лишився %ld (був %ld)", q.f[EDF_LASTCHG].cur, chgWas);
        check(q.f[EDF_LASTCHG].cur == chgWas, m);
    }

    printf("\n7) помилку введення ВИПРАВЛЯЮТЬ, а не відхиляють\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        char why[128];

        // а) число поза межами — притискаємо до найближчої межі й КАЖЕМО про це.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(editPlanSet(p, EDF_HEALTH, 140) && p.f[EDF_HEALTH].want == 100,
                  "140 % стає 100 %, а не відмовою");
            check(editPlanFixed(p) && strstr(p.fix, "межі поля"),
                  "…і виправлення названо: людина бачить, що сталось із її числом");
            EditPlan p2;
            editPlanBuild(p2, b33, b38, wrom, 20260824);
            // Стеля тепер у мА·год і залежить від шунта пакета: беремо її ж.
            long hiMah = p2.f[EDF_HISTCCA].hi;
            check(editPlanSet(p2, EDF_HISTCCA, hiMah * 4) && p2.f[EDF_HISTCCA].want == hiMah,
                  "перебір лічильника теж притискається до стелі");
        }
        // б) сміття замість дати — притискаємо покомпонентно.
        //    13-й місяць -> 12-й, 45-те число -> останнє в місяці.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(editPlanSet(p, EDF_MFG, 20251345) && p.f[EDF_MFG].want == 20251231,
                  "20251345 стає 2025-12-31");
            check(strstr(p.fix, "не схоже на дату"), "…із поясненням, чому виправили");
            EditPlan p2;
            editPlanBuild(p2, b33, b38, wrom, 20260824);
            // Лютий 2024-го високосний: 30-те стає 29-м, а не 28-м.
            check(editPlanSet(p2, EDF_MFG, 20240230) && p2.f[EDF_MFG].want == 20240229,
                  "30 лютого високосного року стає 29-м");
        }
        // в) дата в майбутньому — стає сьогоднішньою: пакета, який почав
        //    працювати завтра, не буває.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(editPlanSet(p, EDF_MFG, 20270101) && p.f[EDF_MFG].want == 20260824,
                  "дата наступного року стає сьогоднішньою");
            check(strstr(p.fix, "майбутньому"), "…і причина названа");
            // Без годинника «сьогодні» невідоме — і виправляти нема від чого.
            EditPlan p2;
            editPlanBuild(p2, b33, b38, wrom, 0);
            check(editPlanSet(p2, EDF_MFG, 20300101) && p2.f[EDF_MFG].want == 20300101,
                  "без годинника майбутнє не вигадуємо");
        }
        // г) дату виготовлення нулем не прибрати — лишаємо ту, що стоїть.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long was = p.f[EDF_MFG].cur;
            check(editPlanSet(p, EDF_MFG, 0) && p.f[EDF_MFG].want == was,
                  "нуль у даті виготовлення лишає ту, що вже стоїть");
            check(strstr(p.fix, "прибрати не можна"), "…і каже чому");
            // А в похідній даті нуль законний: «події не було».
            EditPlan p2;
            editPlanBuild(p2, b33, b38, wrom, 20260824);
            check(editPlanSet(p2, EDF_LASTREC, 0) && p2.f[EDF_LASTREC].want == 0 &&
                  !editPlanFixed(p2),
                  "…а «кондиціювання не було» — законний стан, і виправляти нема чого");
        }
        // ґ) правильне число не виправляють. Без цього рядка розділ доводив би,
        //    що виправляється ВСЕ підряд, — а це вже інша поломка.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(editPlanSet(p, EDF_HEALTH, 77) && p.f[EDF_HEALTH].want == 77 &&
                  !editPlanFixed(p), "нормальне число проходить без правок");
        }
        // д) виправити нема чого, коли писати нікуди — це не помилка введення.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(!editPlanSet(p, EDF_COUNT + 5, 1) && strstr(p.err, "такого поля немає"),
                  "неіснуюче поле — відмова, а не «виправлення»");
        }
    }

    printf("\n7б) суперечливий НАБІР теж лагодиться, а не відхиляється\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        char why[128];

        // ⚑ ПОЧИНКА МІНІМАЛЬНА Й ЗАВЖДИ В ОДИН БІК: пізнішу подію підтягуємо
        //  до ранішої. Дата виготовлення — опора блока (решта зберігається
        //  зміщенням ВІД неї), тож рухаємо не її.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long later = p.f[EDF_USE].cur + 10000;      // виготовлення пізніше за запуск
            editPlanSet(p, EDF_MFG, later);
            check(!editPlanConsistent(p, why, sizeof(why)), "набір справді суперечливий");
            check(editPlanRepair(p), "…і його полагоджено");
            check(editPlanConsistent(p, why, sizeof(why)), "…до несуперечливого стану");
            check(editPlanEff(p, EDF_USE) == later, "запуск підтягнуто до виготовлення");
            check(editPlanEff(p, EDF_MFG) == later, "…а саме виготовлення лишилось як просили");
            check(strstr(p.fix, "раніше за виготовлення"), "…і сказано, що саме зсунули");
        }
        // «Вмикали, але жодного разу не заряджали» — найменша правда, яка це
        // розв'язує: заряджали принаймні в день першого запуску.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long use = p.f[EDF_MFG].cur + 100;
            editPlanSet(p, EDF_USE, use);
            editPlanSet(p, EDF_LASTCHG, 0);
            check(editPlanRepair(p) && editPlanEff(p, EDF_LASTCHG) == use,
                  "«вмикали, але не заряджали» -> заряд у день запуску");
        }
        // ⚑ ЛАНЦЮЖОК ІЗ ДВОХ ПРОХОДІВ — І ЦЕ НЕ ВИГАДАНИЙ ВИПАДОК. Посуваємо
        //  виготовлення вперед і водночас кажемо «не заряджали жодного разу».
        //  Перший прохід підтягне запуск до виготовлення, а заряд — до
        //  СТАРОГО запуску (числа беруться на початку проходу). Після цього
        //  заряд знову раніший за виготовлення, і виправляє це лише ДРУГИЙ
        //  прохід. Одного тут не вистачає — саме тому цикл і стоїть.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long useWas = p.f[EDF_USE].cur;
            editPlanSet(p, EDF_MFG,     useWas + 10000);   // виготовлення ПІСЛЯ запуску
            editPlanSet(p, EDF_LASTCHG, 0);                // …і жодного заряду
            check(editPlanRepair(p) && editPlanConsistent(p, why, sizeof(why)),
                  "ланцюжок правок сходиться");
            check(editPlanEff(p, EDF_LASTCHG) == editPlanEff(p, EDF_USE) &&
                  editPlanEff(p, EDF_USE) == editPlanEff(p, EDF_MFG),
                  "…усі три дати зійшлись на даті виготовлення");
        }
        // Простіший випадок: заряд раніший за запуск.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long m = p.f[EDF_MFG].cur;
            editPlanSet(p, EDF_USE,     m + 10000);
            editPlanSet(p, EDF_LASTCHG, m + 5000);
            check(editPlanRepair(p) && editPlanConsistent(p, why, sizeof(why)),
                  "заряд раніший за запуск теж лагодиться");
            check(editPlanEff(p, EDF_LASTCHG) == editPlanEff(p, EDF_USE),
                  "…заряд підтягнуто до запуску");
        }
        // Несуперечливий набір починка НЕ чіпає — інакше вона правила б усе
        // підряд, і жодна перевірка вище нічого б не доводила.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            editPlanSet(p, EDF_CYCLES, 12);
            check(editPlanRepair(p), "нормальний набір проходить");
            check(!editPlanFixed(p) && editPlanCount(p, 0) == 1,
                  "…і жодне поле не зсунуто без потреби");
        }
    }

    printf("\n7в) мітка події: та сама пам'ять, з якої наробіток і повертався\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        char m[140];
        snprintf(m, sizeof(m), "мітка видима в списку: %ld діб", p.f[EDF_STAMPD].cur);
        check(p.f[EDF_STAMPD].avail && p.f[EDF_STAMPD].cur >= 0, m);
        check(editFieldChip(EDF_STAMPD) == 38, "…і належить моніторові");

        // Задати мітку ПІЗНІШУ за наробіток не можна — це рівно той стан,
        // який станція лікує поверненням старого числа. Виправляємо.
        EditPlan q;
        editPlanBuild(q, b33, b38, wrom, 20260824);
        editPlanSet(q, EDF_ETM,    100);
        editPlanSet(q, EDF_STAMPD, 4545);          // число власника
        char why[128];
        check(!editPlanConsistent(q, why, sizeof(why)) && strstr(why, "пізніша за наробіток"),
              "мітка пізніша за наробіток — суперечність");
        check(editPlanRepair(q) && editPlanEff(q, EDF_STAMPD) == 100,
              "…і мітку притиснуто до наробітку");

        // А задана коректно — доїжджає до чипа як є.
        EditPlan r;
        editPlanBuild(r, b33, b38, wrom, 20260824);
        editPlanSet(r, EDF_ETM,    500);
        editPlanSet(r, EDF_STAMPD, 480);
        bool w3 = false, w8 = false;
        editPlanApply(r, b33, b38, &w3, &w8);
        EditPlan s;
        editPlanBuild(s, b33, b38, wrom, 20260824);
        check(s.f[EDF_ETM].cur == 500 && s.f[EDF_STAMPD].cur == 480,
              "наробіток і мітка записались обидва, і мітка не затерлась");
    }

    printf("\n8) «не чіпати» справді не чіпає\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        bool w3 = false, w8 = false;
        int done = editPlanApply(p, b33, b38, &w3, &w8);
        check(done == 0 && !w3 && !w8, "порожній план нічого не пише");
        check(memcmp(b33, w33, DUMP_SIZE) == 0 && memcmp(b38, w38, DS2438_MEM_SIZE) == 0,
              "…і жоден байт не змінився");

        // Задали те саме значення, що вже стоїть, — теж нічого не пишемо.
        EditPlan p2;
        editPlanBuild(p2, b33, b38, wrom, 20260824);
        editPlanSet(p2, EDF_CYCLES, p2.f[EDF_CYCLES].cur);
        check(editPlanCount(p2, 0) == 0, "«те саме значення» не рахується за правку");
        w3 = w8 = false;
        check(editPlanApply(p2, b33, b38, &w3, &w8) == 0 && !w3,
              "…і в чип нічого не їде");
    }

    printf("\n9) кілька правок одним заходом\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        // ⚑ НАБІР ЗАВІДОМО НЕСУПЕРЕЧЛИВИЙ, І ЦЕ НЕ ПІДГОНКА ПІД ПЕРЕВІРКУ.
        //  Тут перевіряється, що П'ЯТЬ правок одним заходом доїжджають до
        //  обох чипів, а не те, як лагодиться суперечність, — для неї є
        //  окремі розділи. Раніше стояло 55 циклів при 777/666/888 сирих
        //  одиницях (це ~165 циклів заряду) і наробіток 11 діб при подіях на
        //  1500-й добі: набір, якого немає в жодному живому пакеті, і саме
        //  його перевірка вимагала записати.
        // Лічильники — у мА·год і на сітці чипа (див. розділ 3): число з голови
        // притягнулось би до сусіднього можливого, і перевірка «прочитали те
        // саме» падала б на чесній роботі притягування.
        long m1 = impresCcaMahFromRaw(30, p.rsOhm);
        long m2 = impresCcaMahFromRaw(24, p.rsOhm);
        long m3 = impresCcaMahFromRaw(28, p.rsOhm);
        editPlanSet(p, EDF_CYCLES,  555);
        editPlanSet(p, EDF_HISTCCA, m1);
        editPlanSet(p, EDF_HISTDCA, m2);
        editPlanSet(p, EDF_ETM,     2000);
        editPlanSet(p, EDF_MONCCA,  m3);
        check(editPlanCount(p, 33) == 3 && editPlanCount(p, 38) == 2,
              "план знає, скільки правок у кожен чип");
        char why[128];
        check(editPlanConsistent(p, why, sizeof(why)), "набір несуперечливий");
        bool w3 = false, w8 = false;
        int done = editPlanApply(p, b33, b38, &w3, &w8);
        EditPlan q;
        editPlanBuild(q, b33, b38, wrom, 20260824);
        check(done == 5 && w3 && w8, "усі п'ять застосовано, обидва чипи позначені");
        check(q.f[EDF_CYCLES].cur == 555 && q.f[EDF_HISTCCA].cur == m1 &&
              q.f[EDF_HISTDCA].cur == m2 && q.f[EDF_ETM].cur == 2000 &&
              q.f[EDF_MONCCA].cur == m3, "…і всі п'ять читаються назад");
        // ⚑ ОБИДВА НАРОБІТКИ ЛЕЖАТЬ В ОДНОМУ БЛОЦІ. Записати їх по черзі —
        //  це двічі перерахувати ту саму суму; сума мусить лишитись цілою.
        uint16_t hC = 0, hD = 0;
        check(impresBmsHistCounters(b33, &hC, &hD) && hC == 30 && hD == 24,
              "сума блока наробітку ціла — штатний читач бачить обидва числа");
    }

    printf("\n10) без ключа зашифровані поля не показуються й не пишуться\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        // Псуємо блок дат так, щоб ключ перестав підбиратись однозначно.
        uint16_t aDat = impresBmsVector(b33, BMS_V_DATE);
        if (aDat != BMS_INVALID) { b33[aDat] = 0xFF; b33[aDat + 1] = 0xFF; }
        EditPlan p;
        editPlanBuild(p, b33, b38, nullptr, 20260824);
        bool hidden = !p.f[EDF_MFG].avail || p.f[EDF_MFG].cur <= 0;
        check(hidden, "побитий блок дат не видає себе за прочитаний");
        check(!editPlanSet(p, EDF_MFG, 20250101) || p.f[EDF_MFG].avail,
              "…і задати дату в ньому не можна");
        // Незашифровані поля при цьому лишаються робочими: одне побите місце
        // не сміє вимикати редактор цілком.
        check(p.f[EDF_MONCCA].avail && p.f[EDF_ETM].avail,
              "монітор при цьому лишається доступним");
    }

    printf("\n11) один пакет — одна історія: правка не лишає братів позаду\n");
    {
        // ⚑ НА ВСЬОМУ КОРПУСІ, А НЕ НА ОДНОМУ ПАКЕТІ. Обидва правила тут
        //  ВИМІРЯНІ, а не придумані: спочатку доводимо, що співвідношення
        //  справді тримається в живих пакетах, і лише потім — що редактор
        //  його не ламає. Без першої половини друга доводила б, що ми
        //  стережемо власну вигадку.
        int packs = 0, holdsCyc = 0, holdsDate = 0;
        int cycOk = 0, cycTried = 0, datOk = 0, datTried = 0, keyOk = 0;
        for (auto &p33 : all) {
            uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE], rom8[8];
            if (!load(p33.c_str(), a33, DUMP_SIZE)) continue;
            std::string p38 = p33;
            size_t q = p38.find("2433");
            if (q != std::string::npos) p38.replace(q, 4, "2438");
            if (!load(p38.c_str(), a38, DS2438_MEM_SIZE)) continue;
            if (!romFor(a33, a38, rom8)) continue;
            ImpresBms b;
            impresBmsParse(a33, a38, rom8, 0.025f, &b);
            impresBmsDecrypt(a33, &b);
            if (!b.ok || b.cycles <= 0) continue;
            ImpresCryptFields f;
            impresCryptRead(a33, rom8[1], rom8[6], &f);
            uint32_t etm = ((uint32_t)a38[11] << 24) | ((uint32_t)a38[10] << 16) |
                           ((uint32_t)a38[9] << 8) | a38[8];
            long etmD = (long)(etm / 86400UL);
            if (etmD <= 0) continue;
            packs++;

            // (а) стеля родини тримається в самому пакеті
            if ((long)f.cyclesEnc <= b.cycles && (long)f.reverts <= b.cycles &&
                (long)f.topOffCycles <= b.cycles && (long)f.calCycles <= b.cycles &&
                b.ccaCycles <= b.cycles) holdsCyc++;
            // (б) жодна подія не пізніша за наробіток
            if ((long)f.dayInitialUse <= etmD && (long)f.dayLastCharge <= etmD &&
                (long)f.dayLastRecond <= etmD) holdsDate++;

            // (в) обнулили цикли — внутрішній лічильник пішов слідом
            {
                uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
                memcpy(b33, a33, DUMP_SIZE);
                memcpy(b38, a38, DS2438_MEM_SIZE);
                EditPlan p;
                editPlanBuild(p, b33, b38, rom8, 20260824);
                if (p.f[EDF_CYCLES].avail && editPlanSet(p, EDF_CYCLES, 0) &&
                    editPlanRepair(p)) {
                    cycTried++;
                    bool w3 = false, w8 = false;
                    editPlanApply(p, b33, b38, &w3, &w8);
                    ImpresCryptFields g;
                    impresCryptRead(b33, rom8[1], rom8[6], &g);
                    uint16_t nsC = 0, nsD = 0;
                    impresBmsHistCounters(b33, &nsC, &nsD);
                    if (g.cyclesEnc == 0 && g.reverts == 0 && g.topOffCycles == 0 &&
                        g.calCycles == 0 && nsC == 0 && nsD == 0) cycOk++;
                    // ⚑ І НАШ ВЛАСНИЙ ПІДБІР КЛЮЧА МУСИТЬ ЛИШИТИСЬ ОДНОЗНАЧНИМ.
                    //  Це найсуворіша перевірка з можливих: він відкидає зміст,
                    //  який САМ вважає неможливим, тож пакет, що після правки
                    //  перестав розбиратись, — це пакет, який ми щойно зіпсували.
                    ImpresBms nb;
                    impresBmsParse(b33, b38, nullptr, 0.025f, &nb);
                    if (impresBmsFindKey(b33, b38, &nb) == 1) keyOk++;
                }
            }

            // (г) посунули дату виготовлення на рік назад — жодна подія не
            //     опинилась пізніше за наробіток
            {
                uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
                memcpy(b33, a33, DUMP_SIZE);
                memcpy(b38, a38, DS2438_MEM_SIZE);
                EditPlan p;
                editPlanBuild(p, b33, b38, rom8, 20260824);
                long back = editDatePlusDays(p.f[EDF_MFG].cur, -365);
                if (p.f[EDF_MFG].avail && p.f[EDF_MFG].cur > 0 &&
                    editPlanSet(p, EDF_MFG, back) && editPlanRepair(p)) {
                    datTried++;
                    bool w3 = false, w8 = false;
                    editPlanApply(p, b33, b38, &w3, &w8);
                    ImpresCryptFields g;
                    impresCryptRead(b33, rom8[1], rom8[6], &g);
                    if ((long)g.dayInitialUse <= etmD && (long)g.dayLastCharge <= etmD &&
                        (long)g.dayLastRecond <= etmD) datOk++;
                }
            }
        }
        char m[200];
        snprintf(m, sizeof(m), "корпус: у %d із %d пакетів жоден брат не більший за цикли IMPRES",
                 holdsCyc, packs);
        check(packs > 0 && holdsCyc == packs, m);
        snprintf(m, sizeof(m), "корпус: у %d із %d жодна подія не пізніша за наробіток",
                 holdsDate, packs);
        check(packs > 0 && holdsDate == packs, m);
        snprintf(m, sizeof(m), "обнулили цикли — брати пішли слідом у %d із %d", cycOk, cycTried);
        check(cycTried > 0 && cycOk == cycTried, m);
        snprintf(m, sizeof(m), "…і підбір ключа лишився однозначним у %d із %d", keyOk, cycTried);
        check(cycTried > 0 && keyOk == cycTried, m);
        snprintf(m, sizeof(m), "дату виготовлення на рік назад — події лишились у межах наробітку "
                 "у %d із %d", datOk, datTried);
        check(datTried > 0 && datOk == datTried, m);
    }

    printf("\n11в) грубий крок лічильника не ховається, а називається\n");
    {
        // ⚑ ОДИНИЦЯ ЧИПА ВЕЛИКА (339…790 мА·год залежно від шунта), тож
        //  записуване не кожне число в мА·год. Притягування неминуче — але
        //  МОВЧАЗНЕ притягування гірше за сирі одиниці: людина побачила б у
        //  полі не те, що ввела, і не мала б жодної підказки чому.
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        if (!p.f[EDF_MONCCA].avail) {
            printf("   —     у цьому пакеті лічильник монітора не читається\n");
        } else {
            long step = impresCcaMahFromRaw(1, p.rsOhm);
            long onGrid = impresCcaMahFromRaw(10, p.rsOhm);
            char m[140];
            snprintf(m, sizeof(m), "крок лічильника — %ld мА·год, і він не нульовий", step);
            check(step > 0, m);

            // Значення НА сітці проходить недоторканим: правило мусить уміти
            // відповісти «ні», інакше воно чіпає геть усе.
            check(editPlanSet(p, EDF_MONCCA, onGrid) &&
                  p.f[EDF_MONCCA].want == onGrid && !editPlanFixed(p),
                  "значення, яке чип уміє записати, проходить недоторканим");

            // А значення МІЖ вузлами притягується — і про це сказано.
            EditPlan q;
            editPlanBuild(q, b33, b38, wrom, 20260824);
            long off = onGrid + step / 2;
            check(editPlanSet(q, EDF_MONCCA, off) && q.f[EDF_MONCCA].want != off,
                  "значення між вузлами притягується до можливого");
            check(editPlanFixed(q) && strstr(q.fix, "крок лічильника"),
                  "…і людині сказано, ЩО саме сталося з її числом");
        }
    }

    printf("\n11б) стеля родини називає ВИННОГО і вміє відповісти «ні»\n");
    {
        // ⚑ ПО ОДНОМУ ЧЛЕНУ РОДИНИ ЗА РАЗ. Перевірка, де завеликі відразу
        //  всі п'ятеро, не розрізнила б «правило працює» і «правило працює
        //  для сусіда»: починка однаково притисне всіх. Тому кожного
        //  піднімаємо поодинці, лишаючи решту в межах.
        //  ⚑ ЦИКЛИ СТАВИМО ВИСОКО НАВМИСНО. Перша редакція брала 5 циклів —
        //  і тоді стелю перевищували ще й ті лічильники, що вже лежали в
        //  пакеті, тож звірка чесно називала винним ПЕРШОГО з них, а не того,
        //  кого підняли. Перевірка падала й показала саме це.
        // ⚑ ЛІЧИЛЬНИКИ ТЕПЕР У МА·ГОД, І СТЕЛЯ В НИХ ТЕЖ. Число «завелике»
        //  беремо не з голови, а від самої стелі: 9000 мА·год колись було
        //  вчетверо більше за неї в сирих одиницях, а в мА·год це менше за
        //  один цикл — тобто перевірка мовчки перестала б щось перевіряти.
        struct { int f; long over; const char *nm; } who[] = {
            { EDF_CALCYC,  950,  "калібрувальні цикли" },
            { EDF_HISTCCA, 0,    "наробіток у пакеті: заряд" },
            { EDF_HISTDCA, 0,    "наробіток у пакеті: розряд" },
            { EDF_MONCCA,  0,    "лічильник заряду монітора" },
            { EDF_MONDCA,  0,    "лічильник розряду монітора" },
        };
        for (auto &t : who) {
            uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
            memcpy(b33, w33, DUMP_SIZE);
            memcpy(b38, w38, DS2438_MEM_SIZE);
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            if (!p.f[t.f].avail || !p.f[EDF_CYCLES].avail) {
                printf("   —     %s: у цьому пакеті немає\n", t.nm); continue;
            }
            editPlanSet(p, EDF_CYCLES, 900);
            long over = t.over ? t.over : editCcaCapMah(p, 900) * 2;
            editPlanSet(p, t.f, over);
            char why[128];
            char m[200];
            bool refused = !editPlanConsistent(p, why, sizeof(why));
            snprintf(m, sizeof(m), "%s завеликий — звірка каже саме про нього (%s)",
                     t.nm, why);
            check(refused && strstr(why, editFieldName(t.f)) != nullptr, m);
            bool ok = editPlanRepair(p);
            bool pulled = p.f[t.f].want >= 0 && p.f[t.f].want < over;
            snprintf(m, sizeof(m), "…і починка притиснула його до %ld", p.f[t.f].want);
            check(ok && pulled, m);
            check(editPlanFixed(p), "…і сказала про це людині, а не зробила мовчки");
        }
        // ⚑ І ГОЛОВНЕ — ПРАВИЛО МУСИТЬ УМІТИ ВІДПОВІСТИ «НІ». Набір, у якому
        //  всі брати в межах, не сміє нічого чіпати: інакше це не запобіжник,
        //  а мовчазний перезапис того, що людина щойно ввела.
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        EditPlan p;
        editPlanBuild(p, b33, b38, wrom, 20260824);
        editPlanSet(p, EDF_CYCLES, 900);
        editPlanSet(p, EDF_CALCYC, 7);
        char why[128];
        check(editPlanConsistent(p, why, sizeof(why)) && !editPlanFixed(p),
              "законний набір проходить недоторканим");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
