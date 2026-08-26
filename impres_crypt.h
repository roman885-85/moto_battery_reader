#ifndef IMPRES_CRYPT_H
#define IMPRES_CRYPT_H
// ===========================================================================
//  ПЕРЕШИФРУВАННЯ ПІД ROM ЦЬОГО ЧИПА
//
//  Частина полів у DS2433 зашифрована, і ключ береться з ROM-ID чипа DS2433:
//      key1 = ROM[1]   (у дешифруванні бере участь лише НИЖНІЙ нібл)
//      key2 = ROM[6]   (лише ВЕРХНІЙ нібл, і тільки для дат)
//  Схема — обертання вправо на (key1 & 15) біт, далі −0xD8; для дат обертання
//  на ((key2>>4) ^ (key1&0xF)) біт, далі +10048 і розбір як дати FAT
//  (impres_bms.h, розділ 4a.4 документа).
//
//  ⚑ НАСЛІДОК, через який цей файл і з'явився. Еталон у templates.h знято з
//  ЧУЖОГО акумулятора, і його зашифровані поля зашифровані ROM'ом ДОНОРА.
//  Записавши еталон у інший чип, ми віддаємо рації байти, які вона розшифрує
//  СВОЇМ (правильним для неї) ключем — і отримає сміття. На реальному пакеті
//  (dumps/16-verbatim-4409a-chuzhyi-kliuch) це виглядало так:
//
//      дата виготовлення  2107-13-21   (тринадцятий місяць)
//      цикли              3548         (при 1097 з нешифрованої гістограми)
//      знос               76 %         (насправді 34 %)
//
//  Шифрування повністю оборотне, тож лікується це рівно одним: розшифрувати
//  поля ключем, яким вони зашифровані ЗАРАЗ, і зашифрувати назад ключем
//  ЦЬОГО чипа. Числа не міняються — міняється лише те, хто здатен їх прочитати.
//
//  Тут — чиста арифметика без вводу-виводу.
// ===========================================================================
#include <string.h>
#include "impres_format.h"
#include "impres_bms.h"

// ------------------------------------------------------------- шифрування
// Точна інверсія impresBmsDecInt(): (+0xD8, обертання ВЛІВО).
inline uint16_t impresCryptEncInt(uint16_t v, int len, uint8_t key) {
    bool is8 = (len == 1);
    v = (uint16_t)(v + 0xD8);
    if (is8) v &= 0xFF;
    for (int n = key & 15; n > 0; n--) {
        if (is8) { uint8_t b = (uint8_t)v; v = (uint8_t)((b << 1) | (b >> 7)); }
        else     v = (uint16_t)((v << 1) | (v >> 15));
    }
    return is8 ? (uint16_t)(v & 0xFF) : v;
}

// Інверсія impresBmsDecDate(). Обертання ВЛІВО, щонайменше одне (do/while —
// так само, як у дешифруванні: при нульовому лічильнику там теж один прохід).
inline uint16_t impresCryptEncDate(int y, int m, int d, uint8_t key1, uint8_t key2) {
    if (y < 1980) y = 1980;
    uint32_t fat = ((uint32_t)(y - 1980) << 9) | ((uint32_t)m << 5) | (uint32_t)d;
    uint16_t v = (uint16_t)(fat - 10048);
    int k = (key2 >> 4) ^ (key1 & 0x0F);
    do { k--; v = (uint16_t)((v << 1) | (v >> 15)); } while (k > 0);
    return v;
}

inline void impresCryptPutBE(uint8_t *p, int off, uint16_t v) {
    p[off] = (uint8_t)(v >> 8); p[off + 1] = (uint8_t)(v & 0xFF);
}

