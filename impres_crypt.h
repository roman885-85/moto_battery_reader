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
};

inline uint16_t impresCryptAddr(const uint8_t *d33, int vec) {
    return impresBmsVector(d33, vec);
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
        f->dayInitialUse = impresBmsDecInt(bmsBE(d33, aDat + 3), 2, k1);
    }
}

// Записати ті самі поля ключем k1/k2 і полагодити суми блоків. Пишемо лише те,
// що справді прочиталось: інакше «перешифрування» тихо заповнило б нулями
// блок, якого ми не бачили.
inline void impresCryptWrite(uint8_t *d33, uint8_t k1, uint8_t k2,
                             const ImpresCryptFields *f) {
    uint16_t aCyc = impresCryptAddr(d33, BMS_V_CYCLE);
    uint16_t aRec = impresCryptAddr(d33, BMS_V_RECOND);
    uint16_t aDat = impresCryptAddr(d33, BMS_V_DATE);

    if (f->haveCyc && aCyc != BMS_INVALID) {
        impresCryptPutBE(d33, aCyc + 1, impresCryptEncInt(f->cyclesEnc,     2, k1));
        impresCryptPutBE(d33, aCyc + 3, impresCryptEncInt(f->reverts,       2, k1));
        impresCryptPutBE(d33, aCyc + 5, impresCryptEncInt(f->dayLastCharge, 2, k1));
        impresCryptPutBE(d33, aCyc + 7, impresCryptEncInt(f->topOffCycles,  2, k1));
        impresFixRecord(d33, aCyc, d33[aCyc]);
    }
    if (f->haveRec && aRec != BMS_INVALID) {
        impresCryptPutBE(d33, aRec + 1, impresCryptEncInt(f->calCycles,     2, k1));
        impresCryptPutBE(d33, aRec + 3, impresCryptEncInt(f->dayLastRecond, 2, k1));
        d33[aRec + 5] = (uint8_t)impresCryptEncInt(f->firstUse, 1, k1);
        d33[aRec + 6] = (uint8_t)impresCryptEncInt(f->cts,      1, k1);
        impresFixRecord(d33, aRec, d33[aRec]);
    }
    if (f->haveDat && aDat != BMS_INVALID) {
        impresCryptPutBE(d33, aDat + 1, impresCryptEncDate(f->mfgY, f->mfgM, f->mfgD, k1, k2));
        impresCryptPutBE(d33, aDat + 3, impresCryptEncInt(f->dayInitialUse, 2, k1));
        impresFixRecord(d33, aDat, d33[aDat]);
    }
}

// ------------------------------------------------------------- ключ вмісту
// Яким ключем зашифровано те, що зараз лежить у d33. Спочатку — підбір за
// вмістом (impresBmsFindKey перевіряє лише НЕзашифрованими даними, тож
// підробити результат неможливо). Повертає false, якщо ключ не визначається:
// тоді перешифровувати нема чого — ми не знаємо, що саме там записано.
inline bool impresCryptSourceKey(const uint8_t *d33, const uint8_t *d38,
                                 uint8_t *k1, uint8_t *k2) {
    ImpresBms o;
    if (!impresBmsParse(d33, d38, nullptr, 0.0f, &o)) return false;
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

// Головна операція: перешифрувати вміст d33 із ключа джерела в ключ чипа.
// Числа лишаються ті самі — міняється лише те, хто зможе їх прочитати.
// mfg* — якщо задано (рік > 0), дату виготовлення беремо звідти, а не з даних:
// на «свіжому» хвості її просто немає, і вписати її може лише людина.
inline bool impresCryptRekey(uint8_t *d33, const uint8_t *d38,
                             uint8_t ownK1, uint8_t ownK2,
                             int mfgY, int mfgM, int mfgD) {
    uint8_t sk1, sk2;
    if (!impresCryptSourceKey(d33, d38, &sk1, &sk2)) return false;
    ImpresCryptFields f;
    impresCryptRead(d33, sk1, sk2, &f);
    if (mfgY > 0) {
        f.haveDat = f.haveDat || (impresCryptAddr(d33, BMS_V_DATE) != BMS_INVALID);
        f.mfgY = mfgY; f.mfgM = mfgM; f.mfgD = mfgD;
    }
    impresCryptWrite(d33, ownK1, ownK2, &f);
    return true;
}

#endif // IMPRES_CRYPT_H
