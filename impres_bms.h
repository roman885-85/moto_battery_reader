#ifndef IMPRES_BMS_H
#define IMPRES_BMS_H
// ===========================================================================
//  ШТАТНИЙ ДЕКОДЕР IMPRES — те, як читає пакет сама Motorola
// ---------------------------------------------------------------------------
//  Звідки взято. Проєкт rick51231/node-dmr-lib містить робочий декодер BMS
//  (`src/Protocols/BMS/BatteryData.js`) і, головне, ЕТАЛОННУ ВИБІРКУ:
//  `dev/BMS/images/*` — імена файлів це сирі пакети «рація → ПЗ», а самі
//  картинки — знімки екрана фірмової програми Motorola з тими ж пакетами;
//  `dev/BMS/print.md` — та сама вибірка таблицею.
//
//  Декодер портовано і прогнано по 53 еталонних пакетах: ємність, потенційна
//  ємність, паспортна ємність, здоров'я, усі три лічильники циклів і дата
//  першого користування збіглися з фірмовим ПЗ ТОЧНО (єдина розбіжність —
//  ПЗ Motorola ОКРУГЛЮЄ ICA→мА·год, а node-dmr-lib відкидає дробову частину;
//  тут округлюємо, як Motorola). Далі той самий декодер прогнано по всіх 49
//  наших дампах — структура збіглася повністю.
//
//  ЩО ЦЕ ЗМІНИЛО В НАШИХ ВИСНОВКАХ (див. docs/FIRMWARE_ANALYSIS.md §5):
//   • «зносу в прошивці немає» — Є. Це CTS у блоці калібрування, але ЗАШИФРОВАНИЙ.
//   • «дати першого користування немає» — Є. Блок дат, теж зашифрований.
//   • шунт DS2438 не 0.025 Ом «на всіх», а СВІЙ У КОЖНОГО пакета і лежить
//     у самому чипі — DS2438[56..57].
//
// ---------------------------------------------------------------------------
//  1. ТАБЛИЦЯ ВЕКТОРІВ. Блоки шукаються не проходом ланцюга, а за індексом:
//     DS2433[0x41] — кількість блоків N; вектор v (парний, 70..100) лежить
//     у DS2433[v] двома байтами BE і дає зсув блока від початку DS2433.
//     Допустимі вектори: v <= 2*N + 65. Тобто сама таблиця займає
//     DS2433[0x42..0x67] — це всередині «блока моделі» 0x000..0x065, який ми
//     досі описували просто як сталу частину.
//
//  2. ФОРМАТ БЛОКА. [ДОВЖИНА][дані…], Σ байт блока ≡ 0x5A, довжина <= 32 —
//     рівно те правило, яке ми вивели самі. Виняток — блок прапорців ЗП
//     (вектор 74, 3 байти): суми не має. Це збігається з нашим спостереженням
//     у docs (запис `0x1E0`/`0x1E6` — «без контрольної суми»).
//
//  3. ШИФРУВАННЯ. Лічильники й дати зберігаються обертанням вправо на
//     (ключ & 15) біт із подальшим відніманням 0xD8 (для дат — інша схема).
//     КЛЮЧІ БЕРУТЬСЯ З ROM-ID ЧИПА DS2433:
//         key1 = ROM[1]   (молодший байт серійника),
//         key2 = ROM[6]   (старший байт серійника).
//     Тобто прив'язки-CRC до ROM немає (це ми перевірили раніше), але ROM усе
//     ж бере участь: без нього лічильники не розшифрувати. На пристрої ROM у
//     нас є (BatteryReader::rom2433()); для дампа з файлу ключ підбирається
//     перебором — його ефективна довжина лише 4 біти (impresBmsFindKey).
// ===========================================================================

#include <stdint.h>
#include <string.h>
#include "impres_format.h"   // IMPRES_33_SIZE / IMPRES_REC_SUM / impresRatedFromDump