// ------------------------------------------------------ що саме зашифровано
// Рівно ті поля, які читає impresBmsDecrypt(). Тримаємо їх окремою структурою,
// щоб «розшифрувати старим ключем» і «зашифрувати новим» були симетричні й
// щоб між ними нічого не губилось.
struct ImpresCryptFields {
    bool     haveCyc, haveRec, haveDat;
    uint16_t cyclesEnc, reverts, dayLastCharge, topOffCycles;   // блок CYCLE
    uint16_t calCycles, dayLastRecond;                          // блок RECOND
    uint8_t  firstUse, cts;                                     // …він же
    int      mfgY, mfgM, mfgD;                                  // блок DATE
    uint16_t dayInitialUse;
    // ⚑ Поле-близнюк за зсувом +5..+6. Його призначення ми не розібрали, але
    // виміряно: у 40 дампах із 43 воно ПОБАЙТОВО дорівнює dayInitialUse, а в
    // трьох відрізняється на одиниці. Тож читаємо й пишемо його разом із
    // основним — інакше після правки вони розійшлися б, чого в живих пакетах
    // практично не буває.
    uint16_t dayInitialUse2;
};

inline uint16_t impresCryptAddr(const uint8_t *d33, int vec) {
    return impresBmsVector(d33, vec);
}

// Чи можна писати в цей блок: він має існувати й бути достатньо довгим для
// полів, які ми туди кладемо. На СТЕРТОМУ хвості байт довжини = 0xFF, і без
// цієї перевірки перешифрування писало б у порожнечу, псуючи сусідні записи.
inline bool impresCryptBlockUsable(const uint8_t *d33, uint16_t a, int need) {
    if (a == BMS_INVALID) return false;
    uint8_t len = d33[a];
    if (len < need || len > 32) return false;
    return (int)a + len <= IMPRES_33_SIZE;
}

// Прочитати зашифровані поля ключем k1/k2. Блок, сума якого не сходиться,
// пропускаємо: на побитому хвості там не дані, а сміття, і переносити його
// в новий чип означало б закріпити помилку.
inline void impresCryptRead(const uint8_t *d33, uint8_t k1, uint8_t k2,
                            ImpresCryptFields *f) {
    memset(f, 0, sizeof(*f));
    uint16_t aCyc = impresCryptAddr(d33, BMS_V_CYCLE);
    uint16_t aRec = impresCryptAddr(d33, BMS_V_RECOND);
    uint16_t aDat = impresCryptAddr(d33, BMS_V_DATE);

    if (aCyc != BMS_INVALID && impresBmsBlockOk(d33, aCyc)) {
        f->haveCyc       = true;
        f->cyclesEnc     = impresBmsDecInt(bmsBE(d33, aCyc + 1), 2, k1);
        f->reverts       = impresBmsDecInt(bmsBE(d33, aCyc + 3), 2, k1);
        f->dayLastCharge = impresBmsDecInt(bmsBE(d33, aCyc + 5), 2, k1);
        f->topOffCycles  = impresBmsDecInt(bmsBE(d33, aCyc + 7), 2, k1);
    }
    if (aRec != BMS_INVALID && impresBmsBlockOk(d33, aRec)) {
        f->haveRec        = true;
        f->calCycles      = impresBmsDecInt(bmsBE(d33, aRec + 1), 2, k1);
        f->dayLastRecond  = impresBmsDecInt(bmsBE(d33, aRec + 3), 2, k1);
        f->firstUse       = (uint8_t)impresBmsDecInt(d33[aRec + 5], 1, k1);
        f->cts            = (uint8_t)impresBmsDecInt(d33[aRec + 6], 1, k1);
    }
    if (aDat != BMS_INVALID && impresBmsBlockOk(d33, aDat)) {
        f->haveDat = true;
        impresBmsDecDate(bmsBE(d33, aDat + 1), k1, k2, &f->mfgY, &f->mfgM, &f->mfgD);
        f->dayInitialUse  = impresBmsDecInt(bmsBE(d33, aDat + 3), 2, k1);
        f->dayInitialUse2 = impresBmsDecInt(bmsBE(d33, aDat + 5), 2, k1);
    }
}

// Записати ті самі поля ключем k1/k2 і полагодити суми блоків. Пишемо лише те,
// що справді прочиталось: інакше «перешифрування» тихо заповнило б нулями
// блок, якого ми не бачили.
// Привести поля до внутрішньо несуперечливого стану ПЕРЕД записом. Пакет, який
// уже вмикали, не міг жодного разу заряджатись: день останнього заряду не може
// бути меншим за день першого запуску. Раніше ми лишали його нульовим — і
// власний же підбір ключа згодом вважав такий вміст сміттям.
inline void impresCryptNormalize(ImpresCryptFields *f) {
    if (!f) return;
    if (f->dayInitialUse > 0 && f->dayLastCharge < f->dayInitialUse)
        f->dayLastCharge = f->dayInitialUse;
    if (f->dayInitialUse2 && f->dayInitialUse2 != f->dayInitialUse)
        f->dayInitialUse2 = f->dayInitialUse;   // поле-близнюк іде разом
}

