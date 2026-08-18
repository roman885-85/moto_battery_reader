#pragma once
// ===========================================================================
//  mirror_plan.h — СИНХРОНІЗАЦІЯ ДЗЕРКАЛА DS2438 -> DS2433 З ПРАВКОЮ
// ===========================================================================
//  Прохання власника: «є дані з 2438, які дублюються в 2433. Потрібна функція
//  синхронізації цих даних з 2438, а також правка даних перед синхронізацією».
//
//  Що дублюється. Рівно 26 байтів: DS2433[0x01..0x1A] ≡ DS2438[0x18..0x31].
//  Це ідентичність пакета — те, за чим рація й станція його впізнають. Копія в
//  моніторі не випадкова: саме завдяки їй пакет зі стертим DS2433 ще можна
//  відновити, і саме це робить зарядна станція WPLN4226A, коли переписує ці
//  байти назад у чип пам'яті.
//
//  ⚑ ЧОМУ НЕ ВИСТАЧАЛО ТОГО, ЩО ВЖЕ БУЛО. Синхронізація в проєкті є
//  (impresSyncMirror / «добудова заголовка»), але вона СЛІПА: копіює всі 26
//  байтів і виправляє суму. Користувач не бачить ні що саме зміниться, ні чи
//  правильне те, що прийде з монітора. А монітор буває чужий — саме так
//  виглядає більшість несправних пакетів у dumps/. Сліпе копіювання в такому
//  разі не лікує, а закріплює чужу ідентичність.
//
//  Тому тут — ПЛАН: побайтова різниця «що зараз / що прийде», прапорець на
//  кожен байт і окремо розібране поле, яке ми вміємо назвати (паспортна
//  ємність). Спершу дивимось, за потреби правимо, і лише потім пишемо.
//
//  Тут чиста арифметика без вводу-виводу — щоб її перевіряв хостовий тест.
// ===========================================================================

#include <stdint.h>
#include <string.h>
#include "impres_format.h"

// Зсув паспортної ємності ВСЕРЕДИНІ дзеркала. DS2433[0x008] — а дзеркало
// починається з 0x001, отже сьомий байт.
#define MIRROR_RATED_IDX (IMPRES_RATED_BYTE - IMPRES_MIRROR_D33_AT)

struct MirrorPlan {
    bool     have33, have38;
    bool     srcUsable;                    // у моніторі справді є дзеркало
    bool     mirrorOkNow;                  // чипи вже збігаються
    bool     hdrSumOkNow;                  // сума заголовка ціла зараз
    uint8_t  now[IMPRES_MIRROR_LEN];       // що лежить у DS2433 зараз
    uint8_t  src[IMPRES_MIRROR_LEN];       // що каже DS2438
    uint8_t  out[IMPRES_MIRROR_LEN];       // що БУДЕ записано (з урахуванням правок)
    bool     diff[IMPRES_MIRROR_LEN];      // де now і src розходяться
    bool     take[IMPRES_MIRROR_LEN];      // які байти справді переносимо
    int      diffCount;
    int      ratedNow, ratedSrc;           // мА·год, 0 — байт не схожий на ємність
    int      ratedUser;                    // вписано вручну, 0 — ні
};

// Перерахунок «байт <-> мА·год» — той самий крок 25 мА·год, що й у решті
// проєкту (impresRatedFromDump). Окремо, бо тут працюємо з байтом дзеркала, а
// не з дампом цілком.
inline int mirrorRatedFromByte(uint8_t b) {
    int mah = (int)b * IMPRES_RATED_STEP;
    return (mah >= IMPRES_RATED_MIN_MAH && mah <= IMPRES_RATED_MAX_MAH) ? mah : 0;
}
inline uint8_t mirrorByteFromRated(int mah) {
    long b = ((long)mah + IMPRES_RATED_STEP / 2) / IMPRES_RATED_STEP;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint8_t)b;
}

// Перерахувати out[] за поточними прапорцями take[] і ручною ємністю.
//  Викликається після КОЖНОЇ зміни плану — щоб «що буде записано» ніколи не
//  розходилось із тим, що показано користувачу.
inline void mirrorPlanRecalc(MirrorPlan &p) {
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.out[i] = p.take[i] ? p.src[i] : p.now[i];
    // Ручна ємність — ПОВЕРХ усього: користувач вписав її саме тому, що ні
    // чипу, ні монітору тут не довіряє (типово після заміни банок).
    if (p.ratedUser > 0)
        p.out[MIRROR_RATED_IDX] = mirrorByteFromRated(p.ratedUser);
}