// ---- координати -----------------------------------------------------------
// Вектори таблиці (значення v; сам вектор лежить у DS2433[v], два байти BE).
#define BMS_V_DATE      70    // дата виготовлення, день першого використання
#define BMS_V_ADDITION  72    // хімія, гістерезиси, напруги відновлення
#define BMS_V_STATUS    74    // прапорці ЗП — БЕЗ контрольної суми
#define BMS_V_HOSTID    76    // hostID + версія ПЗ ЗП
#define BMS_V_CYCLE     78    // цикли заряду, реверти, день ост. заряду
#define BMS_V_RECOND    80    // калібрування: CTS (потенційна ємність) тощо
#define BMS_V_RWEIGHT   82    // ваги відновлення
#define BMS_V_ADDED     84    // гістограма ДОДАНОГО заряду (звідси цикли)
#define BMS_V_REMAIN    86    // гістограма ЗАЛИШКУ на момент постановки
#define BMS_V_EOS       88    // кінець строку служби
#define BMS_V_NONSMART  90    // не-IMPRES цикли
#define BMS_V_ERRORS    92    // лічильники помилок
#define BMS_V_DISCHAR   94    // заводська крива розряду
#define BMS_V_KIT       96    // рядок моделі (KIT number)
#define BMS_V_RETENTION 98    // саморозряд: % за першу добу / % за добу
#define BMS_V_CALIB     100   // мінімальний % EOSL, лічильник спроб калібрування

#define BMS_BLOCK_COUNT_AT  0x041   // DS2433[0x41] — скільки всього блоків
#define BMS_INVALID         0xFFFF

// Шунт DS2438: DS2438[56..57], LE, в одиницях 10 мкОм (значення/100000 = Ом).
#define BMS_RSENSE_AT       56
#define BMS_RSENSE_MIN_RAW  500     // 5 мОм  — нижче цього байти явно не шунт
#define BMS_RSENSE_MAX_RAW  10000   // 100 мОм

// Ціна молодшого розряду лічильників DS2438 (даташит, мВ·год):
//   ICA      — 0.4882 мВ·год  -> мА·год = 0.4882 * raw / Rsense
//   CCA/DCA  — 15.625 мВ·год  -> мА·год = 15.625 * raw / Rsense   (у 32 рази більше!)
// Формула Motorola для ICA записана як 1000*raw/(2048*Rs) — це те саме число.
#define BMS_ICA_MVH         0.48828125f
#define BMS_CCA_MVH         15.625f

// ---------------------------------------------------------------- результат
struct ImpresBms {
    bool     ok;              // таблиця векторів прочиталась
    uint8_t  blockCount;      // DS2433[0x41]
    uint16_t addr[16];        // зсуви блоків у DS2433 (індекс = (v-70)/2)
    bool     sumOk[16];       // чи сходиться Σ≡0x5A

    // --- незашифроване ---
    bool     rsenseFromChip;  // шунт узято з чипа, а не з налаштувань
    float    rsense;          // Ом
    int      ratedMah;        // паспортна ємність (DS2433[0x008] * 25)
    uint32_t etmSec;          // напрацювання, с
    uint8_t  ica;
    int      icaMah;          // поточний заряд, мА·год
    uint16_t cca, dca;
    long     ccaMah, dcaMah;  // накопичено заряду / розряду, мА·год
    int      ccaCycles;       // повних еквівалентних циклів заряду
    uint8_t  chemistry;       // 1=NiCd 2=NiMH 3=Li-Ion
    char     kit[14];         // рядок моделі з блока KIT
    int      cycles;          // ⭐ цикли заряду IMPRES — те саме число, що
                              //    показує фірмове ПЗ (з гістограми, БЕЗ ключа)
    int      nonImpresCycles; // цикли не-IMPRES зарядки; -1 = НЕВІДОМО

