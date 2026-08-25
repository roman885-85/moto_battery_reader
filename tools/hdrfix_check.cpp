// «Добудова» після зарядної станції WPLN4226A — перевірка ЛОГІКИ виявлення
// (ISS_CHARGER_PARTIAL) і самого ремонту (дзеркало + сума). Фрагменти
// скопійовано з recovery.h/web_server.h дослівно — той самий шаблон, що й у
// wiz_check.cpp: web_server.h надто важкий для хостового тесту (WebServer.h,
// SPIFFS.h), тож перевіряємо чисту логіку окремо від Arduino-обв'язки.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>

#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#define DS2438_RSENSE_OHM 0.025f
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
#include "settings.h"
#include "impres_format.h"
#include "impres_bms.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

// ── дослівно з web_server.h ────────────────────────────────────────────────
static void fixHeaderChecksum(uint8_t *d) {
    int s = 0;
    for (int i = 0; i < 0x1F; i++) s += d[i];
    d[0x1F] = (0x41 - s) & 0xFF;
}
static bool headerChecksumOk(const uint8_t *d) {
    int s = 0;
    for (int i = 0; i <= 0x1F; i++) s += d[i];
    return (s & 0xFF) == 0x41;
}
static void syncMirrorFrom2438(uint8_t *d33, const uint8_t *d38) {
    for (int i = 0; i < 26; i++) d33[1 + i] = d38[24 + i];
    fixHeaderChecksum(d33);
}
static bool mirrorOk(const uint8_t *d33, const uint8_t *d38) {
    for (int i = 0; i < 26; i++) if (d33[1 + i] != d38[24 + i]) return false;
    return true;
}
static bool mirrorSourceValid(const uint8_t *d38) {
    bool allZero = true, allFF = true;
    for (int i = 24; i < 50; i++) { if (d38[i] != 0x00) allZero = false; if (d38[i] != 0xFF) allFF = false; }
    return !allZero && !allFF;
}
// ── детектор ISS_CHARGER_PARTIAL ──────────────────────────────────────────
//  ⚑ БЕЗ КОПІЇ. Раніше тут лежало те саме правило, переписане руками з
//  recovery.h, — і коли оригінал виправили (додали умову «решта чипа порожня»), копія
//  лишилась старою й тест далі проходив на зламаній логіці, ловлячи справний
//  пакет як «станція почала добудову». Тепер обидва місця кличуть ОДНУ
//  функцію з impres_format.h, і розійтися їм нема як.
static bool chargerPartial(const uint8_t *d33, const uint8_t *d38, bool has38) {
    return impresChargerPartial(d33, d38, has38);
}

static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t g = fread(b, 1, n, f); fclose(f); return g == n;
}

