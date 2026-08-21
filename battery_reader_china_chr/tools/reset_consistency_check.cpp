// ===========================================================================
//  СКИДАННЯ ЛІЧИЛЬНИКІВ НЕ СМІЄ ЛАМАТИ ОРИГІНАЛЬНІСТЬ ПАКЕТА
//
//  Скарга власника: «повністю робочий акумулятор після обнулення лічильників,
//  напрацювання, вироблення перестає бачитись як оригінальний».
//
//  Перевіряємо на ВСЬОМУ корпусі dumps/: беремо кожну справжню пару
//  2433+2438, проганяємо через операції скидання і дивимось, чи вціліли
//  інваріанти, за якими рація впізнає фірмовий пакет:
//
//    * сума заголовка DS2433 (Σ ≡ 0x41);
//    * суми TLV-записів (Σ ≡ 0x5A);
//    * ДЗЕРКАЛО DS2438[24:50] ≡ DS2433[1:27];
//    * конфіг монітора (статус, поріг, стале 0x0F) — не 0xFF і не нуль;
//    * аудит змісту не набуває НОВИХ знахідок.
//
//  Головне правило тесту: операція має право лишити пакет таким самим або
//  кращим, але НЕ гіршим. Усе, що було цілим до скидання, мусить лишитись
//  цілим після.
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
#include "impres_bms.h"
#include "impres_crypt.h"
#include "impres_audit.h"
#include "templates.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

static bool load(const char *p, uint8_t *b, size_t n) {
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t g = fread(b, 1, n, f); fclose(f); return g == n;
}

// Усі пари 2433+2438 з корпусу (як в audit_check).
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
static std::string pair38(const std::string &p33) {
    std::string p = p33; size_t i = p.rfind("2433");
    if (i != std::string::npos) p.replace(i, 4, "2438");
    return p;
}

// ── інваріанти ─────────────────────────────────────────────────────────────
// Скільки TLV-записів мають ПОБИТУ суму. Ідемо ланцюгом від 0x120, як і
// прошивка; на 0xFF ланцюг закінчується.
static int brokenRecords(const uint8_t *d33) {
    int n = 0;
    for (int off = 0x120; off < IMPRES_33_SIZE - 2; ) {
        uint8_t len = d33[off];
        if (len == 0xFF || len < 2 || off + len > IMPRES_33_SIZE) break;
        if (!impresRecordOk(d33, off)) n++;
        off += len;
    }
    return n;
}
// Конфіг монітора притомний: статус не стертий, поріг не стертий.
static bool monitorConfigSane(const uint8_t *d38) {
    return d38[0x00] != 0xFF && d38[0x07] != 0xFF;
}