    // --- зашифроване (потрібен ключ) ---
    bool     haveKey;
    uint8_t  key1, key2;
    bool     keyGuessed;      // ключ підібрано, а не взято з ROM
    int      cyclesEnc;       // внутрішній лічильник циклів (менший за cycles)
    int      reverts;
    int      topOffCycles;
    int      calCycles;       // проведених калібрувань
    int      dayInitialUse;   // діб від дати виготовлення до першого вмикання
    int      dayLastCharge;
    int      dayLastRecond;
    int      cts;             // потенційна ємність у одиницях ICA
    int      potentialMah;    // ⭐ реальна ємність пакета, мА·год
    int      firstUseMah;     // ємність при першому використанні
    int      health;          // ⭐ знос: потенційна / паспортна, %
    int      mfgY, mfgM, mfgD;      // дата виготовлення
    int      useY, useM, useD;      // дата першого користування (0 — невідомо)
};

// ------------------------------------------------------------- дешифрування
// Обертання вправо на (key & 15) біт, далі −0xD8. len: 1 або 2 байти.
inline uint16_t impresBmsDecInt(uint16_t v, int len, uint8_t key) {
    bool is8 = (len == 1);
    for (int n = key & 15; n > 0; n--) {
        if (v & 1) v = (uint16_t)((v >> 1) | (is8 ? 0x80 : 0x8000));
        else       v = (uint16_t)(v >> 1);
    }
    v = (uint16_t)(v - 0xD8);
    return is8 ? (uint16_t)(v & 0xFF) : v;
}

// Дата: обертання на ((key2>>4) ^ (key1&0xF)) біт (ЩОНАЙМЕНШЕ одне — цикл
// do/while в оригіналі), далі +10048 і розбір як дати у форматі FAT.
inline void impresBmsDecDate(uint16_t v, uint8_t key1, uint8_t key2,
                             int *y, int *m, int *d) {
    int k = (key2 >> 4) ^ (key1 & 0x0F);
    do {
        k--;
        if (v & 1) v = (uint16_t)((v >> 1) | 0x8000);
        else       v = (uint16_t)(v >> 1);
    } while (k > 0);
    uint32_t t = (uint32_t)v + 10048;
    *y = (int)(t >> 9) + 1980;
    *m = (int)((t >> 5) & 0x0F);
    *d = (int)(t & 0x1F);
}