// ── СТЕЛЯ РОДИНИ ЛІЧИЛЬНИКІВ — ЦИКЛИ IMPRES ───────────────────────────────
//  «Скільки цей пакет відпрацював» — ОДНЕ питання, і в чипі на нього відповідає
//  не одне число, а ціла родина: цикли з гістограми (їх показує фірмове ПЗ),
//  внутрішній лічильник у блоці CYCLE, реверти, дозаряди, калібрувальні цикли.
//  Виміряно на корпусі: у ВСІХ 41 дампі з ключем кожен із братів не більший за
//  цикли з гістограми. Тобто гістограма — стеля родини, а не ще один її член.
//
//  ⚑ ПРИТИСКАЄ, АЛЕ НЕ ПІДНІМАЄ — те саме правило, що й у impresSetEtm() для
//  міток подій. Брат, менший за стелю, — це законна історія пакета (реверти
//  бувають і нульовими); брат, БІЛЬШИЙ за неї, — стан, якого немає в жодному
//  живому пакеті, і саме такий станція «лікує», повертаючи свої числа.
//
//  Правило живе ТУТ, поруч із самими полями, а не в редакторі: обнуляти цикли
//  вміє не лише він, і копія цієї умови в кожному місці розійшлася б на першій
//  же правці.
inline bool impresCryptCapByCycles(ImpresCryptFields *f, long cycles) {
    if (!f || cycles < 0) return false;
    bool did = false;
    auto cap = [&](uint16_t &v) {
        if ((long)v > cycles) { v = (uint16_t)cycles; did = true; }
    };
    if (f->haveCyc) { cap(f->cyclesEnc); cap(f->reverts); cap(f->topOffCycles); }
    if (f->haveRec) { cap(f->calCycles); }
    return did;
}

inline void impresCryptWrite(uint8_t *d33, uint8_t k1, uint8_t k2,
                             const ImpresCryptFields *fIn) {
    ImpresCryptFields tmp = *fIn;
    impresCryptNormalize(&tmp);
    const ImpresCryptFields *f = &tmp;
    uint16_t aCyc = impresCryptAddr(d33, BMS_V_CYCLE);
    uint16_t aRec = impresCryptAddr(d33, BMS_V_RECOND);
    uint16_t aDat = impresCryptAddr(d33, BMS_V_DATE);

    if (f->haveCyc && impresCryptBlockUsable(d33, aCyc, 10)) {
        impresCryptPutBE(d33, aCyc + 1, impresCryptEncInt(f->cyclesEnc,     2, k1));
        impresCryptPutBE(d33, aCyc + 3, impresCryptEncInt(f->reverts,       2, k1));
        impresCryptPutBE(d33, aCyc + 5, impresCryptEncInt(f->dayLastCharge, 2, k1));
        impresCryptPutBE(d33, aCyc + 7, impresCryptEncInt(f->topOffCycles,  2, k1));
        impresFixRecord(d33, aCyc, d33[aCyc]);
    }
    if (f->haveRec && impresCryptBlockUsable(d33, aRec, 8)) {
        impresCryptPutBE(d33, aRec + 1, impresCryptEncInt(f->calCycles,     2, k1));
        impresCryptPutBE(d33, aRec + 3, impresCryptEncInt(f->dayLastRecond, 2, k1));
        d33[aRec + 5] = (uint8_t)impresCryptEncInt(f->firstUse, 1, k1);
        d33[aRec + 6] = (uint8_t)impresCryptEncInt(f->cts,      1, k1);
        impresFixRecord(d33, aRec, d33[aRec]);
    }
    if (f->haveDat && impresCryptBlockUsable(d33, aDat, 6)) {
        impresCryptPutBE(d33, aDat + 1, impresCryptEncDate(f->mfgY, f->mfgM, f->mfgD, k1, k2));
        impresCryptPutBE(d33, aDat + 3, impresCryptEncInt(f->dayInitialUse, 2, k1));
        if (impresCryptBlockUsable(d33, aDat, 8))
            impresCryptPutBE(d33, aDat + 5, impresCryptEncInt(f->dayInitialUse2, 2, k1));
        impresFixRecord(d33, aDat, d33[aDat]);
    }
}