int main() {
    std::vector<std::string> files;
    collect(files);
    printf("Корпус: %d пар DS2433 у dumps/\n", (int)files.size());
    if (files.empty()) { printf("дампів немає — нічого перевіряти\n"); return 0; }

    uint8_t d33[DUMP_SIZE], d38[DS2438_MEM_SIZE];
    uint8_t w33[DUMP_SIZE], w38[DS2438_MEM_SIZE];

    // ── 1. impresResetMonitor(): дзеркало й конфіг ─────────────────────────
    printf("\n1) impresResetMonitor() — лічильники в нуль, дзеркало й конфіг цілі\n");
    {
        int seen = 0, mirrorBrokeBefore = 0, mirrorBrokeAfter = 0;
        int cfgBad = 0, notZeroed = 0, d33Touched = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            seen++;
            bool mirrorWas = impresMirrorOk(d33, d38);
            if (!mirrorWas) mirrorBrokeBefore++;
            memcpy(w33, d33, DUMP_SIZE); memcpy(w38, d38, DS2438_MEM_SIZE);
            impresResetMonitor(w38, w33, 0x80);
            if (!impresMirrorOk(w33, w38)) mirrorBrokeAfter++;
            if (!monitorConfigSane(w38)) cfgBad++;
            if (impresEtm(w38) || impresCca(w38) || impresDca(w38)) notZeroed++;
            if (memcmp(w33, d33, DUMP_SIZE)) d33Touched++;
        }
        printf("   пар із монітором: %d; дзеркало було побите в %d\n", seen, mirrorBrokeBefore);
        check(seen > 0, "корпус містить пари 2433+2438");
        check(mirrorBrokeAfter == 0, "після скидання дзеркало ЗАВЖДИ узгоджене");
        check(cfgBad == 0, "конфіг монітора лишається притомним");
        check(notZeroed == 0, "ETM/CCA/DCA справді обнулені");
        check(d33Touched == 0, "DS2433 не змінюється взагалі");
    }

    // ── 2. «Скидання/очистка» з web_server.h — ТА САМА послідовність ───────
    //  ⚑ resetBatteryData() живе у web_server.h, який на хості не збирається,
    //  тож повторюємо тут РІВНО її логіку: спершу історія в DS2433 під ключем
    //  цього чипа, і лише якщо це вдалось — лічильники монітора. Охоронець у
    //  session_guard_check стежить, щоб текст функції звідти не розійшовся з
    //  тим, що описано тут.
    printf("\n2) скидання лічильників (/api/reset, /api/clean) на живому корпусі\n");
    {
        int seen = 0, hdrBrokeAfter = 0, recBrokeNew = 0, mirrorBrokeNew = 0, auditNew = 0, skipped = 0;
        std::string worst;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            // Беремо лише пакети, ЦІЛІ до операції: питання тесту — чи псує
            // операція справне, а не чи лікує вона поламане.
            if (!impresHeaderOk(d33)) continue;
            if (!impresMirrorOk(d33, d38)) continue;
            seen++;
            int recWas = brokenRecords(d33);
            uint32_t audWas = impresAudit(d33, d38, nullptr, 0, 0, 0);

            memcpy(w33, d33, DUMP_SIZE); memcpy(w38, d38, DS2438_MEM_SIZE);
            // --- тіло resetBatteryData() ---
            uint8_t rom[8] = { 0x23, 0xA3, 0x11, 0x22, 0x33, 0x44, 0x7C, 0x99 };
            if (impresHistoryZero(w33, rom)) {
                for (int i = 8; i <= 11; i++) w38[i] = 0;
                w38[60] = w38[61] = 0;
                w38[62] = w38[63] = 0;
            } else skipped++;
            // --- factoryCleanData() ---
            impresFixHeader(w33);

            if (!impresHeaderOk(w33)) { hdrBrokeAfter++; if (worst.empty()) worst = p; }
            if (brokenRecords(w33) > recWas) { recBrokeNew++; if (worst.empty()) worst = p; }
            if (!impresMirrorOk(w33, w38)) { mirrorBrokeNew++; if (worst.empty()) worst = p; }
            uint32_t audIs = impresAudit(w33, w38, nullptr, 0, 0, 0);
            if (audIs & ~audWas) { auditNew++; if (worst.empty()) worst = p; }
        }
        printf("   цілих пакетів у корпусі: %d (з них відмовились скидати: %d)\n",
               seen, skipped);
        if (!worst.empty()) printf("   перший постраждалий: %s\n", worst.c_str());
        check(seen > 0, "у корпусі є цілі пакети, на яких можна перевіряти");
        check(hdrBrokeAfter == 0, "заголовок лишається цілим");
        check(recBrokeNew == 0, "жоден TLV-запис не ламається");
        check(mirrorBrokeNew == 0, "дзеркало лишається узгодженим");
        check(auditNew == 0, "аудит не набуває НОВИХ знахідок");
    }

    // ── 3. Свіжий хвіст: структура лишається валідною ──────────────────────
    printf("\n3) свіжий навчений хвіст (підготовка до калібрування)\n");
    {
        int seen = 0, hdrBad = 0, recNew = 0, tailBad = 0, auditNew = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            char m[16] = "";
            if (!impresModelName(d33, m, sizeof(m))) continue;
            int t = -1;
            for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++)
                if (!strcmp(BATTERY_TEMPLATES[i].name, m)) t = i;
            if (t < 0 || !BATTERY_TEMPLATES[t].fresh) continue;
            seen++;
            int recWas = brokenRecords(d33);
            uint32_t audWas = impresAudit(d33, nullptr, nullptr, 0, 0, 0);
            memcpy(w33, d33, DUMP_SIZE);
            uint8_t fresh[IMPRES_FRESH_TAIL_LEN];
            memcpy(fresh, BATTERY_TEMPLATES[t].fresh, IMPRES_FRESH_TAIL_LEN);
            if (impresResetTailFrom(w33, fresh) < 0) { tailBad++; continue; }
            impresFixHeader(w33);
            if (!impresHeaderOk(w33)) hdrBad++;
            if (brokenRecords(w33) > recWas) recNew++;
            uint32_t audIs = impresAudit(w33, nullptr, nullptr, 0, 0, 0);
            if (audIs & ~audWas) auditNew++;
        }
        printf("   пакетів із перевіреним шаблоном: %d\n", seen);
        check(seen > 0, "у корпусі є моделі з вшитим свіжим хвостом");
        check(tailBad == 0, "свіжий хвіст застосовується без відмов");
        check(hdrBad == 0, "заголовок після свіжого хвоста цілий");
        check(recNew == 0, "свіжий хвіст не ламає TLV-записів");
        check(auditNew == 0, "свіжий хвіст не додає знахідок аудиту");
    }

    // ── 4. Перешифрування під СВІЙ ROM — числа не міняються ────────────────
    //  Перевірка оборотності: розшифрувати ключем A, зашифрувати ключем B,
    //  розшифрувати ключем B — мусимо отримати те саме, що й на вході.
    printf("\n4) перешифрування оборотне: числа переживають зміну ключа\n");
    {
        int seen = 0, mismatch = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            // Ключі беремо довільні, але різні: тест про арифметику, а не про
            // конкретний чип.
            const uint8_t kA1 = 0x42, kA2 = 0x7C, kB1 = 0xA3, kB2 = 0x50;
            ImpresCryptFields a, b;
            impresCryptRead(d33, kA1, kA2, &a);
            if (!a.haveCyc && !a.haveRec && !a.haveDat) continue;
            seen++;
            memcpy(w33, d33, DUMP_SIZE);
            impresCryptWrite(w33, kB1, kB2, &a);
            impresCryptRead(w33, kB1, kB2, &b);
            // Порівнюємо через нормалізацію: запис свідомо приводить поля до
            // несуперечливого стану, тож еталон теж треба нормалізувати.
            impresCryptNormalize(&a);
            if (a.haveCyc != b.haveCyc || a.haveRec != b.haveRec || a.haveDat != b.haveDat ||
                a.cyclesEnc != b.cyclesEnc || a.reverts != b.reverts ||
                a.dayLastCharge != b.dayLastCharge || a.topOffCycles != b.topOffCycles ||
                a.calCycles != b.calCycles || a.dayLastRecond != b.dayLastRecond ||
                a.firstUse != b.firstUse || a.cts != b.cts ||
                a.mfgY != b.mfgY || a.mfgM != b.mfgM || a.mfgD != b.mfgD ||
                a.dayInitialUse != b.dayInitialUse)
                mismatch++;
        }
        printf("   дампів із зашифрованими блоками: %d\n", seen);
        check(seen > 0, "у корпусі є зашифровані блоки");
        check(mismatch == 0, "після зміни ключа всі числа читаються ті самі");
    }

    // ── 5. Запис нуля в лічильники циклів — і читання того самого нуля ─────
    printf("\n5) обнулення лічильників циклів читається як нуль\n");
    {
        int seen = 0, wrong = 0, recNew = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            memcpy(w33, d33, DUMP_SIZE);
            int recWas = brokenRecords(w33);
            if (!impresCyclesWrite(w33, 0)) continue;
            seen++;
            if (impresBmsCyclesFromHist(w33, impresBmsVector(w33, BMS_V_ADDED)) != 0) wrong++;
            if (brokenRecords(w33) > recWas) recNew++;
        }
        printf("   дампів із гістограмою циклів: %d\n", seen);
        check(seen > 0, "у корпусі є блок гістограми");
        check(wrong == 0, "після запису нуля лічильник читається нулем");
        check(recNew == 0, "запис лічильника не ламає TLV-записів");
    }

    // ── 6. ДОКАЗ ІЗ КОРПУСУ: обнулений монітор — прикмета відкинутого пакета ─
    printf("\n6) корпус: ETM==0 буває рівно там, де рація сказала «невідомий»\n");
    {
        int tot = 0, etm0 = 0, etm0used = 0, from08 = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            tot++;
            if (impresEtm(d38) != 0) continue;
            etm0++;
            if (p.find("08-nova-batareya") != std::string::npos) from08++;
            int cyc = impresBmsCyclesFromHist(d33, impresBmsVector(d33, BMS_V_ADDED));
            if (cyc > 0) etm0used++;
            printf("   %s: ETM 0, циклів %d, CCA %u\n",
                   p.c_str() + 6, cyc, impresCca(d38));
        }
        printf("   цілих пакетів: %d, з них ETM==0: %d\n", tot, etm0);
        check(tot > 40, "корпус достатньо великий, щоб робити висновок");
        // Головне число всієї цієї історії: у ЖИВИХ пакетів напрацювання не
        // нульове. Нуль трапляється лише там, де пакет уже не впізнавався.
        check(etm0 <= 2, "обнулений монітор — рідкісний виняток, а не норма");
        check(etm0 == from08,
              "усі випадки ETM==0 — з 08-nova-batareya, тобто з відкинутого пакета");
        check(etm0used == etm0,
              "і в кожному з них DS2433 суперечить монітору: історія є, напрацювання нема");
    }

    // ── 7. Аудит ЛОВИТЬ цю неузгодженість ──────────────────────────────────
    printf("\n7) аудит бачить «монітор обнулено, а історія лишилась»\n");
    {
        int flagged = 0, falseAlarm = 0, seen = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            seen++;
            uint32_t f = impresAudit(d33, d38, nullptr, 0, 0, 0);
            bool zeroed = (impresEtm(d38) == 0 && impresCca(d38) == 0);
            if (f & AUD_MONITOR_ZEROED) { flagged++; if (!zeroed) falseAlarm++; }
        }
        printf("   позначено пакетів: %d із %d\n", flagged, seen);
        check(flagged >= 2, "обидва пакети з 08-nova-batareya позначені");
        check(falseAlarm == 0, "жодного хибного спрацювання на живих пакетах");
        check(flagged < seen / 4, "знахідка рідкісна — це діагноз, а не шум");
    }

    // ── 8. УЗГОДЖЕНЕ скидання НЕ створює цього стану ───────────────────────
    //  Найважливіший розділ файлу: він відрізняє «як було» від «як стало».
    printf("\n8) узгоджене скидання (impresHistoryZero + монітор) — стан законний\n");
    {
        int seen = 0, badOld = 0, badNew = 0, dateLost = 0, notZeroed = 0, refused = 0;
        uint8_t rom[8] = { 0x23, 0xA3, 0x11, 0x22, 0x33, 0x44, 0x7C, 0x99 };
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            // Беремо лише пакети, у яких Є історія: саме на них стара
            // поведінка й ламалась.
            int cyc = impresBmsCyclesFromHist(d33, impresBmsVector(d33, BMS_V_ADDED));
            if (cyc <= 0) continue;
            seen++;

            // --- ЯК БУЛО: обнулили лише монітор ---
            memcpy(w33, d33, DUMP_SIZE); memcpy(w38, d38, DS2438_MEM_SIZE);
            for (int i = 8; i <= 11; i++) w38[i] = 0;
            w38[60] = w38[61] = w38[62] = w38[63] = 0;
            impresFixHeader(w33);
            if (impresAudit(w33, w38, nullptr, 0, 0, 0) & AUD_MONITOR_ZEROED) badOld++;

            // --- ЯК СТАЛО: обнулили ОБИДВА боки ---
            memcpy(w33, d33, DUMP_SIZE); memcpy(w38, d38, DS2438_MEM_SIZE);
            // Дату виготовлення запам'ятовуємо ключем, яким її ЗАРАЗ записано.
            ImpresCryptFields before;
            impresCryptRead(w33, rom[1], rom[6], &before);
            bool zeroed = impresHistoryZero(w33, rom);
            // ⚑ Монітор чіпаємо ЛИШЕ якщо DS2433 удалось привести до ладу —
            //  рівно так, як це робить resetBatteryData(). Саме ця умова й
            //  відрізняє узгоджене скидання від старого.
            if (zeroed) {
                for (int i = 8; i <= 11; i++) w38[i] = 0;
                w38[60] = w38[61] = w38[62] = w38[63] = 0;
                if (impresBmsCyclesFromHist(w33, impresBmsVector(w33, BMS_V_ADDED)) != 0)
                    notZeroed++;
            } else {
                refused++;
            }
            if (impresAudit(w33, w38, nullptr, 0, 0, 0) & AUD_MONITOR_ZEROED) badNew++;
            // Дата виготовлення мусить ПЕРЕЖИТИ скидання — це єдиний
            // достовірний факт про походження пакета.
            ImpresCryptFields after;
            impresCryptRead(w33, rom[1], rom[6], &after);
            if (before.haveDat && after.haveDat &&
                (before.mfgY != after.mfgY || before.mfgM != after.mfgM ||
                 before.mfgD != after.mfgD)) dateLost++;
        }
        printf("   пакетів з історією: %d; стара поведінка створила суперечність у %d\n",
               seen, badOld);
        check(seen > 20, "у корпусі досить пакетів з історією");
        check(badOld > 0, "стара поведінка справді створювала суперечливий стан");
        check(badNew == 0, "узгоджене скидання не створює його ЖОДНОГО разу");
        check(notZeroed == 0, "лічильник циклів після скидання справді нульовий");
        check(dateLost == 0, "дата виготовлення переживає скидання незмінною");
        printf("   відмовились скидати (не вийшло обнулити гістограму): %d\n", refused);
        check(refused < seen / 4,
              "відмова — рідкісний виняток, а не звичайний результат");
    }

    // ── 9. Наробіток, записаний у САМОМУ пакеті ────────────────────────────
    //  Скарга власника з трьома дампами поспіль (PMNN4409A): скинули всі
    //  лічильники, вставили пакет у зарядну станцію, витягли — а в моніторі
    //  знову 4797/4659. Причина не в станції: у блоці NONSMART, поруч із
    //  лічильником не-IMPRES, лежить ще й пара CCA/DCA у сирих одиницях
    //  монітора — і жоден шлях скидання її не чіпав. Станція взяла числа
    //  звідти (доказ: у монітор поїхало 4797, а не колишнє монітірне 4798 —
    //  тобто саме те, що лежало в DS2433; паливомір, який ми теж переписали,
    //  станція не чіпала, отже знімок вона не відновлює).
    printf("\n9) наробіток у самому пакеті теж обнуляється — інакше станція його поверне\n");
    {
        int seen = 0, hadHist = 0, leftBehind = 0, brokeSum = 0, refused = 0;
        uint8_t rom[8] = { 0x23, 0xA3, 0x11, 0x22, 0x33, 0x44, 0x7C, 0x99 };
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            uint16_t c0 = 0, d0 = 0;
            if (!impresBmsHistCounters(d33, &c0, &d0)) continue;   // полів немає
            seen++;
            if (c0 || d0) hadHist++;

            memcpy(w33, d33, DUMP_SIZE);
            if (!impresHistoryZero(w33, rom)) { refused++; continue; }
            uint16_t c1 = 0, d1 = 0;
            if (impresBmsHistCounters(w33, &c1, &d1) && (c1 || d1)) leftBehind++;
            // Блок правиться на місці, тож його сума мусить лишитись цілою:
            // інакше «полагодили» історію ціною побитого запису.
            if (!impresRecordOk(w33, impresBmsVector(w33, BMS_V_NONSMART))) brokeSum++;
        }
        printf("   пакетів із цими полями: %d, з них із ненульовою історією: %d\n",
               seen, hadHist);
        check(seen > 20,    "поля наробітку є в переважній більшості пакетів корпусу");
        // Без цього рядка перевірка нижче була б порожньою: обнулити нуль
        // легко, і «після скидання там нуль» нічого б не доводило.
        check(hadHist > 20, "…і майже скрізь вони ненульові — є що обнуляти");
        check(leftBehind == 0, "після скидання наробіток у пакеті теж нульовий");
        check(brokeSum == 0,   "…і сума блока при цьому лишилась цілою");
        printf("   відмов скидати: %d\n", refused);
        // ⚑ САМА ПО СОБІ ПЕРЕВІРКА ВИЩЕ НЕ ЛОВИТЬ ГОЛОВНОГО. Якщо скидання
        //  перестане обнуляти ці поля, воно не «залишить історію» — воно
        //  ВІДМОВИТЬСЯ (звірка результату всередині impresHistoryZero це
        //  побачить), і цикл вище просто пропустить такий пакет, лишившись
        //  зеленим. Тобто без цього рядка вся перевірка тримається на нулі
        //  відмов, який ніхто не стереже. Звірка від протилежного це й
        //  показала: три поломки з чотирьох валили не той рядок.
        check(refused * 4 < seen,
              "скидання не почало відмовляти масово — воно працює, а не капітулює");

        // Окремо: правка побитого лічильника розряду (3.38) після скидання
        // мусить ВІДМОВИТИСЬ — джерела більше немає, і брати нуль за значення
        // не можна. Дві функції читають ті самі поля, тож розійтись їм легко.
        int wouldFix = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!load(pair38(p).c_str(), d38, DS2438_MEM_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            memcpy(w33, d33, DUMP_SIZE); memcpy(w38, d38, DS2438_MEM_SIZE);
            if (!impresHistoryZero(w33, rom)) continue;
            // ⚑ МОНІТОР СТАВИМО ЗАВІДОМО ПОБИТИЙ, А НЕ НУЛЬОВИЙ. На нульовому
            //  правка відмовиться сама — «ламати нема чого», — і перевірка
            //  нічого не доведе: саме так вона спершу й була написана, і
            //  звірка від протилежного показала, що зламати її неможливо.
            //  Питання тут інше: коли монітор СПРАВДІ треба лагодити, а історії
            //  вже немає, чи не візьме правка нуль за значення.
            w38[60] = 100; w38[61] = 0;          // CCA = 100
            w38[62] = 0x88; w38[63] = 0x13;      // DCA = 5000 — явно неправдоподібно
            if (impresBmsFixDcaFromHist(w33, w38, nullptr)) wouldFix++;
        }
        check(wouldFix == 0,
              "після скидання правка лічильника розряду не має звідки брати число — і не бере");

        // ⚑ БЛОК, ЯКИЙ ЧИТАЧ РОЗБИРАЄ, А ПИСАР — НІ. Читачеві досить довжини
        //  від 8, писар вимагає ще й не більше за 32. У цю щілину й провалилось
        //  би скидання, якби воно не звіряло РЕЗУЛЬТАТ: доповіло б про успіх,
        //  лишивши історію на місці, а викликач обнулив би монітор проти живої
        //  історії — рівно та суперечність, заради якої узгоджене скидання й
        //  писалось.
        int seenLong = 0, refusedLong = 0;
        for (auto &p : files) {
            if (!load(p.c_str(), d33, DUMP_SIZE)) continue;
            if (!impresHeaderOk(d33)) continue;
            uint16_t a = impresBmsVector(d33, BMS_V_NONSMART);
            if (a == BMS_INVALID || (int)a + 40 > IMPRES_33_SIZE) continue;
            uint16_t c0 = 0, d0 = 0;
            if (!impresBmsHistCounters(d33, &c0, &d0) || (!c0 && !d0)) continue;
            memcpy(w33, d33, DUMP_SIZE);
            w33[a] = 40;                    // читається (>=8), не пишеться (>32)
            seenLong++;
            if (!impresHistoryZero(w33, rom)) refusedLong++;
        }
        printf("   блоків «читається, але не пишеться»: %d\n", seenLong);
        check(seenLong > 0, "є на чому це перевірити");
        check(refusedLong == seenLong,
              "коли історію переписати нічим — скидання відмовляється, а не бреше про успіх");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
