// ===========================================================================
//  СИНХРОНІЗАЦІЯ ДЗЕРКАЛА DS2438 -> DS2433 З ПРАВКОЮ ПЕРЕД ЗАПИСОМ
//
//  Прохання власника: «є дані з 2438, які дублюються в 2433. Потрібна функція
//  синхронізації цих даних з 2438, а також правка даних перед синхронізацією».
//
//  Перевіряємо і саму логіку плану, і поведінку на ВСЬОМУ корпусі dumps/:
//  операція не сміє зробити гірше жодному пакету, який був цілий.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#define DS2438_RSENSE_OHM 0.025f
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
#define PROGMEM
#define memcpy_P memcpy
#include "settings.h"
#include "impres_format.h"
#include "mirror_plan.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t g = fread(b, 1, n, f); fclose(f); return g == n;
}
static void collect(std::vector<std::string> &o) {
    DIR *d = opendir("dumps"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        std::string dir = std::string("dumps/") + e->d_name + "/files";
        DIR *f = opendir(dir.c_str()); if (!f) continue;
        struct dirent *g;
        while ((g = readdir(f))) {
            std::string n = g->d_name;
            if (n.find("2433") == std::string::npos) continue;
            o.push_back(dir + "/" + n);
        }
        closedir(f);
    }
    closedir(d);
    std::sort(o.begin(), o.end());
}
static std::string p38(const std::string &p) {
    std::string q = p; size_t i = q.rfind("2433");
    if (i != std::string::npos) q.replace(i, 4, "2438");
    return q;
}