// ═══════════════════ ІДЕНТИЧНІСТЬ ІЗ ROM САМОГО ЧИПА ═══════════════════════
//  Вшитий еталон — побайтова копія ОДНОГО реального пакета, а його зашифровані
//  блоки (дати, знос, калібрування) зашифровані ROM-ом ДОНОРА. Записати їх на
//  порожній чип означає віддати новому пакету числа, які рація розшифрує СВОЇМ
//  ключем — і побачить сміття. Саме так і виглядає «невідомий акумулятор»
//  (dumps/16-verbatim-4409a-chuzhyi-kliuch).
//
//  Тому для порожнього чипа ідентичність не копіюють, а ГЕНЕРУЮТЬ: беруть
//  ROM-ID цього чипа (він і є серійним номером — окремого поля-серійника в
//  пам'яті DS2433 немає), із нього — ключі (key1 = ROM[1], key2 = ROM[6]), і
//  під цим ключем пишуть свіжі значення: дата виготовлення = сьогодні, пакет
//  ще не вмикали, лічильники нульові, потенційна ємність = паспортна.
//
//  Рік 0 у даті означає «дати немає» — тоді блок DATE не чіпаємо взагалі:
//  краще лишити його, ніж записати завідомо неправдиву дату (годинник
//  пристрою може бути не заведений).
inline void impresIdentityFresh(ImpresCryptFields *f, int y, int m, int d, uint8_t cts) {
    memset(f, 0, sizeof(*f));
    f->haveCyc = true;                  // цикли/повернення/дозаряди — з нуля
    f->haveRec = true;
    f->cts      = cts;                  // потенційна ємність
    f->firstUse = cts;                  // на початку вона ж і була
    if (y > 0) {
        f->haveDat = true;
        f->mfgY = y; f->mfgM = m; f->mfgD = d;
        f->dayInitialUse = f->dayInitialUse2 = 0;   // ще не вмикали
    }
}

// ═══════════ ОБНУЛИТИ ІСТОРІЮ ВИКОРИСТАННЯ, ЗБЕРІГШИ ПОХОДЖЕННЯ ═══════════
//  Різниця з impresIdentityFresh() принципова, і саме через неї ця функція
//  існує окремо. «Свіжа ідентичність» переписує ВСЕ, включно з датою
//  виготовлення (їй нема звідки взяти справжню — чип порожній). Але коли
//  пакет РОБОЧИЙ і ми лише скидаємо лічильники перед калібруванням, його дата
//  виготовлення — справжня й одна-єдина. Затерти її сьогоднішнім числом
//  означає знищити останній достовірний факт про походження пакета, ще й
//  зробивши його «виготовленим» пізніше за власні записи в журналі.
//
//  Тому тут: історія — в нуль, дата — недоторкана.
inline void impresUsageReset(ImpresCryptFields *f) {
    if (!f) return;
    f->cyclesEnc = f->reverts = f->dayLastCharge = f->topOffCycles = 0;
    f->calCycles = f->dayLastRecond = 0;
    f->firstUse  = f->cts;                  // «на початку було стільки ж»
    f->dayInitialUse = f->dayInitialUse2 = 0;   // пакет іще не вмикали
}

// Записати згенеровану ідентичність під ROM ЦЬОГО чипа. rom33 — 8 байт
// лазерного ROM-ID DS2433; nullptr означає «ключ невідомий», і тоді нічого не
// пишемо: наосліп вийде та сама чужа шифровка, лише з іншим підписом.
// Повертає true, якщо ідентичність згенеровано.
inline bool impresIdentityWrite(uint8_t *d33, const uint8_t *rom33,
                                int y, int m, int d, uint8_t cts) {
    if (!d33 || !rom33) return false;
    ImpresCryptFields f;
    impresIdentityFresh(&f, y, m, d, cts);
    impresCryptWrite(d33, rom33[1], rom33[6], &f);
    return true;
}

