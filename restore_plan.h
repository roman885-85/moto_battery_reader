#ifndef RESTORE_PLAN_H
#define RESTORE_PLAN_H
// ===========================================================================
//  ПЛАН ПРАВОК ЕТАЛОНА ПЕРЕД ЗАПИСОМ
//
//  Шаблон у templates.h — побайтова копія ОДНОГО реального акумулятора. Разом
//  із моделлю, кривою й таблицею він несе й числа, які належать ДОНОРУ:
//  рівень заряду в паливомірі, шунт вимірювача струму, апаратний OFFSET АЦП,
//  наробіток. Записати їх у чужий пакет — значить збрехати і рації, і зарядній
//  станції: вона побачить «повний» пакет, який насправді розряджений, або
//  порахує струм за шунтом, якого в цьому пакеті немає.
//
//  Модельну частину еталона (ідентичність, крива, copyright, заводська
//  таблиця) переносити ТРЕБА — вона однакова в усіх екземплярів моделі.
//  А ось перелічені вище поля треба взяти з САМОГО пакета, який ремонтуємо.
//
//  Цей файл — чиста арифметика без вводу-виводу: він лише каже, що пропонує
//  змінити, що каже еталон, що каже пакет і що буде записано. Рішення
//  ухвалює користувач (галочки в клієнті), запис робить web_server.h.
//
//  ⚠️ Заряд береться з РЕАЛЬНОЇ НАПРУГИ пакета, а не з його ж паливоміра:
//  після заміни елементів паливомір показує стан старих банок і довіряти йому
//  не можна. Напруга — єдине, що вимірюється тут і зараз.
// ===========================================================================
#include <string.h>
#include <stdio.h>
#include "impres_format.h"
#include "impres_bms.h"

enum {
    RPF_CHARGE = 0,   // паливомір ICA з реальної напруги пакета
    RPF_RSENSE,       // шунт вимірювача струму (DS2438[56..57])
    RPF_ADCOFF,       // апаратний OFFSET АЦП струму (DS2438[0x0D..0x0E])
    RPF_ETM,          // наробіток -> «дата першого користування» в рації
    RPF_RATED,        // паспортна ємність (DS2433[0x008])
    RPF_COUNT
};

struct RestoreFixDoc {
    const char *key;
    const char *title;
    const char *unit;      // для показу
    uint8_t     chip;      // у яку мікросхему піде правка (OPC_* сумісно: 1=33, 2=38)
    bool        defOn;     // чи вмикати за замовчуванням
    const char *detail;
};

// defOn = false лише в наробітку: після заміни елементів «вік» пакета
// починається наново, і саме цього зазвичай і хочуть. Але це вибір власника, а
// не наш, тож правка є — просто вимкнена.
static const RestoreFixDoc RESTORE_FIX_DOC[RPF_COUNT] = {
    { "charge", "Рівень заряду (паливомір)", "%", 2, true,
      "Еталон несе заряд ДОНОРА на момент зняття копії. Беремо реальний — із "
      "виміряної зараз напруги цього пакета (7.20 В = 0 %, 8.25 В = 100 %). "
      "Власний паливомір пакета тут не джерело: після заміни елементів він "
      "описує старі банки." },
    { "rsense", "Шунт вимірювача струму", "мОм", 2, true,
      "У різних моделей шунт відрізняється майже вдвічі (0.025 проти 0.046 Ом). "
      "Чужий шунт робить неправильними струм, залишок і знос одразу. Беремо той, "
      "що записаний у моніторі САМОГО пакета." },
    { "adcoff", "OFFSET АЦП струму", "", 2, true,
      "Заводське калібрування нуля вимірювача струму, індивідуальне для кожного "
      "екземпляра. Значення донора зсуне всі виміри струму цього пакета." },
    { "etm",    "Наробіток (дата першого користування)", "діб", 2, false,
      "Рація показує «дату першого користування» як «зараз мінус наробіток». "
      "Вимкнено — лічильник почнеться з нуля (пакет із новими елементами — новий). "
      "Увімкнено — збережеться реальний вік цього пакета." },
    { "rated",  "Паспортна ємність", "мА·год", 1, false,
      "Ємність, яку пакет носить у собі (DS2433, 0x008). Зазвичай збігається з "
      "еталоном моделі; розбіжність означає інший підваріант — або що в пакет "
      "уже вписували ємність вручну." },
};

