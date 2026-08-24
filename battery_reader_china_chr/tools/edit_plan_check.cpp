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
            // Прочитане мусить збігтися з тим, що каже штатний розбір.
            if (has38 && p.f[EDF_MONCCA].cur != impresCca(a38)) {
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
        struct { int f; long v; const char *nm; } tries[] = {
            { EDF_CYCLES,  123,      "цикли IMPRES" },
            { EDF_NONIMP,  7,        "цикли не-IMPRES" },
            { EDF_HISTCCA, 4321,     "наробіток у пакеті: заряд" },
            { EDF_HISTDCA, 1234,     "наробіток у пакеті: розряд" },
            { EDF_RATED,   2500,     "паспортна ємність" },
            { EDF_CALCYC,  9,        "калібрувальні цикли" },
            { EDF_ETM,     42,       "наробіток монітора" },
            { EDF_MONCCA,  5555,     "лічильник заряду монітора" },
            { EDF_MONDCA,  4444,     "лічильник розряду монітора" },
        };
        for (auto &t : tries) {
            uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
            memcpy(b33, w33, DUMP_SIZE);
            memcpy(b38, w38, DS2438_MEM_SIZE);
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            if (!p.f[t.f].avail) { printf("   —     %s: у цьому пакеті немає\n", t.nm); continue; }
            char why[128];
            bool set = editPlanSet(p, t.f, t.v);
            bool cons = editPlanConsistent(p, why, sizeof(why));
            bool w3 = false, w8 = false;
            int done = (set && cons) ? editPlanApply(p, b33, b38, &w3, &w8) : 0;
            EditPlan q;
            editPlanBuild(q, b33, b38, wrom, 20260824);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: %ld -> %ld (прочитано %ld)",
                     t.nm, p.f[t.f].cur, t.v, q.f[t.f].cur);
            check(set && cons && done == 1 && q.f[t.f].cur == t.v, msg);
            // Правка в один чип не сміє «забруднювати» другий.
            check(w3 == (editFieldChip(t.f) == 33) && w8 == (editFieldChip(t.f) == 38),
                  "…і торкнулась рівно того чипа, якому належить");
            // ⚑ І НЕ СМІЄ ЗАЧЕПИТИ СУСІДА. Обидва наробітки лежать в ОДНОМУ
            //  блоці й пишуться однією дією — тож правка лише одного з них
            //  легко обнуляє другий, і помітити це можна лише отак, дивлячись
            //  на нього. Решта полів перевіряється тим самим правилом задарма.
            bool intact = true;
            for (int k = 0; k < EDF_COUNT; k++) {
                if (k == t.f || !p.f[k].avail) continue;
                if (q.f[k].cur != p.f[k].cur) { intact = false; break; }
            }
            check(intact, "…і не зачепила жодного сусіднього значення");
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
            check(editPlanSet(p, EDF_HEALTH, 88), "88 % приймається");
            check(!editPlanSet(p, EDF_HEALTH, 140) && strstr(p.err, "поза межами"),
                  "…а 140 % — ні, і відмова каже чому");
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
        check(editPlanSet(p, EDF_ICA, half), "половина ємності приймається");
        check(!editPlanSet(p, EDF_ICA, p.f[EDF_ICA].hi + 500),
              "…а більше за паспортну — ні");
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

    printf("\n7) неможливий набір не складається\n");
    {
        uint8_t b33[DUMP_SIZE], b38[DS2438_MEM_SIZE];
        memcpy(b33, w33, DUMP_SIZE);
        memcpy(b38, w38, DS2438_MEM_SIZE);
        char why[128];

        // а) запуск раніший за виготовлення.
        //    ⚑ ПЕРЕВІРЯЄМО ЖИВИЙ ШЛЯХ, А НЕ ЗРУЧНИЙ. Задати ЗАПУСК раніше за
        //    виготовлення межа поля не дає й сама (lo = поточна дата
        //    виготовлення), тож така спроба до звірки набору просто не
        //    доходить — і перевірка доводила б порожнечу. Суперечність
        //    складається інакше: посунути ВИГОТОВЛЕННЯ вперед, лишивши запуск
        //    на місці. Тут межі поля безсилі: кожне число окремо законне.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(editPlanSet(p, EDF_USE, p.f[EDF_MFG].cur - 10000) == false,
                  "межа поля сама не пускає запуск раніше за виготовлення");
            EditPlan p2;
            editPlanBuild(p2, b33, b38, wrom, 20260824);
            long later = p2.f[EDF_USE].cur + 10000;      // на рік пізніше запуску
            check(editPlanSet(p2, EDF_MFG, later),
                  "…а посунути виготовлення вперед поле дозволяє");
            check(!editPlanConsistent(p2, why, sizeof(why)) &&
                  strstr(why, "раніший за виготовлення"),
                  "…і суперечність ловить уже звірка набору, називаючи причину");
        }
        // б) «вмикали, але жодного разу не заряджали» — той самий стан, що
        //    його ловить аудит (AUD_USE_BEFORE_CHG).
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            editPlanSet(p, EDF_USE, p.f[EDF_MFG].cur + 100);
            editPlanSet(p, EDF_LASTCHG, 0);
            check(!editPlanConsistent(p, why, sizeof(why)) && strstr(why, "не заряджали"),
                  "«вмикали, але не заряджали» — відмова");
        }
        // в) останній заряд раніший за перший запуск.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            long mfg = p.f[EDF_MFG].cur;
            editPlanSet(p, EDF_USE, 20250601);
            editPlanSet(p, EDF_LASTCHG, 20250301);
            (void)mfg;
            check(!editPlanConsistent(p, why, sizeof(why)) && strstr(why, "раніший за перший запуск"),
                  "заряд раніший за запуск — відмова");
        }
        // г) дата в майбутньому.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            // 2027 рік: правдоподібна дата (розбір приймає 2005..2035) —
            //  і саме тому вона й перевіряє МАЙБУТНЄ, а не «схоже на дату».
            check(!editPlanSet(p, EDF_MFG, 20270101) && strstr(p.err, "майбутньому"),
                  "дата в майбутньому не приймається");
        }
        // ґ) дату виготовлення не можна прибрати нулем, а похідну — можна:
        //    нуль там означає «події не було», і це законний стан.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(!editPlanSet(p, EDF_MFG, 0) && strstr(p.err, "не можна прибрати"),
                  "дату виготовлення нулем не прибрати");
            check(editPlanSet(p, EDF_LASTREC, 0), "…а «кондиціювання не було» — законний стан");
        }
        // д) сміття замість дати.
        {
            EditPlan p;
            editPlanBuild(p, b33, b38, wrom, 20260824);
            check(!editPlanSet(p, EDF_MFG, 20251345) && strstr(p.err, "не схоже на дату"),
                  "13-й місяць не приймається");
        }
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
        editPlanSet(p, EDF_CYCLES,  55);
        editPlanSet(p, EDF_HISTCCA, 777);
        editPlanSet(p, EDF_HISTDCA, 666);
        editPlanSet(p, EDF_ETM,     11);
        editPlanSet(p, EDF_MONCCA,  888);
        check(editPlanCount(p, 33) == 3 && editPlanCount(p, 38) == 2,
              "план знає, скільки правок у кожен чип");
        char why[128];
        check(editPlanConsistent(p, why, sizeof(why)), "набір несуперечливий");
        bool w3 = false, w8 = false;
        int done = editPlanApply(p, b33, b38, &w3, &w8);
        EditPlan q;
        editPlanBuild(q, b33, b38, wrom, 20260824);
        check(done == 5 && w3 && w8, "усі п'ять застосовано, обидва чипи позначені");
        check(q.f[EDF_CYCLES].cur == 55 && q.f[EDF_HISTCCA].cur == 777 &&
              q.f[EDF_HISTDCA].cur == 666 && q.f[EDF_ETM].cur == 11 &&
              q.f[EDF_MONCCA].cur == 888, "…і всі п'ять читаються назад");
        // ⚑ ОБИДВА НАРОБІТКИ ЛЕЖАТЬ В ОДНОМУ БЛОЦІ. Записати їх по черзі —
        //  це двічі перерахувати ту саму суму; сума мусить лишитись цілою.
        uint16_t hC = 0, hD = 0;
        check(impresBmsHistCounters(b33, &hC, &hD) && hC == 777 && hD == 666,
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

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