// ═══════════════════ ЛІЧИЛЬНИКИ ЦИКЛІВ (БЕЗ ШИФРУВАННЯ) ═══════════════════
//  Ці два числа ключа не потребують — саме тому фірмове ПЗ показує їх завжди.
//
//  «Цикли заряду IMPRES» лежать у гістограмі доданого заряду (блок ADDED):
//  нульовий кошик несе САМУ суму циклів, решта дев'ять — розподіл. Читання
//  (impresBmsCyclesFromHist): якщо h0 >= суми решти, то це й є відповідь.
//  Отже, щоб записати N, досить покласти h0 = N — але лише поки решта не
//  більша за N. Якщо більша, розподіл масштабуємо, зберігаючи форму: інакше
//  формула поверне суму всіх кошиків замість N.
//
//  «Цикли не-IMPRES» — просте 16-бітне число в блоці NONSMART за зсувом +7.
inline bool impresCyclesWrite(uint8_t *d33, int cycles) {
    uint16_t a = impresBmsVector(d33, BMS_V_ADDED);
    // Потрібно 22 байти, а не 21: [довжина] + 10 кошиків по 2 Б (a+1..a+20) +
    // [сума] на a+21. Із порогом 21 блок, що ЗАЯВЛЯЄ довжину 21, проходив
    // перевірку — і impresFixRecord() клав суму на a+20, тобто просто в
    // молодший байт останнього кошика, тихо псуючи гістограму, з якої потім
    // читаються цикли. У всіх 44 дампах корпусу, де блок є, довжина рівно 22.
    if (cycles < 0 || !impresCryptBlockUsable(d33, a, 22)) return false;
    long rest = 0;
    for (int i = 1; i < 10; i++) rest += bmsBE(d33, a + 1 + i * 2);
    if (rest > cycles) {                       // стиснути розподіл під нову суму
        for (int i = 1; i < 10; i++) {
            long v = bmsBE(d33, a + 1 + i * 2);
            impresCryptPutBE(d33, a + 1 + i * 2, (uint16_t)(rest ? v * cycles / rest : 0));
        }
    }
    impresCryptPutBE(d33, a + 1, (uint16_t)cycles);
    impresFixRecord(d33, a, d33[a]);
    return true;
}

inline bool impresNonImpresWrite(uint8_t *d33, int cycles) {
    uint16_t a = impresBmsVector(d33, BMS_V_NONSMART);
    if (cycles < 0 || !impresCryptBlockUsable(d33, a, 10)) return false;
    impresCryptPutBE(d33, a + 7, (uint16_t)cycles);
    impresFixRecord(d33, a, d33[a]);
    return true;
}

// ═══════ НАРОБІТОК, ЗАПИСАНИЙ У САМОМУ ПАКЕТІ (CCA/DCA у блоці NONSMART) ═══
//  У ТОМУ САМОМУ блоці, поруч із лічильником не-IMPRES, лежить ще одна пара
//  чисел: CCA і DCA за зсувами +3 і +5, у ТИХ САМИХ сирих одиницях, що й у
//  моніторі (читає їх impresBmsHistCounters, звірено на 56 пакетах корпусу).
//
//  ⚑ ЦЕ Й Є ТЕ, ЗВІДКИ СТАНЦІЯ ПОВЕРТАЄ ЛІЧИЛЬНИКИ ПІСЛЯ СКИДАННЯ. Доказ —
//  дампи власника «до / після скидання / після станції» на PMNN4409A:
//  скидання обнулило монітор (CCA 4798 -> 0), пакет побував у зарядці, і в
//  моніторі опинилось **4797** — не 4798, тобто НЕ старе значення монітора, а
//  байт-у-байт те, що весь цей час лежало тут, у DS2433 (0x156: 12 BD, 0x158:
//  12 33). Паливомір (ICA), який ми теж переписали, станція не чіпала — отже
//  вона не відновлює знімок, а бере саме ці поля.
//
//  Ми знали про них (на них тримається правка побитого лічильника розряду),
//  але жоден шлях скидання їх не чіпав: impresNonImpresWrite() пише за зсувом
//  +7 і про сусідів не знає. Тобто «скидання лічильників» лишало в пакеті
//  повну історію й чесно дивувалось, чому вона повертається.
inline bool impresHistCountersWrite(uint8_t *d33, uint16_t cca, uint16_t dca) {
    uint16_t a = impresBmsVector(d33, BMS_V_NONSMART);
    // 8 — стільки ж, скільки вимагає читач: [довжина] + два поля до a+6 і сума.
    // Брати більше означало б мовчки пропускати блоки, які читач розбирає.
    if (!impresCryptBlockUsable(d33, a, 8)) return false;
    impresCryptPutBE(d33, a + 3, cca);
    impresCryptPutBE(d33, a + 5, dca);
    impresFixRecord(d33, a, d33[a]);
    return true;
}

