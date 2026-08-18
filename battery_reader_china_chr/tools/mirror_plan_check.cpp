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

    printf("\n7) свідчення «чи монітор від цього пакета» всередині плану\n");
    // Скарга власника, дослівно: «Напрацювання ETM (6397 діб) більше за вік
    // пакета (15 діб від 2026-08-03)… — нудно добавить в пункт синхронизации».
    // Питання справді належить саме сюди: синхронізація переносить
    // ідентичність З МОНІТОРА, тож із чужим монітором вона не лікує пакет, а
    // приписує йому чужу особу — і робить це «за планом», тобто впевнено.
    {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33)); memset(a38, 0, sizeof(a38));
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
            a33[IMPRES_MIRROR_D33_AT + i] = (uint8_t)(0x20 + i);
            a38[IMPRES_MIRROR_D38_AT + i] = (uint8_t)(0x20 + i);
        }
        a38[IMPRES_MIRROR_D38_AT + 7] = 0xCC;      // одна розбіжність, щоб було що знімати

        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);
        check(!p.haveAge && !p.etmForeign, "поки віку не повідомили — мовчимо");

        mirrorPlanSetEtm(p, 6397, 15);             // числа з натури
        check(p.haveAge && p.etmForeign, "наробіток 6397 діб при віці 15 — підозра");
        check(p.etmDays == 6397 && p.ageDays == 15, "числа доїжджають до клієнта як є");

        mirrorPlanSetEtm(p, 6397, 0);
        check(!p.haveAge && !p.etmForeign,
              "без дати не звинувачуємо: нема з чим порівнювати — нема й підозри");

        // ⚑ Межа допуску — та сама, що в аудиті. Тут це перевіряється НЕ на
        //  числі 180, а на самій функції: якби план мав власну копію правила,
        //  вони розійшлися б саме на межі, тобто там, де це найдорожче.
        for (long age = 1; age <= 900; age += 37)
            for (long etm = 0; etm <= 4000; etm += 311) {
                mirrorPlanSetEtm(p, etm, age);
                if (p.etmForeign != impresEtmForeign(etm, age)) {
                    check(false, "план і аудит відповідають однаково");
                    age = 10000; break;
                }
            }
        mirrorPlanSetEtm(p, 400, 100);
        check(p.etmForeign == impresEtmForeign(400, 100), "план і аудит відповідають однаково");

        // ⚑ СВІДЧЕННЯ НЕ СМІЄ ЧІПАТИ ПЛАН. Дату приносить клієнт уже після
        //  того, як людина розставила галочки; якби її поява перебудовувала
        //  план, галочки тихо злітали б — і записалось би не те, що показано.
        //  Стан галочок перед приходом дати мусить бути НЕПОРОЖНІМ — інакше
        //  «нічого не змінилось» вийде саме по собі й перевірка нічого не
        //  доведе (перша редакція цього тесту саме так і мовчала).
        mirrorPlanTakeOne(p, 3, true);         // однаковий байт — руками
        check(p.take[7] && p.take[3], "перед приходом дати галочки справді стоять");
        uint8_t before[IMPRES_MIRROR_LEN];
        bool    takeBefore[IMPRES_MIRROR_LEN];
        memcpy(before, p.out, sizeof(before));
        memcpy(takeBefore, p.take, sizeof(takeBefore));
        mirrorPlanSetEtm(p, 6397, 15);
        check(memcmp(before, p.out, sizeof(before)) == 0 &&
              memcmp(takeBefore, p.take, sizeof(takeBefore)) == 0,
              "поява дати не скидає розставлених галочок");
    }

    printf("\n8) однаковий байт можна відмітити ПАЛЬЦЕМ\n");
    // Скарга власника: «Bridge/web — не выбираются пункты плана». Причина була
    // не в клієнтах: галочка жила лише в рядка, де байти РІЗНІ, а на цілих
    // пакетах дзеркало здебільшого вже збігається (див. секцію 5) — тобто в
    // натурі жодна галочка не натискалась.
    {
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33)); memset(a38, 0, sizeof(a38));
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
            a33[IMPRES_MIRROR_D33_AT + i] = (uint8_t)(0x30 + i);
            a38[IMPRES_MIRROR_D38_AT + i] = (uint8_t)(0x30 + i);
        }
        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);
        check(p.srcUsable && p.diffCount == 0, "чипи вже збігаються — типовий стан цілого пакета");
        check(!p.take[5], "типово не беремо нічого: зайвий запис EEPROM ні до чого");
        mirrorPlanTakeOne(p, 5, true);
        check(p.take[5], "…але вручну байт відмічається");
        check(mirrorPlanChanges(p) == 0, "значення від цього не змінюється — воно й так однакове");

        // Єдина умова лишилась: щоб було ЗВІДКИ брати.
        memset(a38, 0xFF, sizeof(a38));
        mirrorPlanBuild(p, a33, a38);
        mirrorPlanTakeOne(p, 5, true);
        check(!p.take[5], "без придатного джерела «перенести» нема чого — і галочка не стає");
    }

    printf("\n9) значення, обліковані двічі — на числах із поля\n");
    // Скарга власника з натури (PMNN4409B, обидва дампи в листуванні):
    //   ETM у DS2438 …… 552 727 405 с = 6397 діб = 17 років
    //   а пакет за DS2433: виготовлений 2026-08-03, уперше ввімкнений
    //   2026-08-18 — тобто йому 15 діб, і наробітку в нього нуль;
    //   CCA raw 195 -> 31 еквівалентний цикл, DCA raw 238 -> 37 циклів,
    //   а лічильники Motorola кажуть 1 цикл IMPRES + 5 не-IMPRES.
    // Байтове дзеркало в цьому пакеті збігається ПОВНІСТЮ — тобто стара
    // синхронізація сказала б «усе гаразд» і не полагодила б нічого.
    {
        const int   RATED = 2150;        // DS2433[0x008] = 0x56 -> 86 * 25
        const float RS    = 0.04565f;    // DS2438[56..57] = 0x11D5 -> 45.65 мОм

        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33)); memset(a38, 0, sizeof(a38));
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {          // дзеркало збігається
            a33[IMPRES_MIRROR_D33_AT + i] = (uint8_t)(0x40 + i);
            a38[IMPRES_MIRROR_D38_AT + i] = (uint8_t)(0x40 + i);
        }
        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);
        check(p.diffCount == 0, "байтове дзеркало збігається — байтам синхронізація не потрібна");
        check(!p.haveVals, "поки числа не повідомили — значеннєвих рядків немає");

        MirrorValIn in;
        memset(&in, 0, sizeof(in));
        in.have38 = true; in.ratedMah = RATED; in.rsense = RS; in.rsFromChip = true;
        in.monEtmDays = 552727405L / 86400L;      // 6397
        in.monCca = 195; in.monDca = 238;
        in.packEtmKnown = true; in.packEtmDays = 0;      // перший запуск сьогодні
        in.packCycKnown = true; in.packCycles = 1 + 5;   // IMPRES + не-IMPRES
        mirrorPlanSetVals(p, in);

        printf("   ETM: пакет %ld діб, монітор %ld діб\n", p.val[MVAL_ETM].pack, p.val[MVAL_ETM].mon);
        printf("   CCA: пакет %ld циклів, монітор %ld;  DCA: %ld / %ld\n",
               p.val[MVAL_CCA].pack, p.val[MVAL_CCA].mon,
               p.val[MVAL_DCA].pack, p.val[MVAL_DCA].mon);
        check(p.haveVals, "значеннєві рядки з'явились");
        check(p.val[MVAL_ETM].mon == 6397, "наробіток монітора розібрано як 6397 діб");
        check(p.val[MVAL_CCA].mon == 31, "CCA 195 -> 31 цикл (як і показує картка даних)");
        check(p.val[MVAL_DCA].mon == 37, "DCA 238 -> 37 циклів");
        check(p.val[MVAL_CCA].pack == 6 && p.val[MVAL_DCA].pack == 6,
              "пакет каже 6 циклів (1 IMPRES + 5 не-IMPRES)");
        // ⚑ Типово беремо лише те, що СПРАВДІ розходиться, — так само, як на
        //  байтовому боці. Тут розходяться всі три.
        check(p.val[MVAL_ETM].take && p.val[MVAL_CCA].take && p.val[MVAL_DCA].take,
              "усі три рядки розходяться — типово вони й обрані");
        check(mirrorValChanges(p) == 3, "зміняться всі три");

        // Запис у монітор.
        uint8_t w38[DS2438_MEM_SIZE];
        memcpy(w38, a38, sizeof(w38));
        // Покладемо в дамп саме ті числа, що були в пакеті власника.
        w38[8] = 0x6D; w38[9] = 0xF3; w38[10] = 0xF1; w38[11] = 0x20;
        w38[0x3C] = 195; w38[0x3D] = 0; w38[0x3E] = 238; w38[0x3F] = 0;
        int n = mirrorPlanApply38(p, w38);
        check(n == 3, "у монітор записано три значення");
        check(impresEtm(w38) == 0, "наробіток став 0 — стільки, скільки пакет і прожив");
        uint16_t cca = (uint16_t)w38[0x3C] | ((uint16_t)w38[0x3D] << 8);
        uint16_t dca = (uint16_t)w38[0x3E] | ((uint16_t)w38[0x3F] << 8);
        printf("   записано CCA raw %u, DCA raw %u\n", cca, dca);
        check(mirrorCyclesFromRaw(cca, RATED, RS) == 6, "після запису монітор показує 6 циклів заряду");
        check(mirrorCyclesFromRaw(dca, RATED, RS) == 6, "…і 6 циклів розряду");

        // Повторний прохід не сміє знайти нових розбіжностей.
        MirrorPlan q;
        mirrorPlanBuild(q, a33, w38);
        MirrorValIn in2 = in;
        in2.monEtmDays = (long)(impresEtm(w38) / 86400UL);
        in2.monCca = cca; in2.monDca = dca;
        mirrorPlanSetVals(q, in2);
        check(mirrorValChanges(q) == 0, "повторна синхронізація нічого не змінює");
        check(!q.val[MVAL_CCA].take && !q.val[MVAL_DCA].take && !q.val[MVAL_ETM].take,
              "…і жоден рядок уже не позначений");
    }

    printf("\n10) значеннєвий бік: правка, межі й доступність\n");
    {
        const int RATED = 2150; const float RS = 0.04565f;
        uint8_t a33[DUMP_SIZE], a38[DS2438_MEM_SIZE];
        memset(a33, 0, sizeof(a33)); memset(a38, 0, sizeof(a38));
        MirrorPlan p;
        mirrorPlanBuild(p, a33, a38);

        MirrorValIn in; memset(&in, 0, sizeof(in));
        in.have38 = true; in.ratedMah = RATED; in.rsense = RS; in.rsFromChip = true;
        in.monEtmDays = 6397; in.monCca = 195; in.monDca = 238;
        in.packEtmKnown = false;                 // ключа немає — дата нечитана
        in.packCycKnown = true; in.packCycles = 6;
        mirrorPlanSetVals(p, in);
        check(!p.val[MVAL_ETM].avail, "без читаної дати рядок наробітку недоступний");
        check(p.val[MVAL_CCA].avail, "а цикли ключа не потребують — рядок доступний");
        mirrorValTake(p, MVAL_ETM, true);
        check(!p.val[MVAL_ETM].take, "недоступний рядок не вмикається галочкою");

        // ⚑ Вписане руками оживляє рядок: «монітор бреше, а скільки насправді
        //  — я знаю» — законний і типовий випадок.
        long got = mirrorValSetUser(p, MVAL_ETM, 15);
        check(got == 15 && p.val[MVAL_ETM].take, "вписане число вмикає рядок навіть без пакета");
        check(p.val[MVAL_ETM].out == 15 && p.val[MVAL_ETM].outRaw == 15L * 86400L,
              "…і йде в чип секундами");
        check(mirrorValSetUser(p, MVAL_ETM, -1) == -1 && !p.val[MVAL_ETM].take,
              "скасування ручного числа знімає й галочку");

        // Ручне сильніше за пакет.
        mirrorValSetUser(p, MVAL_CCA, 2);
        check(p.val[MVAL_CCA].out == 2, "ручне число сильніше за те, що каже пакет");
        // Межі: наробіток понад 20 років і понад 9999 циклів — це вже сміття.
        check(mirrorValSetUser(p, MVAL_ETM, 99999) == 20L * 365L, "наробіток затиснуто 20 роками");
        check(mirrorValSetUser(p, MVAL_CCA, 100000) == 9999, "цикли затиснуто 9999");

        // Без шунта з чипа цикли перерахувати нічим — рядки мовчать.
        MirrorPlan q; mirrorPlanBuild(q, a33, a38);
        MirrorValIn in3 = in; in3.rsense = 0.0f; in3.rsFromChip = false;
        mirrorPlanSetVals(q, in3);
        check(!q.val[MVAL_CCA].avail && !q.val[MVAL_DCA].avail,
              "без шунта з чипа цикли не рахуються — і рядок не пропонується");

        // Без монітора писати нікуди.
        MirrorPlan r; mirrorPlanBuild(r, a33, nullptr);
        MirrorValIn in4 = in; in4.have38 = false;
        mirrorPlanSetVals(r, in4);
        check(!r.haveVals, "без прочитаного DS2438 значеннєвих рядків немає");
        uint8_t dummy[DS2438_MEM_SIZE];
        memset(dummy, 0xAB, sizeof(dummy));
        check(mirrorPlanApply38(r, dummy) == 0, "…і в чип нічого не пишеться");
    }

    printf("\n11) перерахунок циклів стійкий на всьому корпусі\n");
    // Перетворення «цикли -> сирі одиниці -> цикли» мусить давати те саме
    // число: інакше після синхронізації картка показувала б на цикл менше,
    // ніж вписали, і людина тиснула б кнопку знову й знову.
    {
        std::vector<std::string> files; collect(files);
        int seen = 0, bad = 0, pairs = 0;
        for (auto &f : files) {
            if (!load(f.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(p38(f).c_str(), d38, DS2438_MEM_SIZE)) continue;
            int rated = impresRatedFromDump(d33);
            float rs  = impresBmsRsense(d38);
            if (rated <= 0 || rs <= 0.0f) continue;
            seen++;
            for (long c = 0; c <= 400; c++) {
                uint16_t raw = mirrorRawFromCycles(c, rated, rs);
                long back = mirrorCyclesFromRaw(raw, rated, rs);
                pairs++;
                if (back != c && raw < 65535) { bad++; break; }
            }
        }
        printf("   пар «ємність × шунт»: %d, перевірок: %d\n", seen, pairs);
        check(seen > 20, "є на чому перевіряти");
        check(bad == 0, "зворотний перерахунок завжди дає те саме число циклів");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