struct RestoreFix {
    bool avail;      // чи є в пакеті звідки взяти
    bool on;         // чи застосовувати
    long tplVal;     // що каже еталон
    long packVal;    // що каже цей пакет
    long useVal;     // що буде записано (== packVal, коли on)
};

struct RestorePlan {
    char     model[16];
    bool     haveTpl33, haveTpl38;
    bool     havePack33, havePack38;
    uint16_t packMv;        // виміряна напруга пакета, мВ
    int      packPct;       // з неї — заряд, %
    uint16_t tplMv;         // напруга, зашита в еталоні (для показу «чиє це число»)
    int      ratedMah;      // за яким рахували паливомір
    float    rsPack, rsTpl;
    uint8_t  icaTpl, icaPack, icaUse;
    RestoreFix fx[RPF_COUNT];
};

// Правдоподібна напруга Li-Ion 2S: нижче — обрив/не читалось, вище — сміття.
#define RP_MV_MIN 3000
#define RP_MV_MAX 9500
// Наробіток більше 20 років — не дані, а сміття (або чужий монітор).
#define RP_ETM_MAX (20UL * 365UL * 24UL * 3600UL)

inline uint16_t restoreRsRaw(const uint8_t *d38) {
    return d38 ? (uint16_t)(d38[56] | (d38[57] << 8)) : 0;
}
inline uint16_t restoreAdcOff(const uint8_t *d38) {
    return d38 ? (uint16_t)(d38[0x0D] | (d38[0x0E] << 8)) : 0;
}

