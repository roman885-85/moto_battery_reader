// ===========================================================================
//  ПРАВИЛА МАЙСТРА ВІДНОВЛЕННЯ: повнота, порядок і те, що знахідка має ліки.
//
//  ⚑ ЧОМУ ЦЕЙ ТЕСТ З'ЯВИВСЯ ТАК ПІЗНО. Правила жили в recovery.h — файлі, який
//  на хості не збирається взагалі (глобальні дампи, дисплей, SPIFFS). Тобто
//  найважливіша частина Майстра — перелік проблем, таблиця діагнозів і порядок
//  кроків — не була покрита нічим і трималась на уважності. Трималась погано:
//
//   • ISS_DCA_INSANE додали, показали користувачеві — і не дали жодного кроку,
//     тобто Майстер називав біду й мовчав про ліки;
//   • AUD_MONITOR_ZEROED аудит рахував, а Майстер про нього не знав узагалі —
//     біт нікуди не доходив;
//   • детектор формату існував у ДВОХ копіях (wizDetectFormat у recovery.h і
//     impresFormatYear у impres_format.h), і працювала не та, що перевірялась.
//
//  Правила винесено в wizard_rules.h — чисті дані й чиста wizPlanIssues().
//  Тут вони ганяються напряму.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <set>

// operations.h оголошує ці наперед (вони живуть у charge.h/discharge.h);
// для правил Майстра досить сталих значень — так само, як у menu_check.
#include "radio_mode.h"   // RADIO_* потрібні заглушці radioMode() нижче

inline uint16_t dischargeTargetMv()  { return 7200; }
inline uint8_t  chargeTargetPct()    { return 100; }
inline uint8_t  chargeProfile()      { return 0; }
inline uint16_t chargeManualMa()     { return 0; }
inline uint16_t chargeManualMv()     { return 0; }
inline uint8_t  dischargeProfile()   { return 0; }
inline uint16_t dischargeManualMa()  { return 0; }

#include "settings.h"
#include "wizard_rules.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

// Усі проблеми поіменно — щоб повідомлення тесту називали біт словами, а не
// числом. Список навмисно РУЧНИЙ: додав ISS_* у wizard_rules.h і забув тут —
// перевірка «жодного біта не пропущено» нижче про це скаже.
struct Named { uint32_t bit; const char *name; };
static const Named ISSUES[] = {
    { ISS_NO_CHIP,          "ISS_NO_CHIP" },
    { ISS_BLANK33,          "ISS_BLANK33" },
    { ISS_HDR_BAD,          "ISS_HDR_BAD" },
    { ISS_NO_MODEL,         "ISS_NO_MODEL" },
    { ISS_MIRROR_BAD,       "ISS_MIRROR_BAD" },
    { ISS_CCA_OVERFLOW,     "ISS_CCA_OVERFLOW" },
    { ISS_NO_2438,          "ISS_NO_2438" },
    { ISS_NEEDS_CALIB,      "ISS_NEEDS_CALIB" },
    { ISS_HEALTH_LOW,       "ISS_HEALTH_LOW" },
    { ISS_TAIL_FOREIGN,     "ISS_TAIL_FOREIGN" },
    { ISS_TAIL_ERASED,      "ISS_TAIL_ERASED" },
    { ISS_NEVER_CALIB,      "ISS_NEVER_CALIB" },
    { ISS_CRYPT_WRONG,      "ISS_CRYPT_WRONG" },
    { ISS_CRYPT_UNKNOWN,    "ISS_CRYPT_UNKNOWN" },
    { ISS_BLOCK_SUM,        "ISS_BLOCK_SUM" },
    { ISS_DATE_INSANE,      "ISS_DATE_INSANE" },
    { ISS_ETM_FOREIGN,      "ISS_ETM_FOREIGN" },
    { ISS_USE_BEFORE_CHG,   "ISS_USE_BEFORE_CHG" },
    { ISS_CHARGER_PARTIAL,  "ISS_CHARGER_PARTIAL" },
    { ISS_DCA_INSANE,       "ISS_DCA_INSANE" },
    { ISS_MONITOR_ZEROED,   "ISS_MONITOR_ZEROED" },
};
#define ISSUE_N ((int)(sizeof(ISSUES) / sizeof(ISSUES[0])))

