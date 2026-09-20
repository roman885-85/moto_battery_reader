// Відновлення за зразком китайської копії. Головне, що тут перевіряється:
// дзеркало заголовка в DS2438 справді несе паспортну ємність, тож копія з
// ПОРОЖНІМ DS2433 лишається читабельною для рації.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#define DS2438_RSENSE_OHM 0.025f
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
#define PROGMEM
#define memcpy_P memcpy
#include "settings.h"
#include "impres_format.h"
#include "impres_bms.h"
#include "impres_clone.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

// Дамп монітора реальної копії — надіслав власник.
static const char *CLONE_HEX =
 "0F 48 18 22 03 00 00 40 86 E5 0B 00 F0 18 00 FC 78 E5 0B 00 8B 89 F1 20 "
 "50 01 01 00 00 C7 53 56 05 C2 2D FF FF 2B C0 71 08 FF FF 01 56 D7 FF FF "
 "AF 0F FC 18 17 33 00 00 D5 11 89 8C B7 09 81 09";
static void parseHex(const char *s, uint8_t *d, int n) {
    for (int i = 0; i < n; i++) d[i] = (uint8_t)strtol(s + i * 3, nullptr, 16);
}
static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t g = fread(b, 1, n, f); fclose(f); return g == n;
}

int main() {
    uint8_t c38[DS2438_MEM_SIZE];
    parseHex(CLONE_HEX, c38, DS2438_MEM_SIZE);

    // ── 1. Дзеркало заголовка справді там, де ми його шукаємо ─────────────
    printf("1) дзеркало заголовка в DS2438\n");
    printf("   присутнє: %s\n", impresCloneMirrorPresent(c38) ? "так" : "НІ");
    if (!impresCloneMirrorPresent(c38)) bad("дзеркала не знайдено — уся схема на ньому тримається");
    int rated = impresCloneRatedFrom38(c38);
    printf("   паспортна ємність із дзеркала: %d мА·год (байт %d)\n", rated, c38[CLONE_RATED_AT]);
    if (rated != 2150) bad("ємність із дзеркала прочиталась не 2150");

    // Звіряємо з РЕАЛЬНИМ заголовком пакета тієї ж родини: дзеркало має
    // збігатися з DS2433[0x01..0x20] побайтово (зсув +1).
    uint8_t r33[DUMP_SIZE];
    if (load("dumps/02-katalog-osnovnyi/files/02_PMNN4409B_2433.bin", r33, DUMP_SIZE)) {
        uint8_t r38[DS2438_MEM_SIZE];
        if (load("dumps/02-katalog-osnovnyi/files/02_PMNN4409B_2438.bin", r38, DS2438_MEM_SIZE)) {
            int same = 0;
            for (int i = 0; i < CLONE_MIRROR_LEN; i++)
                if (r38[CLONE_MIRROR_AT + i] == r33[1 + i]) same++;
            printf("   на рідній парі 4409B збіг дзеркала: %d/%d байт\n", same, CLONE_MIRROR_LEN);
            // Збігатись мають УСІ 27 байт збіжної частини: якби зсув був
            // інший, збіг був би випадковим і неповним.
            if (same != CLONE_MIRROR_LEN) bad("зсув дзеркала визначено неправильно");
            printf("   ємність із дзеркала 4409B: %d мА·год (з DS2433: %d)\n",
                   impresCloneRatedFrom38(r38), impresRatedFromDump(r33));
            if (impresCloneRatedFrom38(r38) != impresRatedFromDump(r33))
                bad("ємність із дзеркала не збіглася з ємністю з DS2433");
        }
    }

    // ── 2. Побудова монітора: лічильники в нуль, ємність і шунт свої ──────
    printf("\n2) підготовка монітора за зразком копії\n");
    {
        uint8_t out[DS2438_MEM_SIZE];
        impresCloneBuild38(out, c38, /*rated*/2500, /*rs*/4565, /*ica*/176);
        printf("   ETM %u, CCA %u, DCA %u, ICA %u\n",
               (unsigned)impresEtm(out),
               (unsigned)((out[61] << 8) | out[60]), (unsigned)((out[63] << 8) | out[62]), out[12]);
        if (impresEtm(out) != 0) bad("наробіток не обнулено");
        if (((out[61] << 8) | out[60]) || ((out[63] << 8) | out[62])) bad("лічильники CCA/DCA не обнулено");
        if (out[12] != 176) bad("паливомір не встановлено");
        printf("   ємність із дзеркала: %d мА·год\n", impresCloneRatedFrom38(out));
        if (impresCloneRatedFrom38(out) != 2500) bad("вписана ємність не потрапила в дзеркало");
        int rs = out[56] | (out[57] << 8);
        printf("   шунт: %.2f мОм\n", rs / 100.0);
        if (rs != 4565) bad("шунт не записався");

        // Решта монітора має лишитись від копії: конфіг, OFFSET, температура.
        if (out[0] != c38[0]) bad("зіпсовано байт конфігурації");
        if (memcmp(out + 13, c38 + 13, 2)) bad("зіпсовано OFFSET АЦП");

        // Нуль у ємності означає «лишити як є».
        uint8_t keep[DS2438_MEM_SIZE];
        impresCloneBuild38(keep, c38, 0, 0, 100);
        printf("   без вписаної ємності лишається: %d мА·год, шунт %.2f мОм\n",
               impresCloneRatedFrom38(keep), (keep[56] | (keep[57] << 8)) / 100.0);
        if (impresCloneRatedFrom38(keep) != 2150) bad("нуль мав лишити ємність копії");
        if ((keep[56] | (keep[57] << 8)) != 4565) bad("нуль мав лишити шунт копії");
    }

    // ── 3. Порожній монітор дзеркала не має — і ми це бачимо ─────────────
    printf("\n3) монітор без дзеркала\n");
    {
        uint8_t z[DS2438_MEM_SIZE]; memset(z, 0, sizeof(z));
        uint8_t f[DS2438_MEM_SIZE]; memset(f, 0xFF, sizeof(f));
        printf("   нулі: дзеркало %s, ємність %d\n",
               impresCloneMirrorPresent(z) ? "є" : "немає", impresCloneRatedFrom38(z));
        printf("   0xFF: дзеркало %s, ємність %d\n",
               impresCloneMirrorPresent(f) ? "є" : "немає", impresCloneRatedFrom38(f));
        if (impresCloneMirrorPresent(z) || impresCloneMirrorPresent(f))
            bad("порожній монітор визнано таким, що несе дзеркало");
        if (impresCloneRatedFrom38(z) || impresCloneRatedFrom38(f))
            bad("з порожнього монітора взялась якась ємність");
    }

    // ── 4. Вбудований зразок = дамп власника, байт у байт ─────────────────
    //  Зразок вшито в прошивку, щоб не носити файл із собою. Якщо він колись
    //  розійдеться з оригіналом, режим копії почне писати не те, що перевірено.
    printf("\n4) вбудований зразок\n");
    printf("   зразків у прошивці: %d\n", CLONE_SAMPLE_COUNT);
    if (CLONE_SAMPLE_COUNT < 1) bad("жодного вбудованого зразка");
    else {
        const CloneSample &sm = CLONE_SAMPLES[0];
        printf("   «%s» — %s\n", sm.name, sm.note);
        int d = 0;
        for (int i = 0; i < DS2438_MEM_SIZE; i++) if (sm.d38[i] != c38[i]) d++;
        printf("   збіг із дампом власника: %d/%d байт\n", DS2438_MEM_SIZE - d, DS2438_MEM_SIZE);
        if (d) bad("вбудований зразок розійшовся з дампом власника");
        printf("   ємність із дзеркала: %d мА·год, шунт %.2f мОм\n",
               impresCloneRatedFrom38(sm.d38), (sm.d38[56] | (sm.d38[57] << 8)) / 100.0);
        if (impresCloneRatedFrom38(sm.d38) != 2150) bad("ємність зразка не 2150");
    }

    // ── 5. Скидання лічильників — прапорцем, а не мовчки ──────────────────
    printf("\n5) скидання лічильників можна не робити\n");
    {
        uint8_t keep[DS2438_MEM_SIZE];
        impresCloneBuild38(keep, c38, 0, 0, 100, /*zeroCounters=*/false);
        printf("   без скидання: ETM %u с, CCA %u, DCA %u\n",
               (unsigned)impresEtm(keep),
               (unsigned)((keep[61] << 8) | keep[60]), (unsigned)((keep[63] << 8) | keep[62]));
        if (impresEtm(keep) != impresEtm(c38)) bad("наробіток зачепило попри знятий прапорець");
        if (((keep[61] << 8) | keep[60]) != ((c38[61] << 8) | c38[60]))
            bad("CCA зачепило попри знятий прапорець");
        // Паливомір ставиться завжди — він не лічильник, а поточний стан.
        if (keep[12] != 100) bad("паливомір не встановлено");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
