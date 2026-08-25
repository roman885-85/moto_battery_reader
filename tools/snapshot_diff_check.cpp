// ===========================================================================
//  snapshot_diff_check — ЧИ ЗНАЙДЕ РІЗНИЦЯ ТЕ, ЗАРАДИ ЧОГО ВОНА НАПИСАНА
// ===========================================================================
//  Чотири рази поспіль ми шукали, «звідки повернулось число», перебираючи
//  корпус. Різниця «знімок до станції → чипи зараз» має закрити це прямим
//  вимірюванням. Тому перевіряємо не «функція щось повернула», а те єдине, що
//  тут важить: чи побачить вона зміну в БУДЬ-ЯКОМУ байті — зокрема в такому,
//  про призначення якого ми нічого не знаємо.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#include "snapshot_diff.h"
#include "impres_format.h"

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

static void collect(std::vector<std::string> &out, const char *tag) {
    DIR *d = opendir("dumps");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        std::string dir = std::string("dumps/") + e->d_name + "/files";
        DIR *f = opendir(dir.c_str());
        if (!f) continue;
        struct dirent *g;
        while ((g = readdir(f))) {
            std::string n = g->d_name;
            if (n.find(tag) == std::string::npos) continue;
            out.push_back(dir + "/" + n);
        }
        closedir(f);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

int main() {
    printf("1) різниця бачить зміну в будь-якому байті\n");
    {
        uint8_t a[DS2438_MEM_SIZE], b[DS2438_MEM_SIZE];
        memset(a, 0x5A, sizeof(a));
        SnapDiff d;
        memcpy(b, a, sizeof(a));
        snapDiffBytes(a, b, sizeof(a), d);
        check(d.n == 0 && d.total == 0, "однакові образи різниці не дають");

        // ⚑ КОЖЕН байт, а не лише відомі нам. Поле, якого ми не знаємо, у
        //  список полів не потрапить за визначенням — і саме його різниця
        //  мусить показати.
        bool all = true;
        for (int i = 0; i < (int)sizeof(a); i++) {
            memcpy(b, a, sizeof(a));
            b[i] ^= 0xFF;
            snapDiffBytes(a, b, sizeof(a), d);
            if (d.n != 1 || d.total != 1 || d.runs[0].at != i || d.runs[0].len != 1) { all = false; break; }
        }
        check(all, "зміна в кожному з 64 байтів монітора знаходиться");
    }

    printf("\n2) сусідні байти збираються в одну ділянку\n");
    {
        uint8_t a[DS2438_MEM_SIZE], b[DS2438_MEM_SIZE];
        memset(a, 0, sizeof(a));
        memcpy(b, a, sizeof(a));
        b[0x32] = 0x11; b[0x33] = 0x22;          // мітка 3 — два байти поспіль
        SnapDiff d;
        snapDiffBytes(a, b, sizeof(a), d);
        check(d.n == 1 && d.runs[0].at == 0x32 && d.runs[0].len == 2,
              "«змінились 0x32 і 0x33» — це одна новина, а не дві");
        // ⚑ І РВЕТЬСЯ НА МЕЖІ ПОЛЯ. Мітка3 — це лише 0x32..0x33; якби прогін
        //  переповз у 0x34, невідомий байт приїхав би підписаним «МІТКА 3»,
        //  тобто знахідка сховалась би за чужим заспокійливим іменем.
        memcpy(b, a, sizeof(a));
        b[0x32] = 1; b[0x33] = 2; b[0x34] = 3; b[0x35] = 4;
        snapDiffBytes(a, b, sizeof(a), d, snapName38);
        check(d.n == 2 && d.runs[0].at == 0x32 && d.runs[0].len == 2 &&
              d.runs[1].at == 0x34 && d.runs[1].len == 2,
              "зміна через межу поля ділиться надвоє, а не приїжджає під чужим ім'ям");
        check(snapName38(d.runs[1].at)[0] == '\0',
              "…і друга половина лишається без імені — це й є новина");

        // …а розділені — у різні.
        memcpy(b, a, sizeof(a));
        b[0x08] = 1; b[0x3C] = 1;
        snapDiffBytes(a, b, sizeof(a), d);
        check(d.n == 2 && d.runs[0].at == 0x08 && d.runs[1].at == 0x3C,
              "розділені зміни лишаються різними ділянками");
    }

    printf("\n3) переповнення не ховається\n");
    {
        uint8_t a[DUMP_SIZE], b[DUMP_SIZE];
        memset(a, 0, sizeof(a));
        memcpy(b, a, sizeof(a));
        for (int i = 0; i < SNAP_MAX_RUNS + 5; i++) b[i * 2] = 0xFF;   // через один
        SnapDiff d;
        snapDiffBytes(a, b, sizeof(a), d);
        check(d.n == SNAP_MAX_RUNS && d.truncated,
              "ділянок більше, ніж влізло — і про це сказано");
        check(d.total == SNAP_MAX_RUNS + 5,
              "…а лічильник байтів рахує ВСІ, а не лише показані");
    }

    printf("\n4) підпис дають лише відомим байтам\n");
    {
        check(!strcmp(snapName38(0x08), "НАРОБІТОК (ETM)"), "наробіток підписано");
        check(!strcmp(snapName38(0x32), "МІТКА 3 (доби)"),  "третя мітка підписана");
        check(!strcmp(snapName38(0x10), "МІТКА 1 (секунди)"), "перша мітка підписана");
        // ⚑ НАЙВАЖЛИВІШЕ В ЦЬОМУ РОЗДІЛІ. Байт, призначення якого ми не
        //  знаємо, мусить лишитись БЕЗ ІМЕНІ: вигадана назва заховала б саме
        //  те, заради чого різниця й написана.
        check(snapName38(0x34)[0] == '\0' && snapName38(0x3A)[0] == '\0',
              "невідомі байти лишаються без назви — інакше знахідку не помітиш");
    }

    printf("\n5) на живій парі «до станції / після» видно, що саме змінилось\n");
    {
        // dumps/13 — та сама пара, з якої свого часу дізнались про мітки.
        uint8_t a[DS2438_MEM_SIZE], b[DS2438_MEM_SIZE];
        bool okA = load("dumps/13-dozaryadka-na-stantsii/files/01_PMNN4409A_2438.bin", a, sizeof(a));
        bool okB = load("dumps/13-dozaryadka-na-stantsii/files/02_PMNN4409A_2438.bin", b, sizeof(b));
        if (!okA || !okB) {
            std::vector<std::string> v;
            collect(v, "2438");
            printf("   пари з dumps/13 немає — беремо перші дві з корпусу\n");
            okA = v.size() > 1 && load(v[0].c_str(), a, sizeof(a));
            okB = v.size() > 1 && load(v[1].c_str(), b, sizeof(b));
        }
        check(okA && okB, "пара образів зчиталась");
        if (okA && okB) {
            SnapDiff d;
            snapDiffBytes(a, b, sizeof(a), d, snapName38);
            printf("   змінених байтів %d у %d ділянках:\n", d.total, d.n);
            for (int i = 0; i < d.n; i++) {
                const char *nm = snapName38(d.runs[i].at);
                printf("      0x%02X..0x%02X  %s\n", d.runs[i].at,
                       d.runs[i].at + d.runs[i].len - 1, nm[0] ? nm : "(невідоме поле)");
            }
            check(d.total > 0, "різниця між двома різними образами не порожня");
        }
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
