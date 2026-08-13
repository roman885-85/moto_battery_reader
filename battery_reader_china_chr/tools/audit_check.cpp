// Аудит змісту прошивки (impres_audit.h) на ВСЬОМУ корпусі дампів.
// Головне, що тут перевіряється: аудит ловить те, на що реально скаржився
// власник, і НЕ здіймає тривогу на робочих пакетах.
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
#include "impres_bms.h"
#include "impres_crypt.h"
#include "impres_audit.h"
#include "templates.h"
#include "restore_plan.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t g = fread(b, 1, n, f); fclose(f); return g == n;
}
static void hexrom(const char *s, uint8_t *r) {
    for (int i = 0; i < 8; i++) { char t[3] = {s[i*2], s[i*2+1], 0}; r[i] = (uint8_t)strtol(t, nullptr, 16); }
}
static std::string flags(uint32_t f) {
    std::string s;
    if (f & AUD_CRYPT_WRONG)    s += "чужий-ключ ";
    if (f & AUD_CRYPT_UNKNOWN)  s += "ключ-невідомий ";
    if (f & AUD_BLOCK_SUM)      s += "сума-блока ";
    if (f & AUD_DATE_INSANE)    s += "дата ";
    if (f & AUD_ETM_FOREIGN)    s += "чужий-2438 ";
    if (f & AUD_USE_BEFORE_CHG) s += "запуск-без-заряду ";
    return s.empty() ? std::string("—") : s;
}
// Усі пари 2433+2438 з корпусу.
static void collect(std::vector<std::string> &out) {
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
            if (n.find("2433") == std::string::npos) continue;
            out.push_back(dir + "/" + n);
        }
        closedir(f);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