static const Named ACTIONS[] = {
    { ACT_READ, "ACT_READ" },                   { ACT_RESTORE, "ACT_RESTORE" },
    { ACT_REPAIR, "ACT_REPAIR" },               { ACT_RECAL, "ACT_RECAL" },
    { ACT_RECAL_DEEP, "ACT_RECAL_DEEP" },       { ACT_RESET, "ACT_RESET" },
    { ACT_CLEAN, "ACT_CLEAN" },                 { ACT_SETCHARGE_AUTO, "ACT_SETCHARGE_AUTO" },
    { ACT_CHARGE_STATION, "ACT_CHARGE_STATION" },{ ACT_HDRFIX, "ACT_HDRFIX" },
    { ACT_CRYPT, "ACT_CRYPT" },                 { ACT_DCAFIX, "ACT_DCAFIX" },
    { ACT_VERIFY, "ACT_VERIFY" },
};
#define ACTION_N ((int)(sizeof(ACTIONS) / sizeof(ACTIONS[0])))

static std::vector<uint8_t> planOf(uint32_t issues) {
    uint8_t buf[WIZ_MAX_STEPS];
    int n = wizPlanIssues(issues, buf, WIZ_MAX_STEPS);
    return std::vector<uint8_t>(buf, buf + n);
}
static bool has(const std::vector<uint8_t> &p, uint8_t a) {
    for (uint8_t x : p) if (x == a) return true;
    return false;
}
static int posOf(const std::vector<uint8_t> &p, uint8_t a) {
    for (size_t i = 0; i < p.size(); i++) if (p[i] == a) return (int)i;
    return -1;
}
static const char *ruleFor(uint32_t bit) {
    for (size_t i = 0; i < sizeof(RECOVERY_RULES) / sizeof(RECOVERY_RULES[0]); i++)
        if (RECOVERY_RULES[i].issue == bit) return RECOVERY_RULES[i].problem;
    return nullptr;
}