// Побудувати план. d38 може бути nullptr — тоді переносити нічого, але план
// усе одно показує, що зараз лежить у DS2433.
inline void mirrorPlanBuild(MirrorPlan &p, const uint8_t *d33, const uint8_t *d38) {
    memset(&p, 0, sizeof(p));
    p.have33 = (d33 != nullptr);
    p.have38 = (d38 != nullptr);
    if (!p.have33) return;

    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.now[i] = d33[IMPRES_MIRROR_D33_AT + i];
    p.hdrSumOkNow = impresHeaderOk(d33);
    p.ratedNow    = mirrorRatedFromByte(p.now[MIRROR_RATED_IDX]);

    if (p.have38) {
        for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
            p.src[i] = d38[IMPRES_MIRROR_D38_AT + i];
        p.srcUsable   = impresMirrorUsable(d38);
        p.mirrorOkNow = impresMirrorOk(d33, d38);
        p.ratedSrc    = mirrorRatedFromByte(p.src[MIRROR_RATED_IDX]);
    }

    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
        p.diff[i] = p.have38 && (p.now[i] != p.src[i]);
        if (p.diff[i]) p.diffCount++;
        // ⚑ ТИПОВО БЕРЕМО ЛИШЕ ТЕ, ЩО СПРАВДІ РІЗНЕ, І ЛИШЕ З ПРИДАТНОГО
        //  ДЖЕРЕЛА. Умикати все підряд означало б «перезаписати ідентичність
        //  байтами монітора» — а монітор буває чужий. Однакові байти писати
        //  теж нема сенсу: це зайвий цикл запису EEPROM без жодної користі.
        p.take[i] = p.diff[i] && p.srcUsable;
    }
    mirrorPlanRecalc(p);
}

// Увімкнути/вимкнути перенесення всіх РІЗНИХ байтів одразу.
inline void mirrorPlanTakeAll(MirrorPlan &p, bool on) {
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++)
        p.take[i] = on && p.diff[i] && p.srcUsable;
    mirrorPlanRecalc(p);
}

// Один байт — руками. Поза межами дзеркала мовчки нічого не робимо.
inline void mirrorPlanTakeOne(MirrorPlan &p, int idx, bool on) {
    if (idx < 0 || idx >= IMPRES_MIRROR_LEN) return;
    p.take[idx] = on && p.have38;
    mirrorPlanRecalc(p);
}

// Ручна паспортна ємність, мА·год. 0 — скасувати ручне значення.
//  Повертає ФАКТИЧНО збережене число (після округлення до кроку 25) — щоб
//  клієнт показав те, що буде записано, а не те, що набрали.
inline int mirrorPlanSetRated(MirrorPlan &p, int mah) {
    if (mah <= 0) { p.ratedUser = 0; mirrorPlanRecalc(p); return 0; }
    if (mah < IMPRES_RATED_MIN_MAH) mah = IMPRES_RATED_MIN_MAH;
    if (mah > IMPRES_RATED_MAX_MAH) mah = IMPRES_RATED_MAX_MAH;
    p.ratedUser = mirrorRatedFromByte(mirrorByteFromRated(mah));
    mirrorPlanRecalc(p);
    return p.ratedUser;
}

// Скільки байтів справді зміниться, якщо застосувати план ЗАРАЗ.
inline int mirrorPlanChanges(const MirrorPlan &p) {
    int n = 0;
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) if (p.out[i] != p.now[i]) n++;
    return n;
}

// Застосувати план до дампа DS2433. Повертає кількість змінених байтів.
//  Суму заголовка виправляємо ЗАВЖДИ, навіть коли жоден байт не змінився:
//  саме заради неї цю операцію часто й запускають після зарядної станції —
//  вона переписує дзеркало, але суму не чіпає, і заголовок лишається
//  структурно невалідним при правильних даних.
inline int mirrorPlanApply(const MirrorPlan &p, uint8_t *d33) {
    if (!d33 || !p.have33) return 0;
    int n = 0;
    for (int i = 0; i < IMPRES_MIRROR_LEN; i++) {
        if (d33[IMPRES_MIRROR_D33_AT + i] != p.out[i]) {
            d33[IMPRES_MIRROR_D33_AT + i] = p.out[i];
            n++;
        }
    }
    impresFixHeader(d33);
    return n;
}