// Скласти план. tpl33/tpl38 — еталон моделі (tpl38 може бути nullptr), pack33/
// pack38 — те, що зараз у пакеті (теж може бути nullptr, якщо чіп не читається).
inline void restorePlanBuild(RestorePlan &p, const char *model,
                             const uint8_t *tpl33, const uint8_t *tpl38,
                             const uint8_t *pack33, const uint8_t *pack38) {
    memset(&p, 0, sizeof(p));
    snprintf(p.model, sizeof(p.model), "%s", model ? model : "");
    p.haveTpl33 = tpl33 != nullptr;
    p.haveTpl38 = tpl38 != nullptr;
    p.havePack33 = pack33 != nullptr;
    p.havePack38 = pack38 != nullptr;

    // Паспортна ємність — за якою рахуємо паливомір. Спершу з еталона (це
    // властивість моделі), інакше з таблиці.
    p.ratedMah = impresRatedMahFor(tpl33, p.model);

    p.rsTpl  = tpl38  ? impresBmsRsense(tpl38)  : 0.0f;
    p.rsPack = pack38 ? impresBmsRsense(pack38) : 0.0f;
    p.tplMv  = tpl38  ? impresVoltageMv(tpl38)  : 0;
    p.packMv = pack38 ? impresVoltageMv(pack38) : 0;

    // --- заряд ------------------------------------------------------------
    RestoreFix &c = p.fx[RPF_CHARGE];
    p.icaTpl  = tpl38  ? tpl38[0x0C]  : 0;
    p.icaPack = pack38 ? pack38[0x0C] : 0;
    c.avail = pack38 && p.packMv >= RP_MV_MIN && p.packMv <= RP_MV_MAX;
    if (c.avail) {
        p.packPct = impresPercentFromMv(p.packMv);
        // Шунт беремо той, що реально опиниться в чипі: якщо правку шунта
        // ввімкнено — пакетний, інакше — еталонний. Інакше паливомір
        // порахували б за одним шунтом, а читали за іншим.
        float rs = (p.rsPack > 0.0f) ? p.rsPack : p.rsTpl;
        p.icaUse = impresIcaFromPercentRs(p.packPct, p.ratedMah, rs);
    } else {
        p.packPct = -1;
        p.icaUse  = p.icaTpl;
    }
    c.on = c.avail && RESTORE_FIX_DOC[RPF_CHARGE].defOn;
    c.tplVal  = tpl38 ? impresPercentFromIca(p.icaTpl, p.ratedMah,
                            p.rsTpl > 0.0f ? p.rsTpl : DS2438_RSENSE_OHM) : -1;
    c.packVal = p.packPct;
    c.useVal  = c.on ? p.packPct : c.tplVal;

    // --- шунт --------------------------------------------------------------
    RestoreFix &r = p.fx[RPF_RSENSE];
    r.avail   = p.rsPack > 0.0f;
    r.on      = r.avail && RESTORE_FIX_DOC[RPF_RSENSE].defOn;
    r.tplVal  = restoreRsRaw(tpl38);
    r.packVal = restoreRsRaw(pack38);
    r.useVal  = r.on ? r.packVal : r.tplVal;

    // --- OFFSET АЦП ---------------------------------------------------------
    RestoreFix &a = p.fx[RPF_ADCOFF];
    uint16_t off = restoreAdcOff(pack38);
    a.avail   = pack38 && off != 0x0000 && off != 0xFFFF;
    a.on      = a.avail && RESTORE_FIX_DOC[RPF_ADCOFF].defOn;
    a.tplVal  = restoreAdcOff(tpl38);
    a.packVal = off;
    a.useVal  = a.on ? a.packVal : a.tplVal;

    // --- наробіток ----------------------------------------------------------
    RestoreFix &e = p.fx[RPF_ETM];
    uint32_t etm = pack38 ? impresEtm(pack38) : 0;
    e.avail   = pack38 && etm > 0 && etm < RP_ETM_MAX;
    e.on      = e.avail && RESTORE_FIX_DOC[RPF_ETM].defOn;
    e.tplVal  = 0;                       // без правки наробіток обнуляється
    e.packVal = (long)etm;
    e.useVal  = e.on ? e.packVal : 0;

    // --- паспортна ємність ---------------------------------------------------
    RestoreFix &m = p.fx[RPF_RATED];
    int packRated = impresRatedFromDump(pack33);
    int tplRated  = impresRatedFromDump(tpl33);
    // Пропонуємо лише коли пакет каже щось СВОЄ й інше: збіг нічого не міняє,
    // а показувати правку-пустушку означає привчати не читати список.
    m.avail   = packRated > 0 && tplRated > 0 && packRated != tplRated;
    m.on      = m.avail && RESTORE_FIX_DOC[RPF_RATED].defOn;
    m.tplVal  = tplRated;
    m.packVal = packRated;
    m.useVal  = m.on ? m.packVal : m.tplVal;
}

// Увімкнути/вимкнути правки маскою бітів (біт i = RPF_i). Недоступну правку
// увімкнути не можна: брати значення нема звідки.
inline void restorePlanSetMask(RestorePlan &p, uint32_t mask) {
    for (int i = 0; i < RPF_COUNT; i++)
        p.fx[i].on = p.fx[i].avail && (mask & (1UL << i));
    // Заряд рахується за шунтом, який реально опиниться в чипі.
    float rs = p.fx[RPF_RSENSE].on ? p.rsPack : p.rsTpl;
    if (rs <= 0.0f) rs = (p.rsPack > 0.0f) ? p.rsPack : p.rsTpl;
    if (p.fx[RPF_CHARGE].on && p.packPct >= 0)
        p.icaUse = impresIcaFromPercentRs(p.packPct, p.ratedMah, rs);
    else
        p.icaUse = p.icaTpl;
    for (int i = 0; i < RPF_COUNT; i++)
        p.fx[i].useVal = p.fx[i].on ? p.fx[i].packVal
                                    : (i == RPF_ETM ? 0 : p.fx[i].tplVal);
}