int main() {
    // ── 1. Реальний випадок власника ───────────────────────────────────────
    printf("1) дамп зі скарги: станція дописала дзеркало, суму не виправила\n");
    uint8_t d33[DUMP_SIZE], d38[DS2438_MEM_SIZE];
    if (!load("dumps/19-stantsiya-dobudova/files/01_2433.bin", d33, DUMP_SIZE)) {
        bad("дамп зі скарги не знайдено");
    } else {
        // DS2438 для цього самого пакета: беремо той, з якого дзеркало явно
        // збігається (сам дамп власника ніс лише 32 hex-рядки DS2433,
        // а дзеркало вже підтверджує зміст DS2438[0x18..0x31]).
        memset(d38, 0, DS2438_MEM_SIZE);
        for (int i = 0; i < 26; i++) d38[24 + i] = d33[1 + i];
        d38[50] = 0xAF; d38[51] = 0x0F;   // хоч щось поза дзеркалом, щоб не «все нулі»

        printf("   заголовок Σ=0x%02X (валідний: %s)\n",
               ([&]{int s=0;for(int i=0;i<=0x1F;i++)s+=d33[i];return s&0xFF;})(),
               headerChecksumOk(d33) ? "так" : "ні");
        bool cp = chargerPartial(d33, d38, true);
        printf("   ISS_CHARGER_PARTIAL: %s\n", cp ? "так" : "ні");
        if (!cp) bad("не розпізнано як частковий запис станції");

        // Добудова: дзеркало вже правильне, лишається виправити суму.
        uint8_t w[DUMP_SIZE]; memcpy(w, d33, DUMP_SIZE);
        bool already = mirrorOk(w, d38);
        syncMirrorFrom2438(w, d38);
        printf("   дзеркало вже було правильне: %s\n", already ? "так" : "ні");
        printf("   після добудови: заголовок валідний: %s\n", headerChecksumOk(w) ? "так" : "ні");
        if (!headerChecksumOk(w)) bad("добудова не полагодила суму");
        if (!mirrorOk(w, d38)) bad("добудова зіпсувала дзеркало");
        // Решта чипа (0x020..0x1FF) НЕ мала змінитись — добудова обіцяє РІВНО
        // заголовок, не більше.
        if (memcmp(w + 0x20, d33 + 0x20, DUMP_SIZE - 0x20) != 0)
            bad("добудова зачепила щось поза заголовком");
        // Після добудови це вже не «частковий запис» — стан визначено.
        if (chargerPartial(w, d38, true)) bad("після добудови досі вважається частковим");
    }

    // ── 2. Не плутати зі «звичайним» побитим заголовком ────────────────────
    printf("\n2) звичайний побитий заголовок — це НЕ той самий стан\n");
    {
        // Псуємо один байт заголовка на реальному робочому дампі: дзеркало
        // після цього вже НЕ збіжиться (ми зіпсували саме мирровану частину).
        uint8_t g33[DUMP_SIZE], g38[DS2438_MEM_SIZE];
        if (load("dumps/02-katalog-osnovnyi/files/02_PMNN4409B_2433.bin", g33, DUMP_SIZE) &&
            load("dumps/02-katalog-osnovnyi/files/02_PMNN4409B_2438.bin", g38, DS2438_MEM_SIZE)) {
            g33[5] ^= 0xFF;   // усередині мирророваної частини
            fixHeaderChecksum(g33); g33[5] ^= 0x00; // лишаємо суму «правильною під зіпсоване» — сама сума ОК, а дзеркало розійшлось
            bool cp = chargerPartial(g33, g38, true);
            printf("   зіпсований байт усередині дзеркала, сума ОК: ISS_CHARGER_PARTIAL = %s (має бути «ні»)\n",
                   cp ? "так" : "ні");
            if (cp) bad("звичайне псування переплутано з частковим записом станції");
        }
    }

    // ── 2а. ПОБУДОВАНИЙ чип із побитою сумою — теж НЕ добудова ─────────────
    //  Саме цей випадок і був хибно продіагностований: у справного пакета
    //  дзеркало сходиться (на те воно й дзеркало), тож самої лише «сума хибна
    //  + дзеркало збігається» замало. Реальний дамп зі скарги власника.
    printf("\n2а) чип побудований, а сума заголовка втрачена — це НЕ добудова\n");
    {
        uint8_t r33[DUMP_SIZE], r38[DS2438_MEM_SIZE];
        if (load("dumps/20-vymahaie-vidnovlennya/files/03_PMNN4409A_2433.bin", r33, DUMP_SIZE) &&
            load("dumps/20-vymahaie-vidnovlennya/files/03_PMNN4409A_2438.bin", r38, DS2438_MEM_SIZE)) {
            int body = 0;
            for (int i = 0x20; i < (int)DUMP_SIZE; i++) if (r33[i] != 0xFF) body++;
            printf("   сума заголовка: %s, дзеркало: %s, непорожніх байтів після 0x020: %d\n",
                   impresHeaderOk(r33) ? "ОК" : "ХИБНА",
                   impresMirrorOk(r33, r38) ? "сходиться" : "розійшлось", body);
            if (impresHeaderOk(r33)) bad("дамп зі скарги мусить мати саме ХИБНУ суму заголовка");
            if (!impresMirrorOk(r33, r38)) bad("…і дзеркало в ньому мусить сходитись — інакше випадок не той");
            if (body == 0) bad("…і чип мусить бути побудований — інакше це справді добудова");
            if (chargerPartial(r33, r38, true))
                bad("побудований чип із втраченою сумою переплутано з добудовою станції");
            else
                printf("   ок    діагноз не «добудова» — тобто ремонт піде іншим шляхом\n");
        } else bad("дамп 20-vymahaie-vidnovlennya/03 не зчитався");

        // І дзеркальна перевірка: варто ЄДИНОМУ байту з'явитися після 0x020 у
        // справжньому випадку добудови — і це вже не добудова.
        uint8_t w33[DUMP_SIZE];
        memcpy(w33, d33, DUMP_SIZE);
        w33[0x148] = 0x0B;                 // «щось уже записано»
        if (chargerPartial(w33, d38, true))
            bad("умова «решта чипа порожня» не діє — один байт її не зрушив");
        else
            printf("   ок    один непорожній байт після 0x020 знімає діагноз добудови\n");
    }

    // ── 3. Порожній чип — теж не «частковий запис» ─────────────────────────
    printf("\n3) повністю стертий чип\n");
    {
        uint8_t z33[DUMP_SIZE]; memset(z33, 0xFF, DUMP_SIZE);
        bool cp = chargerPartial(z33, d38, true);
        printf("   ISS_CHARGER_PARTIAL на 0xFF: %s (має бути «ні» — це ISS_BLANK33)\n", cp ? "так" : "ні");
        if (cp) bad("порожній чип переплутано з частковим записом станції");
    }

    // ── 4. Жодної хибної тривоги на всьому корпусі ─────────────────────────
    printf("\n4) увесь корпус дампів — жодного хибного спрацювання\n");
    {
        DIR *dd = opendir("dumps");
        std::vector<std::string> all;
        struct dirent *e;
        while (dd && (e = readdir(dd))) {
            if (e->d_name[0] == '.') continue;
            std::string sub = std::string("dumps/") + e->d_name + "/files";
            DIR *f = opendir(sub.c_str());
            if (!f) continue;
            struct dirent *g;
            while ((g = readdir(f))) {
                std::string n = g->d_name;
                if (n.size() > 9 && n.substr(n.size() - 9) == "_2433.bin") all.push_back(sub + "/" + n);
            }
            closedir(f);
        }
        if (dd) closedir(dd);
        std::sort(all.begin(), all.end());
        int hits = 0, checked = 0;
        for (auto &p33 : all) {
            uint8_t a33[DUMP_SIZE];
            if (!load(p33.c_str(), a33, DUMP_SIZE)) continue;
            std::string p38 = p33; size_t q = p38.find("2433");
            if (q != std::string::npos) p38.replace(q, 4, "2438");
            uint8_t a38[DS2438_MEM_SIZE];
            bool has38 = load(p38.c_str(), a38, DS2438_MEM_SIZE);
            checked++;
            if (chargerPartial(a33, a38, has38)) {
                hits++;
                printf("   спрацювало: %s\n", p33.c_str());
            }
        }
        printf("   перевірено %d дампів, спрацювань: %d (очікуємо рівно 1 — щойно доданий випадок)\n",
               checked, hits);
        if (hits != 1) bad("спрацювань не рівно 1 — або пропуск, або хибна тривога на корпусі");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