int main() {
    uint8_t d33[DUMP_SIZE], d38[DS2438_MEM_SIZE], w33[DUMP_SIZE];

    printf("1) зсуви дзеркала названі, а не вписані числами\n");
    {
        check(IMPRES_MIRROR_D33_AT == 0x01, "у DS2433 дзеркало починається з 0x01");
        check(IMPRES_MIRROR_D38_AT == 0x18, "у DS2438 — з 0x18");
        check(IMPRES_MIRROR_LEN == 26, "довжина 26 байтів");
        // Паспортна ємність DS2433[0x008] мусить потрапляти всередину дзеркала.
        check(MIRROR_RATED_IDX >= 0 && MIRROR_RATED_IDX < IMPRES_MIRROR_LEN,
              "байт паспортної ємності лежить усередині дзеркала");
        check(IMPRES_MIRROR_D33_AT + MIRROR_RATED_IDX == IMPRES_RATED_BYTE,
              "…і рівно там, де його шукає решта проєкту");
    }

    printf("\n2) план на синтетичних даних\n");
    {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33));
        memset(a38, 0, sizeof(a38));
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
            a33[IMPRES_MIRROR_D33_AT + i] = (uint8_t)(0x10 + i);
            a38[IMPRES_MIRROR_D38_AT + i] = (uint8_t)(0x10 + i);
        }
        a38[IMPRES_MIRROR_D38_AT + 3] = 0xAA;      // один байт розійшовся
        a38[IMPRES_MIRROR_D38_AT + 9] = 0xBB;      // і другий

        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);
        printf("   розбіжностей: %d\n", p.diffCount);
        check(p.have33 && p.have38, "обидва чипи в плані");
        check(p.srcUsable, "джерело придатне (не суцільні 0x00/0xFF)");
        check(p.diffCount == 2, "знайдено рівно дві розбіжності");
        check(p.diff[3] && p.diff[9], "і саме ті байти, які ми зіпсували");
        check(p.take[3] && p.take[9], "типово переносимо різні байти");
        check(!p.take[0] && !p.take[5], "однакові байти не переписуємо — це зайвий запис EEPROM");
        check(mirrorPlanChanges(p) == 2, "зміниться рівно два байти");

        // Правка ПЕРЕД синхронізацією: зняти один байт із перенесення.
        mirrorPlanTakeOne(p, 3, false);
        check(mirrorPlanChanges(p) == 1, "знятий байт більше не переноситься");
        check(p.out[3] == a33[IMPRES_MIRROR_D33_AT + 3], "…і в ньому лишається СТАРЕ значення");
        check(p.out[9] == 0xBB, "а увімкнений байт бере значення з монітора");

        mirrorPlanTakeAll(p, true);
        check(mirrorPlanChanges(p) == 2, "«взяти все» повертає обидва");
        mirrorPlanTakeAll(p, false);
        check(mirrorPlanChanges(p) == 0, "«не брати нічого» лишає дамп як є");

        // Застосування справді пише те, що показано.
        mirrorPlanTakeAll(p, true);
        memcpy(w33, a33, DUMP_SIZE);
        int n = mirrorPlanApply(p, w33);
        check(n == 2, "застосовано рівно два байти");
        check(w33[IMPRES_MIRROR_D33_AT + 3] == 0xAA &&
              w33[IMPRES_MIRROR_D33_AT + 9] == 0xBB, "записано саме значення з монітора");
        check(impresHeaderOk(w33), "сума заголовка виправлена");
        check(impresMirrorOk(w33, a38), "після повного перенесення чипи збігаються");
    }

    printf("\n3) правка паспортної ємності перед синхронізацією\n");
    {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33)); memset(a38, 0, sizeof(a38));
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
            a33[IMPRES_MIRROR_D33_AT + i] = 0x11;
            a38[IMPRES_MIRROR_D38_AT + i] = 0x11;
        }
        a33[IMPRES_RATED_BYTE] = 86;                                  // 2150 мА·год
        a38[IMPRES_MIRROR_D38_AT + MIRROR_RATED_IDX] = 120;           // 3000 мА·год

        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);
        printf("   у чипі %d мА·год, у моніторі %d мА·год\n", p.ratedNow, p.ratedSrc);
        check(p.ratedNow == 2150, "ємність із DS2433 розібрана");
        check(p.ratedSrc == 3000, "ємність із DS2438 розібрана");

        int got = mirrorPlanSetRated(p, 2500);
        printf("   вписали 2500 -> збережено %d мА·год\n", got);
        check(got == 2500, "рівне кратне кроку 25 зберігається як є");
        check(p.out[MIRROR_RATED_IDX] == 100, "у байт лягає 2500/25 = 100");

        // Округлення до кроку 25 — і клієнту повертається саме те, що ляже в чип.
        got = mirrorPlanSetRated(p, 2140);
        printf("   вписали 2140 -> збережено %d мА·год\n", got);
        check(got == 2150, "некратне округлюється до кроку 25");
        check(mirrorRatedFromByte(p.out[MIRROR_RATED_IDX]) == got,
              "повернене число дорівнює тому, що буде в чипі");

        // Межі.
        check(mirrorPlanSetRated(p, 10) == IMPRES_RATED_MIN_MAH, "нижче мінімуму — затиск");
        check(mirrorPlanSetRated(p, 99999) == IMPRES_RATED_MAX_MAH, "вище стелі — затиск");

        // ⚑ Ручна ємність мусить перемагати монітор: її вписують саме тоді,
        //  коли монітору не довіряють (після заміни банок).
        mirrorPlanSetRated(p, 2500);
        mirrorPlanTakeAll(p, true);
        check(mirrorRatedFromByte(p.out[MIRROR_RATED_IDX]) == 2500,
              "ручне значення сильніше за монітор навіть при «взяти все»");
        // …і скасування повертає монітор.
        mirrorPlanSetRated(p, 0);
        check(mirrorRatedFromByte(p.out[MIRROR_RATED_IDX]) == 3000,
              "скасування ручного значення повертає число монітора");
    }

    printf("\n4) непридатне джерело нічого не переносить\n");
    {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0x55, sizeof(a33));
        MirrorPlan p;

        memset(a38, 0xFF, sizeof(a38));            // монітор стертий
        mirrorPlanBuild(p, a33, a38);
        check(!p.srcUsable, "суцільні 0xFF — не джерело");
        check(mirrorPlanChanges(p) == 0, "і нічого не переноситься");

        memset(a38, 0x00, sizeof(a38));            // монітор занулений
        mirrorPlanBuild(p, a33, a38);
        check(!p.srcUsable, "суцільні 0x00 — теж не джерело");
        check(mirrorPlanChanges(p) == 0, "і теж нічого не переноситься");

        mirrorPlanBuild(p, a33, nullptr);          // монітора немає взагалі
        check(p.have33 && !p.have38, "без монітора план усе одно будується");
        check(mirrorPlanChanges(p) == 0, "але переносити нічого");
        memcpy(w33, a33, DUMP_SIZE);
        mirrorPlanApply(p, w33);
        check(impresHeaderOk(w33), "…і сума заголовка все одно виправляється");
    }

    printf("\n5) корпус dumps/: цілим пакетам не стає гірше\n");
    {
        std::vector<std::string> files; collect(files);
        int seen = 0, sync = 0, hdrBad = 0, mirrorBad = 0, changedOk = 0;
        for (auto &f : files) {
            if (!load(f.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(p38(f).c_str(), d38, DS2438_MEM_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;         // цікавлять ЦІЛІ пакети
            seen++;
            MirrorPlan p;
            mirrorPlanBuild(p, d33, d38);
            if (p.mirrorOkNow) sync++;
            memcpy(w33, d33, DUMP_SIZE);
            int n = mirrorPlanApply(p, w33);
            if (n) changedOk++;
            if (!impresHeaderOk(w33)) hdrBad++;
            // Якщо джерело придатне й ми взяли все — чипи мусять зійтись.
            if (p.srcUsable && !impresMirrorOk(w33, d38)) mirrorBad++;
        }
        printf("   цілих пакетів %d, з них дзеркало вже збігалось у %d, змінено %d\n",
               seen, sync, changedOk);
        check(seen > 30, "корпус достатній");
        check(hdrBad == 0, "після синхронізації заголовок ЗАВЖДИ цілий");
        check(mirrorBad == 0, "де джерело придатне — чипи після синхронізації збігаються");
        // На цілих пакетах дзеркало здебільшого вже на місці: операція
        // потрібна для ремонту, а не для щоденного вжитку.
        check(sync > seen / 2, "у більшості цілих пакетів дзеркало вже узгоджене");
    }

    printf("\n6) операція ідемпотентна\n");
    {
        std::vector<std::string> files; collect(files);
        int seen = 0, notIdem = 0;
        for (auto &f : files) {
            if (!load(f.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(p38(f).c_str(), d38, DS2438_MEM_SIZE)) continue;
            seen++;
            MirrorPlan p;
            mirrorPlanBuild(p, d33, d38);
            memcpy(w33, d33, DUMP_SIZE);
            mirrorPlanApply(p, w33);
            // Другий прохід не сміє змінити вже нічого.
            MirrorPlan q;
            mirrorPlanBuild(q, w33, d38);
            uint8_t x33[DUMP_SIZE]; memcpy(x33, w33, DUMP_SIZE);
            if (mirrorPlanApply(q, x33) != 0 || memcmp(x33, w33, DUMP_SIZE) != 0) notIdem++;
        }
        printf("   перевірено пар: %d\n", seen);
        check(seen > 30, "є на чому перевіряти");
        check(notIdem == 0, "повторна синхронізація нічого не змінює");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