int main() {
    printf("=== ПРАВИЛА МАЙСТРА ВІДНОВЛЕННЯ ===\n");

    // ── 1. Жоден біт проблеми не загублено в переліку тесту ────────────────
    printf("\n1) перелік проблем у тесті збігається з переліком у правилах\n");
    {
        uint32_t seen = 0;
        for (int i = 0; i < ISSUE_N; i++) {
            if (seen & ISSUES[i].bit) bad("той самий біт названо двічі");
            seen |= ISSUES[i].bit;
        }
        // Біти йдуть підряд від 0: якщо в wizard_rules.h додали новий, а сюди
        // ні — маска стане «дірявою» саме на ньому.
        uint32_t solid = 0;
        for (int i = 0; i < ISSUE_N; i++) solid |= (1u << i);
        printf("   проблем %d, маска 0x%06X (суцільна 0x%06X)\n", ISSUE_N, seen, solid);
        check(seen == solid, "біти проблем ідуть підряд — жоден не пропущено в тесті");
    }

    // ── 2. Кожна проблема має діагноз і пропозицію ─────────────────────────
    printf("\n2) кожна проблема пояснена людською мовою\n");
    {
        int miss = 0;
        for (int i = 0; i < ISSUE_N; i++) {
            const char *p = ruleFor(ISSUES[i].bit);
            if (!p || !p[0]) { miss++; printf("      без діагнозу: %s\n", ISSUES[i].name); }
        }
        check(miss == 0, "у кожної проблеми є рядок у таблиці діагнозів");
        for (size_t i = 0; i < sizeof(RECOVERY_RULES) / sizeof(RECOVERY_RULES[0]); i++)
            if (!RECOVERY_RULES[i].fix || !RECOVERY_RULES[i].fix[0])
                bad("діагноз без пропозиції — користувач дізнається про біду й не дізнається, що робити");
        printf("   рядків у таблиці: %d\n",
               (int)(sizeof(RECOVERY_RULES) / sizeof(RECOVERY_RULES[0])));
    }

    // ── 3. ⚑ ГОЛОВНЕ: знахідка без ліків неможлива ─────────────────────────
    //  Кожна проблема або веде до кроку в плані, або стоїть у списку
    //  WIZ_INFO_ONLY з написаною причиною, чому кроку бути не може. Третього
    //  стану немає — саме в ньому тихо опинилась ISS_DCA_INSANE.
    printf("\n3) кожна проблема або лікується кроком, або свідомо лишена без нього\n");
    {
        int planned = 0, info = 0;
        for (int i = 0; i < ISSUE_N; i++) {
            uint32_t b = ISSUES[i].bit;
            // Один біт — і дивимось, чи з'явився в плані бодай один крок,
            // окрім завершальної перевірки (вона є завжди).
            std::vector<uint8_t> p = planOf(b);
            int real = 0;
            for (uint8_t a : p) if (a != ACT_VERIFY) real++;
            bool io = wizIssueInfoOnly(b);
            if (real > 0) planned++;
            if (io)       info++;
            if (real == 0 && !io) {
                printf("      БЕЗ ЛІКІВ: %s — ні кроку, ні рядка в WIZ_INFO_ONLY\n", ISSUES[i].name);
                bad("проблема показується користувачеві, а полагодити її нічим");
            }
            if (real > 0 && io) {
                printf("      СУПЕРЕЧНІСТЬ: %s — і крок є, і записана в «кроку не буде»\n",
                       ISSUES[i].name);
                bad("проблема одночасно лікується і вважається невиліковною");
            }
            if (io && !wizIssueInfoWhy(b)[0])
                bad("рядок у WIZ_INFO_ONLY без пояснення, чому кроку немає");
        }
        printf("   лікуються кроком %d, свідомо без кроку %d, разом %d із %d\n",
               planned, info, planned + info, ISSUE_N);
        check(planned + info == ISSUE_N, "жодна проблема не лишилась поза двома станами");
        check(info < planned, "«невиліковних» менше, ніж лікованих — інакше Майстер здебільшого лише скаржиться");
    }

    // ── 4. Кожна дія має назву, опис і код ─────────────────────────────────
    printf("\n4) кожна дія підписана\n");
    {
        std::set<std::string> ids;
        for (int i = 0; i < ACTION_N; i++) {
            const char *t = nullptr, *d = nullptr; bool ext = false; uint8_t chips = 0;
            wizActionMeta(ACTIONS[i].bit, &t, &d, &ext, &chips);
            if (!t || !t[0] || strcmp(t, "—") == 0) {
                printf("      без назви: %s\n", ACTIONS[i].name); bad("крок без назви");
            }
            if (!d || !d[0]) { printf("      без опису: %s\n", ACTIONS[i].name); bad("крок без опису"); }
            const char *id = wizActionId(ACTIONS[i].bit);
            if (!id || strcmp(id, "none") == 0) {
                printf("      без коду: %s\n", ACTIONS[i].name); bad("крок без рядкового коду для клієнтів");
            } else if (!ids.insert(id).second) {
                printf("      код повторюється: %s -> %s\n", ACTIONS[i].name, id);
                bad("два кроки з однаковим кодом — клієнт їх не розрізнить");
            }
        }
        printf("   дій %d, унікальних кодів %d\n", ACTION_N, (int)ids.size());
        check((int)ids.size() == ACTION_N, "коди дій унікальні");
    }

    // ── 5. Куди пише крок — сказано прямо ──────────────────────────────────
    //  Плутанина між DS2433 (ідентичність) і DS2438 (монітор із заводським
    //  калібруванням вимірювача струму) коштує дорого, тож кожен крок, який
    //  ПИШЕ, мусить називати мікросхему.
    printf("\n5) кожен крок, що пише, називає мікросхему\n");
    {
        const uint8_t writers[] = { ACT_RESTORE, ACT_REPAIR, ACT_RECAL, ACT_RECAL_DEEP,
                                    ACT_RESET, ACT_CLEAN, ACT_SETCHARGE_AUTO,
                                    ACT_HDRFIX, ACT_CRYPT, ACT_DCAFIX };
        for (uint8_t a : writers) {
            const char *t, *d; bool e; uint8_t chips = OPC_NONE;
            wizActionMeta(a, &t, &d, &e, &chips);
            if (chips == OPC_NONE) { printf("      не сказано куди пише: %s\n", t); bad("крок пише, а куди — не каже"); }
        }
        // І навпаки: читання та зовнішній крок писати не мусять.
        for (uint8_t a : { (uint8_t)ACT_READ, (uint8_t)ACT_CHARGE_STATION, (uint8_t)ACT_VERIFY }) {
            const char *t, *d; bool e; uint8_t chips = OPC_NONE;
            wizActionMeta(a, &t, &d, &e, &chips);
            if (chips != OPC_NONE) bad("крок, який нічого не пише, позначено як записувальний");
        }
        const char *t, *d; bool ext = false;
        wizActionMeta(ACT_CHARGE_STATION, &t, &d, &ext);
        check(ext, "калібрування на ЗП позначено як ЗОВНІШНІЙ крок — його робить людина");
        wizActionMeta(ACT_DCAFIX, &t, &d, &ext);
        check(!ext, "правка лічильника — крок самого пристрою, а не людини");
        uint8_t chips = OPC_NONE;
        wizActionMeta(ACT_DCAFIX, &t, &d, &ext, &chips);
        check(chips == OPC_38, "…і пише вона саме в монітор DS2438, а не в DS2433");
    }

    // ── 6. Порядок кроків ──────────────────────────────────────────────────
    printf("\n6) порядок кроків у плані\n");
    {
        std::vector<uint8_t> p = planOf(ISS_HDR_BAD | ISS_CRYPT_WRONG | ISS_NEEDS_CALIB);
        check(posOf(p, ACT_REPAIR) >= 0 && posOf(p, ACT_CRYPT) > posOf(p, ACT_REPAIR),
              "перешифрування ПІСЛЯ ремонту структури");
        check(posOf(p, ACT_CHARGE_STATION) > posOf(p, ACT_CRYPT),
              "…а станція — після перешифрування: на ЗП їде вже читаний пакет");
        check(p.back() == ACT_VERIFY, "перевірка результату — завжди останній крок");

        // Відновлення еталона саме перешифровує, тож окремий крок зайвий.
        std::vector<uint8_t> q = planOf(ISS_BLANK33 | ISS_CRYPT_WRONG);
        check(has(q, ACT_RESTORE) && !has(q, ACT_CRYPT),
              "після відновлення еталона окремого перешифрування не пропонують");

        // Порожній набір проблем — жодної роботи, крім підтвердження.
        std::vector<uint8_t> e = planOf(0);
        check(e.size() == 1 && e[0] == ACT_VERIFY, "здоровому пакету пропонують лише перевірку");

        // «Чіпів немає» обриває план: решта кроків нема на чому виконувати.
        std::vector<uint8_t> nc = planOf(ISS_NO_CHIP | ISS_HDR_BAD | ISS_NEEDS_CALIB);
        check(nc.size() == 1 && nc[0] == ACT_READ,
              "без чіпів план — рівно «зчитати», без списку робіт над порожнечею");

        // План не переповнює масив, навіть коли зламано все одразу.
        // ⚑ БЕЗ ISS_NO_CHIP: він обриває план першим кроком, і перевірка стелі
        //  стала б порожньою — саме це вона й показала при першому запуску
        //  («кроків 1 зі стелі 8»).
        uint32_t all = 0;
        for (int i = 0; i < ISSUE_N; i++)
            if (ISSUES[i].bit != ISS_NO_CHIP) all |= ISSUES[i].bit;
        std::vector<uint8_t> big = planOf(all);
        printf("   «зламано все, крім зв'язку»: кроків %d зі стелі %d:", (int)big.size(), WIZ_MAX_STEPS);
        for (uint8_t a : big) { const char *t, *d; bool e; wizActionMeta(a, &t, &d, &e); printf(" [%s]", t); }
        printf("\n");
        check((int)big.size() > 2, "на «зламано все» план справді довгий — перевірка стелі не порожня");
        check((int)big.size() <= WIZ_MAX_STEPS, "…і все одно не виходить за стелю");
    }

    // ── 7. ⚑ ПРАВКА ЛІЧИЛЬНИКА РОЗРЯДУ — те, заради чого все й почалось ────
    printf("\n7) побитий лічильник розряду тепер лікується\n");
    {
        std::vector<uint8_t> p = planOf(ISS_DCA_INSANE);
        check(has(p, ACT_DCAFIX), "сам по собі побитий DCA дає крок правки");
        printf("   план: ");
        for (uint8_t a : p) { const char *t, *d; bool e; wizActionMeta(a, &t, &d, &e); printf("[%s] ", t); }
        printf("\n");

        // ⚑ А ОСЬ ТУТ КРОКУ БУТИ НЕ МАЄ. Скидання лічильників і підготовка до
        //  калібрування обнуляють монітор самі, тож правити DCA перед ними —
        //  це робота, яку наступний крок зітре, і зайвий пункт у плані.
        for (uint32_t with : { (uint32_t)ISS_HEALTH_LOW, (uint32_t)ISS_NEEDS_CALIB,
                               (uint32_t)ISS_TAIL_FOREIGN, (uint32_t)ISS_CCA_OVERFLOW,
                               (uint32_t)ISS_TAIL_ERASED }) {
            std::vector<uint8_t> q = planOf(ISS_DCA_INSANE | with);
            if (has(q, ACT_DCAFIX)) {
                printf("      зайвий крок разом із 0x%X\n", with);
                bad("правку лічильника пропонують перед кроком, який його однаково обнулить");
            }
        }
        check(true, "…і не пропонують перед скиданням чи підготовкою до калібрування");

        // Разом із ремонтом структури — навпаки, ПОТРІБНА: ремонт лічильників
        // монітора не чіпає.
        std::vector<uint8_t> r = planOf(ISS_DCA_INSANE | ISS_BLOCK_SUM);
        check(has(r, ACT_REPAIR) && has(r, ACT_DCAFIX),
              "разом із ремонтом сум правка лічильника лишається — ремонт монітор не чіпає");
        check(posOf(r, ACT_DCAFIX) > posOf(r, ACT_REPAIR),
              "…і йде після нього: спершу структура, потім числа");
    }

    // ── 8. Проблеми, знайдені партією дампів 20 ────────────────────────────
    printf("\n8) знахідки партії 20-vymahaie-vidnovlennya доходять до Майстра\n");
    {
        check(ruleFor(ISS_DCA_INSANE) != nullptr,  "побитий лічильник розряду має діагноз");
        check(ruleFor(ISS_MONITOR_ZEROED) != nullptr,
              "обнулений монітор при цілій історії має діагноз (раніше біт нікуди не доходив)");
        check(wizIssueInfoOnly(ISS_MONITOR_ZEROED),
              "…і свідомо лишений без автоматичного кроку: монітор обнуляють НАВМИСНО");
        check(!wizIssueInfoOnly(ISS_DCA_INSANE),
              "…а побитий лічильник — навпаки, лікується автоматично");
    }

    // ── Наробіток: крок є, і стоїть ПІСЛЯ калібрування ─────────────────────
    printf("\n9) наробіток більший за вік: крок є, і він після станції\n");
    {
        uint8_t a[WIZ_MAX_STEPS];
        int n = wizPlanIssues(ISS_ETM_FOREIGN, a, WIZ_MAX_STEPS);
        int iEtm = -1;
        for (int i = 0; i < n; i++) if (a[i] == ACT_ETMFIX) iEtm = i;
        check(iEtm >= 0, "проблема «наробіток більший за вік» тепер має крок");
        check(!wizIssueInfoOnly(ISS_ETM_FOREIGN),
              "…і більше не значиться свідомо залишеною без кроку");

        // ⚑ ПОРЯДОК ТУТ — НЕ КОСМЕТИКА. ЗП переписує наробіток своїм числом,
        //  тож правка ДО станції витрачається даремно.
        n = wizPlanIssues(ISS_ETM_FOREIGN | ISS_NEEDS_CALIB, a, WIZ_MAX_STEPS);
        int iSt = -1; iEtm = -1;
        for (int i = 0; i < n; i++) {
            if (a[i] == ACT_CHARGE_STATION) iSt = i;
            if (a[i] == ACT_ETMFIX)         iEtm = i;
        }
        check(iSt >= 0 && iEtm > iSt, "крок наробітку стоїть ПІСЛЯ калібрування на станції");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