inline uint32_t restorePlanMask(const RestorePlan &p) {
    uint32_t m = 0;
    for (int i = 0; i < RPF_COUNT; i++) if (p.fx[i].on) m |= (1UL << i);
    return m;
}

// Маска зі списку ключів через кому: "charge,rsense". Порожній рядок -> 0.
// Рядок "*" або "default" -> типові значення (те, що вже в плані).
inline uint32_t restoreMaskFromKeys(const char *s, const RestorePlan &p) {
    if (!s || !*s) return 0;
    if (!strcmp(s, "*") || !strcmp(s, "default")) return restorePlanMask(p);
    uint32_t m = 0;
    for (int i = 0; i < RPF_COUNT; i++) {
        const char *k = RESTORE_FIX_DOC[i].key;
        size_t kl = strlen(k);
        for (const char *q = s; (q = strstr(q, k)) != nullptr; q += kl) {
            bool lb = (q == s) || q[-1] == ',' || q[-1] == ' ';
            char after = q[kl];
            bool rb = (after == '\0' || after == ',' || after == ' ');
            if (lb && rb) { m |= (1UL << i); break; }
        }
    }
    return m;
}

// Застосувати план до буферів, у які ВЖЕ завантажено еталон. d38 може бути
// nullptr (для моделі немає еталона монітора — тоді монітор пакета не чіпаємо
// зовсім, і правки монітора просто нікуди не пишуться).
inline void restorePlanApply(const RestorePlan &p, uint8_t *d33, uint8_t *d38) {
    if (d38) {
        if (p.fx[RPF_RSENSE].on) {
            d38[56] = (uint8_t)(p.fx[RPF_RSENSE].useVal & 0xFF);
            d38[57] = (uint8_t)((p.fx[RPF_RSENSE].useVal >> 8) & 0xFF);
        }
        if (p.fx[RPF_ADCOFF].on) {
            d38[0x0D] = (uint8_t)(p.fx[RPF_ADCOFF].useVal & 0xFF);
            d38[0x0E] = (uint8_t)((p.fx[RPF_ADCOFF].useVal >> 8) & 0xFF);
        }
        // Наробіток пишемо ПІСЛЯ impresResetMonitor() — той його обнуляє.
        uint32_t etm = (uint32_t)(p.fx[RPF_ETM].on ? p.fx[RPF_ETM].useVal : 0);
        d38[8]  = (uint8_t)(etm & 0xFF);
        d38[9]  = (uint8_t)((etm >> 8) & 0xFF);
        d38[10] = (uint8_t)((etm >> 16) & 0xFF);
        d38[11] = (uint8_t)((etm >> 24) & 0xFF);
        d38[0x0C] = p.icaUse;
    }
    if (d33 && p.fx[RPF_RATED].on) {
        d33[IMPRES_RATED_BYTE] = (uint8_t)(p.fx[RPF_RATED].useVal / IMPRES_RATED_STEP);
        impresFixHeader(d33);
    }
}

// Людський текст значення правки — щоб веб, USB і екран показували однаково.
inline void restoreFixText(const RestorePlan &p, int i, long v, char *out, size_t n) {
    if (v < 0) { snprintf(out, n, "—"); return; }
    switch (i) {
        case RPF_CHARGE: snprintf(out, n, "%ld %%", v); break;
        case RPF_RSENSE: snprintf(out, n, "%.2f мОм", v / 100.0); break;
        case RPF_ADCOFF: snprintf(out, n, "0x%04lX", (unsigned long)v); break;
        case RPF_ETM:    v ? snprintf(out, n, "%ld діб", v / 86400L)
                           : snprintf(out, n, "з нуля"); break;
        case RPF_RATED:  snprintf(out, n, "%ld мА·год", v); break;
        default:         snprintf(out, n, "%ld", v); break;
    }
}

#endif // RESTORE_PLAN_H