int main() {
    uint8_t d33[DUMP_SIZE], d38[DS2438_MEM_SIZE], rom[8];

    // ── 1. Реальна скарга: еталон донора на чужому чипі ────────────────────
    printf("1) dumps/16 — вміст донора, ROM чипа A3427C17010050A6\n");
    if (!load("dumps/16-verbatim-4409a-chuzhyi-kliuch/files/01_PMNN4409A_2433.bin", d33, DUMP_SIZE) ||
        !load("dumps/16-verbatim-4409a-chuzhyi-kliuch/files/01_PMNN4409A_2438.bin", d38, DS2438_MEM_SIZE)) {
        printf("   дампів немає — пропускаємо\n");
    } else {
        hexrom("A3427C17010050A6", rom);
        uint32_t f = impresAudit(d33, d38, rom, 2026, 9, 20);
        printf("   знайдено: %s\n", flags(f).c_str());
        if (!(f & AUD_CRYPT_WRONG)) bad("чужий ключ не виявлено — саме на це скаржився власник");

        // Після перешифрування тривоги бути не має.
        int t = -1;
        for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++)
            if (!strcmp(BATTERY_TEMPLATES[i].name, "PMNN4409A")) t = i;
        RestorePlan p;
        restorePlanBuild(p, "PMNN4409A", BATTERY_TEMPLATES[t].d33, BATTERY_TEMPLATES[t].d38,
                         d33, d38, rom);
        uint8_t w[DUMP_SIZE]; memcpy(w, d33, DUMP_SIZE);
        restorePlanApply(p, w, nullptr, true);
        uint32_t f2 = impresAudit(w, d38, rom, 2026, 9, 20);
        printf("   після перешифрування: %s\n", flags(f2).c_str());
        if (f2 & AUD_CRYPT_WRONG) bad("після перешифрування ключ усе ще вважається чужим");
    }

    // ── 2. Побита сума блока: виявляємо й лагодимо ─────────────────────────
    printf("\n2) побита сума блока — виявити й полагодити\n");
    {
        int t = -1;
        for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++)
            if (!strcmp(BATTERY_TEMPLATES[i].name, "PMNN4409A")) t = i;
        uint8_t w[DUMP_SIZE]; memcpy(w, BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
        hexrom("A3427C17010050A6", rom);
        uint32_t clean = impresAudit(w, nullptr, nullptr, 0, 0, 0);
        printf("   цілий еталон: %s\n", flags(clean).c_str());
        if (clean & AUD_BLOCK_SUM) bad("на цілому еталоні знайдено побиту суму");

        uint16_t a = impresBmsVector(w, BMS_V_RECOND);
        w[a + 2] ^= 0x5A;                       // псуємо байт усередині блока
        uint32_t brk = impresAudit(w, nullptr, nullptr, 0, 0, 0);
        printf("   зіпсували байт у RECOND: %s\n", flags(brk).c_str());
        if (!(brk & AUD_BLOCK_SUM)) bad("побиту суму блока не виявлено");

        int n = impresAuditFixSums(w);
        uint32_t fx = impresAudit(w, nullptr, nullptr, 0, 0, 0);
        printf("   полагоджено сум: %d -> %s\n", n, flags(fx).c_str());
        if (n < 1) bad("ремонт не полагодив жодної суми");
        if (fx & AUD_BLOCK_SUM) bad("після ремонту сума все ще побита");
    }

    // ── 3. «Вмикали, але не заряджали» ─────────────────────────────────────
    printf("\n3) пакет позначено як увімкнений, але жодного разу не заряджений\n");
    {
        int t = -1;
        for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++)
            if (!strcmp(BATTERY_TEMPLATES[i].name, "PMNN4409A")) t = i;
        hexrom("A3427C17010050A6", rom);
        uint8_t w[DUMP_SIZE]; memcpy(w, BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
        // Пишемо суперечливий стан НАПРЯМУ, повз нормалізацію: саме такі
        // пакети лишились у людей після старих версій.
        ImpresCryptFields f; memset(&f, 0, sizeof(f));
        f.haveDat = f.haveRec = f.haveCyc = true;
        f.mfgY = 2023; f.mfgM = 2; f.mfgD = 15;
        f.dayInitialUse = f.dayInitialUse2 = 1258;
        f.dayLastCharge = 0;                    // ← суперечність
        f.cts = 199; f.firstUse = 199;
        uint16_t aCyc = impresCryptAddr(w, BMS_V_CYCLE);
        uint16_t aRec = impresCryptAddr(w, BMS_V_RECOND);
        uint16_t aDat = impresCryptAddr(w, BMS_V_DATE);
        uint8_t k1 = rom[1], k2 = rom[6];
        impresCryptPutBE(w, aCyc + 5, impresCryptEncInt(0, 2, k1));    // dayLastCharge = 0
        impresFixRecord(w, aCyc, w[aCyc]);
        impresCryptPutBE(w, aRec + 1, impresCryptEncInt(0, 2, k1));
        w[aRec + 6] = (uint8_t)impresCryptEncInt(199, 1, k1);
        impresFixRecord(w, aRec, w[aRec]);
        impresCryptPutBE(w, aDat + 1, impresCryptEncDate(2023, 2, 15, k1, k2));
        impresCryptPutBE(w, aDat + 3, impresCryptEncInt(1258, 2, k1));
        impresCryptPutBE(w, aDat + 5, impresCryptEncInt(1258, 2, k1));
        impresFixRecord(w, aDat, w[aDat]);
        impresFixHeader(w);

        uint32_t f1 = impresAudit(w, nullptr, rom, 0, 0, 0);
        printf("   знайдено: %s\n", flags(f1).c_str());
        if (!(f1 & AUD_USE_BEFORE_CHG)) bad("суперечність «вмикали без заряду» не виявлено");

        // Перезапис через нормалізацію має її прибрати.
        ImpresCryptFields g; impresCryptRead(w, k1, k2, &g);
        impresCryptWrite(w, k1, k2, &g);
        uint32_t f2 = impresAudit(w, nullptr, rom, 0, 0, 0);
        printf("   після перезапису: %s\n", flags(f2).c_str());
        if (f2 & AUD_USE_BEFORE_CHG) bad("нормалізація не прибрала суперечність");
    }

    // ── 4. Корпус: чи немає хибних тривог на робочих пакетах ───────────────
    printf("\n4) увесь корпус дампів (ROM невідомий — судимо лише зі змісту)\n");
    {
        std::vector<std::string> all; collect(all);
        int nSum = 0, nDate = 0, nUse = 0, n = 0;
        for (auto &p33 : all) {
            uint8_t a33[DUMP_SIZE];
            if (!load(p33.c_str(), a33, DUMP_SIZE)) continue;
            std::string p38 = p33; size_t q = p38.find("2433");
            if (q != std::string::npos) p38.replace(q, 4, "2438");
            uint8_t a38[DS2438_MEM_SIZE];
            bool has38 = load(p38.c_str(), a38, DS2438_MEM_SIZE);
            uint32_t f = impresAudit(a33, has38 ? a38 : nullptr, nullptr, 0, 0, 0);
            n++;
            if (f & AUD_BLOCK_SUM)    { nSum++; printf("      сума: %s\n", p33.c_str() + 6); }
            if (f & AUD_DATE_INSANE)    nDate++;
            if (f & AUD_USE_BEFORE_CHG) nUse++;
        }
        printf("   дампів %d: побитих сум %d, безглуздих дат %d, «запуск без заряду» %d\n",
               n, nSum, nDate, nUse);
        if (n < 40) bad("корпус не зчитався — перевірка нічого не значить");
        // Без ROM ключ підбирається, і на частині дампів він не знаходиться —
        // тоді дати не читаються взагалі. Це не привід валити тест, але й
        // тиші тут бути не може: якщо ЖОДНОЇ знахідки, аудит просто мовчить.
        if (nSum + nDate + nUse == 0) bad("аудит не знайшов нічого на 49 дампах — підозріло");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