// ═══════════ УЗГОДЖЕНЕ СКИДАННЯ ІСТОРІЇ В DS2433 ═══════════════════════════
//  ⚑ НАВІЩО ЦЕ ПОТРІБНО — НАЙВАЖЛИВІШЕ В ЦЬОМУ ФАЙЛІ.
//
//  Скидання лічильників завжди робилось лише в DS2438: ETM, CCA, DCA в нуль.
//  DS2433 при цьому лишався як був — із датою першого користування, з
//  гістограмою на сотні циклів, із лічильником калібрувань. У результаті
//  пакет починав розповідати про себе дві несумісні речі одночасно:
//
//      DS2433: «мене вмикали 2023-го, я пройшов 1097 циклів»
//      DS2438: «я не пропрацював жодної секунди й не прийняв жодного мА·год»
//
//  Такого стану не буває в природі. І він не гіпотетичний: у корпусі dumps/ з
//  52 цілих фірмових пакетів ETM дорівнює нулю рівно у ДВОХ — обидва з
//  08-nova-batareya, тобто саме з того випадку, коли рація сказала «невідомий
//  акумулятор». В інших 50 напрацювання — сотні мільйонів секунд.
//
//  Раніше провал 08-nova-batareya пояснювали ЧУЖИМ навченим хвостом, і це теж
//  правда. Але дефекту там два, і другий — оцей: обнулений монітор проти
//  DS2433, повного історії. Хвіст ми полагодили, а це лишалось.
//
//  Тому скидання тепер узгоджене: обнуляємо лічильники в ОБОХ чипах або в
//  жодному. rom33 обов'язковий — шифроване інакше нічим переписати; без нього
//  функція чесно повертає false, а викликач мусить НЕ чіпати й монітор.
inline bool impresHistoryZero(uint8_t *d33, const uint8_t *rom33) {
    if (!d33 || !rom33) return false;
    uint8_t k1 = rom33[1], k2 = rom33[6];
    ImpresCryptFields f;
    impresCryptRead(d33, k1, k2, &f);
    // Читаємо СВОЇМ ключем і пишемо ним же: перешифрування тут не завдання.
    // Якщо блоків немає взагалі — писати нічого, але це не помилка: у пакета
    // просто немає зашифрованої історії (порожній або стертий хвіст).
    impresUsageReset(&f);
    impresCryptWrite(d33, k1, k2, &f);
    // Лічильники циклів ключа не потребують і живуть окремо — гістограма
    // доданого заряду й лічильник не-IMPRES. Без них «нуль циклів» був би
    // видно лише в зашифрованому блоці, а фірмове ПЗ читає саме гістограму.
    impresCyclesWrite(d33, 0);
    impresNonImpresWrite(d33, 0);
    // …і наробіток, записаний у самому пакеті. Без цього рядка скидання
    // трималось рівно до першої зарядної станції: вона бере CCA/DCA звідси.
    impresHistCountersWrite(d33, 0, 0);
    impresFixHeader(d33);

    // ⚑ ЗВІТУЄМО ЗА РЕЗУЛЬТАТОМ, А НЕ ЗА ТИМ, ЩО МИ ЩОСЬ ВИКЛИКАЛИ.
    //  impresCyclesWrite() відмовляється працювати з блоком, довжина якого не
    //  та, — і на трьох пакетах корпусу вона саме так і робить. Якби ми
    //  повернули true не дивлячись, викликач обнулив би монітор, а гістограма
    //  лишилась би на своїх сотнях циклів: рівно та суперечність, задля якої
    //  ця функція й писалась. Тому перечитуємо результат тим самим кодом, яким
    //  його читатиме аудит.
    uint16_t aAdd = impresBmsVector(d33, BMS_V_ADDED);
    if (aAdd != BMS_INVALID && impresBmsCyclesFromHist(d33, aAdd) != 0) return false;
    // Те саме питання до наробітку в пакеті — і ставимо його ТИМ САМИМ кодом,
    // яким його читатиме станція й наш аудит. Якщо читач ці поля розбирає, вони
    // мусять бути нулем; якщо не розбирає (блок закороткий) — писати не було
    // куди, і це не суперечність, а відсутність полів.
    uint16_t hC = 0, hD = 0;
    if (impresBmsHistCounters(d33, &hC, &hD) && (hC || hD)) return false;
    return true;
}