inline bool impresBmsDateSane(int y, int m, int d) {
    return y >= 2005 && y <= 2035 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// ── ЧИ ПРАВДОПОДІБНИЙ ЛІЧИЛЬНИК РОЗРЯДУ ────────────────────────────────────
//  З пакета не можна взяти суттєво більше, ніж у нього залили: DCA завжди
//  трохи менший за CCA. Перевищення БУВАЄ (пакет приїхав із заводу зарядженим
//  і його розрядили до першого заряду), але це одиниці відсотків, а не рази.
//
//  ⚑ ЦЕ НЕ ТЕОРІЯ. У дампі 20-vymahaie-vidnovlennya/08 монітор віддає
//  CCA = 559, DCA = 33384 — тобто «розряджено 26 мільйонів мА·год», у 57 разів
//  більше, ніж заряджено, при 191 циклі заряду. Ми показували ці 11465 циклів
//  розряду як факт, поруч зі 191 циклом заряду, і жодна перевірка не
//  заперечувала. Регістр DCA просто побитий.
//
//  Запас навмисно щедрий (півтора рази плюс 200 одиниць): мета — ловити
//  ПОБИТИЙ регістр, а не сперечатися з нормальною експлуатацією.
inline bool impresBmsDcaSane(uint16_t cca, uint16_t dca) {
    return (uint32_t)dca <= (uint32_t)cca + (uint32_t)cca / 2u + 200u;
}

// Додати дні до дати (алгоритм Хауарда Хіннанта days_from_civil/civil_from_days).
inline long impresBmsToDays(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

inline void impresBmsFromDays(long z, int *y, int *m, int *d) {
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}

// ------------------------------------------------------------ доступ до блоків
inline uint16_t impresBmsVector(const uint8_t *d33, int v) {
    uint8_t n = d33[BMS_BLOCK_COUNT_AT];
    if (n == 0 || n == 0xFF) return BMS_INVALID;
    if (v > 2 * (int)n + 65)  return BMS_INVALID;
    uint16_t a = (uint16_t)((d33[v] << 8) | d33[v + 1]);
    return (a >= IMPRES_33_SIZE) ? BMS_INVALID : a;
}

inline bool impresBmsBlockOk(const uint8_t *d33, uint16_t a) {
    if (a == BMS_INVALID) return false;
    uint8_t len = d33[a];
    if (len == 0 || len > 32 || (int)a + len > IMPRES_33_SIZE) return false;
    int s = 0;
    for (int i = 0; i < len; i++) s += d33[a + i];
    return (s & 0xFF) == IMPRES_REC_SUM;
}

inline uint16_t bmsBE(const uint8_t *p, int o) { return (uint16_t)((p[o] << 8) | p[o + 1]); }
inline uint16_t bmsLE(const uint8_t *p, int o) { return (uint16_t)((p[o + 1] << 8) | p[o]); }

// Шунт із чипа. Повертає 0, якщо поле порожнє або неправдоподібне.
inline float impresBmsRsense(const uint8_t *d38) {
    if (!d38) return 0.0f;
    uint16_t raw = bmsLE(d38, BMS_RSENSE_AT);
    if (raw < BMS_RSENSE_MIN_RAW || raw > BMS_RSENSE_MAX_RAW) return 0.0f;
    return raw / 100000.0f;
}

// ── CCA/DCA: сирі одиниці <-> мА·год ────────────────────────────────────────
//  Обидва напрямки поруч і в одному місці. Зворотний потрібен для
//  синхронізації лічильників (mirror_plan.h): накопичений заряд у моніторі
//  доводиться ПЕРЕРАХОВУВАТИ з циклів, які веде сам пакет у DS2433.
inline long impresCcaMahFromRaw(uint16_t raw, float rsOhm) {
    return (rsOhm > 0.0f) ? (long)(BMS_CCA_MVH * raw / rsOhm + 0.5f) : 0;
}
inline uint16_t impresCcaRawFromMah(long mah, float rsOhm) {
    if (rsOhm <= 0.0f || mah <= 0) return 0;
    long r = (long)(mah * rsOhm / BMS_CCA_MVH + 0.5f);
    if (r < 0) r = 0;
    if (r > 65535) r = 65535;
    return (uint16_t)r;
}

// Цикли заряду з гістограми доданого заряду — БЕЗ ключа. Саме це число фірмове
// ПЗ показує як «Total IMPRES charge cycles» (звірено з 53 знімками екрана).
// Нульовий кошик містить суму, решта — розподіл; алгоритм відновлює обидва.
inline int impresBmsCyclesFromHist(const uint8_t *d33, uint16_t a) {
    if (a == BMS_INVALID) return -1;
    long sum = 0, h0 = bmsBE(d33, a + 1);
    for (int i = 0; i < 10; i++) sum += bmsBE(d33, a + 1 + i * 2);
    long rest = sum - h0;
    if (h0 >= rest) h0 -= rest;
    return (int)(rest + h0);
}

// ---------------------------------------------------------------- розбір
//  d33  — 512 Б DS2433 (обов'язково)
//  d38  — 64 Б DS2438 або nullptr
//  rom33— 8 Б ROM-ID чипа DS2433 або nullptr (тоді ключ підбирається)
//  rsFallback — шунт із налаштувань, якщо в чипі поля немає
inline bool impresBmsParse(const uint8_t *d33, const uint8_t *d38,
                           const uint8_t *rom33, float rsFallback,
                           ImpresBms *o) {
    memset(o, 0, sizeof(*o));
    o->cycles = -1;
    // ⚑ −1, А НЕ НУЛЬ, І З ТІЄЇ Ж ПРИЧИНИ, ЩО Й У cycles. Нуль тут — законне
    //  показання («жодного разу не заряджали простою ЗП»), тож видати ним
    //  «блок не читається» означає збрехати конкретним числом. І це не
    //  теорія: у двох дампах партії 20-vymahaie-vidnovlennya блок NONSMART
    //  побитий, у ньому лежать 11 і 58 циклів, а показували ми нуль — і той
    //  самий нуль ішов у план синхронізації дзеркала як «зараз у пакеті».
    o->nonImpresCycles = -1;
    if (!d33) return false;

    // Шунт, паспортна ємність і показники DS2438 не залежать від таблиці
    // векторів — заповнюємо їх навіть для стертого чипа.
    o->rsense = impresBmsRsense(d38);
    o->rsenseFromChip = (o->rsense > 0.0f);
    if (!o->rsenseFromChip) o->rsense = rsFallback;
    o->ratedMah = impresRatedFromDump(d33);

    // --- DS2438 ---
    if (d38) {
        o->etmSec = ((uint32_t)d38[11] << 24) | ((uint32_t)d38[10] << 16) |
                    ((uint32_t)d38[9] << 8) | d38[8];
        o->ica = d38[12];
        o->cca = bmsLE(d38, 60);
        o->dca = bmsLE(d38, 62);
        if (o->rsense > 0.0f) {
            o->icaMah = (int)(BMS_ICA_MVH * o->ica / o->rsense + 0.5f);
            o->ccaMah = impresCcaMahFromRaw(o->cca, o->rsense);
            o->dcaMah = impresCcaMahFromRaw(o->dca, o->rsense);
            if (o->ratedMah > 0) o->ccaCycles = (int)(o->ccaMah / o->ratedMah);
        }
    }

    o->blockCount = d33[BMS_BLOCK_COUNT_AT];
    for (int i = 0; i < 16; i++) {
        o->addr[i]  = impresBmsVector(d33, 70 + i * 2);
        o->sumOk[i] = impresBmsBlockOk(d33, o->addr[i]);
    }
    o->ok = (o->addr[0] != BMS_INVALID);
    if (!o->ok) return false;

    // --- незашифровані блоки ---
    uint16_t aAdd = o->addr[(BMS_V_ADDITION - 70) / 2];
    if (o->sumOk[(BMS_V_ADDITION - 70) / 2]) o->chemistry = d33[aAdd + 1];

    uint16_t aKit = o->addr[(BMS_V_KIT - 70) / 2];
    if (o->sumOk[(BMS_V_KIT - 70) / 2]) {
        int n = (int)d33[aKit] - 2;
        if (n > (int)sizeof(o->kit) - 1) n = (int)sizeof(o->kit) - 1;
        for (int i = 0; i < n; i++) {
            uint8_t c = d33[aKit + 1 + i];
            o->kit[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        }
    }

    // Гістограму читаємо лише з блока з ВІРНОЮ сумою: на побитому хвості вона
    // дає безглузді десятки тисяч циклів, і краще чесне «невідомо».
    o->cycles = o->sumOk[(BMS_V_ADDED - 70) / 2]
                ? impresBmsCyclesFromHist(d33, o->addr[(BMS_V_ADDED - 70) / 2]) : -1;
    uint16_t aNs = o->addr[(BMS_V_NONSMART - 70) / 2];
    if (o->sumOk[(BMS_V_NONSMART - 70) / 2]) o->nonImpresCycles = bmsBE(d33, aNs + 7);

    if (rom33) {
        o->key1 = rom33[1];
        o->key2 = rom33[6];
        o->haveKey = true;
    }
    return true;
}

// Заповнити зашифровані поля наявним ключем. Викликається після impresBmsParse.
inline void impresBmsDecrypt(const uint8_t *d33, ImpresBms *o) {
    if (!o->ok || !o->haveKey) return;
    uint16_t aCyc = o->addr[(BMS_V_CYCLE  - 70) / 2];
    uint16_t aRec = o->addr[(BMS_V_RECOND - 70) / 2];
    uint16_t aDat = o->addr[(BMS_V_DATE   - 70) / 2];

    if (aCyc != BMS_INVALID) {
        o->cyclesEnc    = impresBmsDecInt(bmsBE(d33, aCyc + 1), 2, o->key1);
        o->reverts      = impresBmsDecInt(bmsBE(d33, aCyc + 3), 2, o->key1);
        o->dayLastCharge= impresBmsDecInt(bmsBE(d33, aCyc + 5), 2, o->key1);
        o->topOffCycles = impresBmsDecInt(bmsBE(d33, aCyc + 7), 2, o->key1);
    }
    if (aRec != BMS_INVALID) {
        o->calCycles     = impresBmsDecInt(bmsBE(d33, aRec + 1), 2, o->key1);
        o->dayLastRecond = impresBmsDecInt(bmsBE(d33, aRec + 3), 2, o->key1);
        o->cts           = impresBmsDecInt(d33[aRec + 6], 1, o->key1);
        int first        = impresBmsDecInt(d33[aRec + 5], 1, o->key1);
        if (o->rsense > 0.0f) {
            o->potentialMah = (int)(BMS_ICA_MVH * o->cts / o->rsense + 0.5f);
            o->firstUseMah  = (int)(BMS_ICA_MVH * first / o->rsense + 0.5f);
        }
        if (o->ratedMah > 0 && o->potentialMah > 0) {
            int h = (int)((100L * o->potentialMah + o->ratedMah / 2) / o->ratedMah);
            o->health = h > 100 ? 100 : h;
        }
    }
    if (aDat != BMS_INVALID) {
        impresBmsDecDate(bmsBE(d33, aDat + 1), o->key1, o->key2,
                         &o->mfgY, &o->mfgM, &o->mfgD);
        o->dayInitialUse = impresBmsDecInt(bmsBE(d33, aDat + 3), 2, o->key1);
        if (o->dayInitialUse > 0 && impresBmsDateSane(o->mfgY, o->mfgM, o->mfgD)) {
            long z = impresBmsToDays(o->mfgY, o->mfgM, o->mfgD) + o->dayInitialUse;
            impresBmsFromDays(z, &o->useY, &o->useM, &o->useD);
        }
    }
}

// --------------------------------------------------------- підбір ключа
//  Для дампа, прочитаного з файлу, ROM-ID невідомий. Але в дешифруванні бере
//  участь лише НИЖНІЙ НІББЛ key1 і ВЕРХНІЙ НІББЛ key2 — усього 16×16 варіантів,
//  і майже завжди правдоподібний рівно один. Перевірки (усі — на даних, які
//  ключа не потребують, тому підробити результат неможливо):
//     • зашифрований лічильник циклів <= циклів із гістограми;
//     • реверти й калібрування <= циклів;
//     • дні (перше вмикання, останній заряд, останнє калібрування) не більші
//       за напрацювання ETM (ETM у DS2438 не шифрований);
//     • еквівалентні цикли з CCA <= лічильника циклів;
//     • потенційна ємність у межах 20..125 % паспортної.
//  Повертає кількість варіантів, що пройшли (0 — не вдалось, 1 — однозначно).
// Чи правдоподібний вміст, розшифрований ключем k1: числа мають узгоджуватись
// між собою і з тим, що ключа НЕ потребує (цикли з гістограми, CCA, шунт).
// Одна функція на два входи — підбір ключа й пряму перевірку ключа з ROM;
// двома копіями цих умов вони одного дня розійшлися б.
//
// ⚑ dayInitialUse > dayLastCharge раніше означало «не може бути»: пакет, який
// уже вмикали, мусив хоч раз заряджатися. Але після заміни елементів дату
// першого запуску вписують РУКАМИ, а день останнього заряду лишається
// нульовим — і цілком правильний вміст відкидався як сміття. Тому нуль тут
// окремо: «ще не заряджали» — законний стан, а не суперечність.
inline bool impresBmsKeyPlausible(const uint8_t *d33, const ImpresBms *o,
                                  const ImpresBms *t, long dayLim) {
    (void)d33;
    if (t->cyclesEnc > o->cycles || t->reverts > t->cyclesEnc ||
        t->calCycles > t->cyclesEnc) return false;
    if (t->dayLastCharge > dayLim || t->dayLastRecond > dayLim) return false;
    if (t->dayLastCharge > 0 && t->dayInitialUse > t->dayLastCharge) return false;
    if (o->ccaCycles > 0 && o->ccaCycles > t->cyclesEnc + t->cyclesEnc / 5) return false;
    // CTS == 0 — законний стан «пакет ще не калібрувався», не відкидаємо.
    if (o->ratedMah > 0 && t->cts > 0 &&
        (t->potentialMah * 5 < o->ratedMah || t->potentialMah * 4 > o->ratedMah * 5))
        return false;
    return true;
}

// Ліміт «скільки діб пакет узагалі міг прожити» — від наробітку монітора.
inline long impresBmsDayLimit(const ImpresBms *o) {
    long etmDays = (long)(o->etmSec / 86400UL);
    return (etmDays > 0 ? etmDays : 1) * 11 / 10 + 60;
}

inline int impresBmsFindKey(const uint8_t *d33, const uint8_t *d38, ImpresBms *o) {
    (void)d38;   // дані DS2438 вже враховано в o (ETM, CCA, шунт)
    if (!o->ok || o->cycles < 0) return 0;
    int found = 0;
    uint8_t bestK1 = 0, bestK2 = 0;
    long dayLim = impresBmsDayLimit(o);

    for (int k1 = 0; k1 < 16; k1++) {
        ImpresBms t = *o;
        t.key1 = (uint8_t)k1; t.key2 = 0x50; t.haveKey = true;
        impresBmsDecrypt(d33, &t);
        if (!impresBmsKeyPlausible(d33, o, &t, dayLim)) continue;
        found++;
        bestK1 = (uint8_t)k1;
        // Верхній нібл key2 впливає ТІЛЬКИ на дату. Беремо той, що дає осмислену
        // дату виготовлення; у переважної більшості пакетів серійник починається
        // з 0x50, тож 5 пробуємо першим.
        bestK2 = 0x50;
        for (int i = 0; i < 16; i++) {
            int n2 = (i == 0) ? 5 : (i <= 5 ? i - 1 : i);   // 5, потім 0..4, 6..15
            int y, m, d;
            impresBmsDecDate(bmsBE(d33, o->addr[0] + 1), (uint8_t)k1,
                             (uint8_t)(n2 << 4), &y, &m, &d);
            if (impresBmsDateSane(y, m, d)) { bestK2 = (uint8_t)(n2 << 4); break; }
        }
    }
    if (found == 1) {
        o->key1 = bestK1; o->key2 = bestK2;
        o->haveKey = true; o->keyGuessed = true;
        impresBmsDecrypt(d33, o);
    }
    return found;
}

// ------------------------------------------------- єдина точка для всіх поверхонь
//  Розбір дешевий (16 векторів + 16 коротких сум + щонайбільше 16 спроб ключа),
//  тож кеш із інвалідацією не потрібен — перечитуємо на кожен виклик. Так
//  жодна поверхня не може показати застарілі дані після запису в чип.
//    d33/d38 — дампи (d38 може бути nullptr),
//    rom33   — ROM-ID DS2433 або nullptr (тоді ключ підбирається),
//    rsFallback — шунт із налаштувань, коли в чипі поля немає.
inline const ImpresBms &impresBmsOf(const uint8_t *d33, const uint8_t *d38,
                                    const uint8_t *rom33, float rsFallback) {
    static ImpresBms s;
    if (!impresBmsParse(d33, d38, rom33, rsFallback, &s)) return s;
    if (s.haveKey) impresBmsDecrypt(d33, &s);
    else           impresBmsFindKey(d33, d38, &s);
    return s;
}

// ── ІСТОРІЯ ЛІЧИЛЬНИКІВ У САМОМУ ПАКЕТІ ───────────────────────────────────
//  Блок NONSMART (той самий, з якого читаються цикли не-IMPRES) містить CCA і
//  DCA В ТИХ САМИХ сирих одиницях, що й монітор: зсуви +3 і +5, порядок
//  старшим байтом уперед. Звірено на корпусі — у 47 із 56 пакетів із цілим
//  блоком монітор іде поруч з історією, трохи попереду: він рахує безперервно,
//  а станція дописує історію лише на циклі калібрування.
//
//  Повертає false, якщо блока немає або він завеликий/закороткий для полів.
inline bool impresBmsHistCounters(const uint8_t *d33, uint16_t *cca, uint16_t *dca) {
    if (!d33) return false;
    uint16_t a = impresBmsVector(d33, BMS_V_NONSMART);
    if (a == BMS_INVALID) return false;
    int len = d33[a];
    if (len < 8 || (int)a + len > IMPRES_33_SIZE) return false;
    if (cca) *cca = bmsBE(d33, a + 3);
    if (dca) *dca = bmsBE(d33, a + 5);
    return true;
}

// ── ПОЛАГОДИТИ ПОБИТИЙ ЛІЧИЛЬНИК РОЗРЯДУ МОНІТОРА ─────────────────────────
//  Пише в d38 значення DCA з історії пакета. Повертає false — і НІЧОГО не
//  пише, — коли підстав для правки немає або джерелу не можна довіряти.
//
//  ⚑ ЧОМУ НЕ ВИМАГАЄМО ЦІЛОЇ СУМИ БЛОКА. Саме в тих пакетах, де ця біда й
//  трапилась, сума блока не сходиться (байт довжини 0x0F замість 0x0D) — а
//  ДАНІ в ньому демонстровано цілі: у сусідньому пакеті з тією самою
//  аномалією історія 3014/2967 проти монітора 3025/2978, тобто рівно +11 по
//  ОБОХ лічильниках. Формальна сума тут відхилила б справні числа. Тому
//  беремо змістовні перевірки — і вони сильніші за суму:
//
//   • заряд в історії не нульовий: нульовий означає, що станція до цього
//     пакета ще не доходила, і брати з такої історії нема чого;
//   • історія сама з собою узгоджена: розряду не більше, ніж заряду;
//   • ЗАРЯД у моніторі цілий — це наш якір: ламається зазвичай один регістр,
//     і без цілого CCA порівнювати нема з чим;
//   • історія НЕ ПОПЕРЕДУ монітора. Якщо попереду — монітор обнулили або
//     замінили навмисно (так виглядають dumps/08-nova-batareya), і заливати
//     в нього стару історію означало б скасувати чуже свідоме рішення;
//   • і, нарешті, монітор справді зламаний. Інакше функція стала б способом
//     тихо переписати справний лічильник.
inline bool impresBmsFixDcaFromHist(const uint8_t *d33, uint8_t *d38, uint16_t *wrote) {
    if (!d33 || !d38) return false;
    uint16_t hC = 0, hD = 0;
    if (!impresBmsHistCounters(d33, &hC, &hD)) return false;
    //  ⚑ Стерту історію (0xFFFF) окремим рядком НЕ перевіряємо — така умова
    //  ніколи б не спрацювала, і в коді лишилась би вдавана охорона. Стерте
    //  значення відсіюють ті самі перевірки, що й решту сміття, просто нижче:
    //  стертий ЗАРЯД (65535) більший за будь-який справний монітор, отже впаде
    //  на «історія не попереду»; а стертий РОЗРЯД пройшов би правдоподібність
    //  лише при заряді від 43557 — і тоді монітор із таким самим заряджанням
    //  уже неможливо визнати зламаним (65535 < 43557·1.5), тобто до запису
    //  справа не дійде. Лишається те, що справді вирішує долю правки:
    if (hC == 0) return false;
    if (!impresBmsDcaSane(hC, hD)) return false;

    uint16_t mC = bmsLE(d38, 60), mD = bmsLE(d38, 62);
    if (mC == 0xFFFF) return false;          // якір ненадійний
    if (hC > mC)      return false;          // монітор обнулили — не чіпаємо
    if (impresBmsDcaSane(mC, mD)) return false;   // ламати нема чого

    d38[62] = (uint8_t)(hD & 0xFF);
    d38[63] = (uint8_t)(hD >> 8);
    if (wrote) *wrote = hD;
    return true;
}

#endif // IMPRES_BMS_H