// ------------------------------------------------------------- ключ вмісту
// Яким ключем зашифровано те, що зараз лежить у d33. Спочатку — підбір за
// вмістом (impresBmsFindKey перевіряє лише НЕзашифрованими даними, тож
// підробити результат неможливо). Повертає false, якщо ключ не визначається:
// тоді перешифровувати нема чого — ми не знаємо, що саме там записано.
inline bool impresCryptSourceKey(const uint8_t *d33, const uint8_t *d38,
                                 uint8_t *k1, uint8_t *k2,
                                 const uint8_t *rom33 = nullptr) {
    ImpresBms o;
    if (!impresBmsParse(d33, d38, nullptr, 0.0f, &o)) return false;
    // ⚑ Якщо ROM цього чипа відомий — його ключ перевіряємо ПЕРШИМ і прямо, а
    // не шукаємо наосліп. Підбір вимагає, щоб кандидат лишився РІВНО один, і на
    // щойно записаному пакеті (лічильники нульові, дату вписали руками) жоден
    // кандидат не проходив — картка правок показувала прочерк, хоча вкладка
    // «Дані», яка читає прямо ROM-ом, показувала все правильно.
    // Питання, на яке тут відповідають, — не «який ключ підійде», а «чи не
    // зашифровано вміст ЧУЖИМ ключем». Із відомим ROM його вирішує сама дата:
    // правильний ключ дає правдоподібну дату виготовлення, чужий — сміття на
    // кшталт 2107-13-21 (dumps/16). Звірку лічильників тут НЕ застосовуємо: вона
    // потрібна, щоб розрізнити 16 кандидатів наосліп, а ROM неоднозначності не
    // лишає. Саме на ній і горіло: щойно записаний пакет має нульові лічильники,
    // тоді як CCA монітора вже ненульове, — і власний ключ відкидався як чужий.
    if (rom33) {
        ImpresBms t = o;
        t.key1 = rom33[1]; t.key2 = rom33[6]; t.haveKey = true;
        impresBmsDecrypt(d33, &t);
        if (impresBmsDateSane(t.mfgY, t.mfgM, t.mfgD)) {
            *k1 = rom33[1]; *k2 = rom33[6];
            return true;
        }
    }
    if (impresBmsFindKey(d33, d38, &o) != 1) return false;
    *k1 = o.key1; *k2 = o.key2;
    return true;
}

// Чи потрібне перешифрування: ключ вмісту відомий і НЕ збігається з ключем
// цього чипа. Значущі лише нижній нібл k1 і верхній нібл k2 — решта біт у
// дешифруванні не бере участі, і вимагати їхнього збігу означало б пропонувати
// правку там, де рація й так усе прочитає.
inline bool impresCryptKeyDiffers(uint8_t srcK1, uint8_t srcK2,
                                  uint8_t ownK1, uint8_t ownK2) {
    return ((srcK1 & 0x0F) != (ownK1 & 0x0F)) || ((srcK2 >> 4) != (ownK2 >> 4));
}

#endif // IMPRES_CRYPT_H
