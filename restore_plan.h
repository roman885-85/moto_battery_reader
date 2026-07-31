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
#include "settings.h"        // BATTERY_SCALE_TXT — шкала «заряд за напругою»
#include "impres_format.h"
#include "impres_bms.h"
#include "impres_crypt.h"   // перешифрування дат/лічильників під ROM цього чипа

enum {
    RPF_CHARGE = 0,   // паливомір ICA з реальної напруги пакета
    RPF_RSENSE,       // шунт вимірювача струму (DS2438[56..57])
    RPF_ADCOFF,       // апаратний OFFSET АЦП струму (DS2438[0x0D..0x0E])
    RPF_ETM,          // наробіток -> «дата першого користування» в рації
    RPF_RATED,        // паспортна ємність (DS2433[0x008])
    RPF_CRYPT,        // дати й лічильники — під ROM ЦЬОГО чипа (шифрування)
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
      "виміряної зараз напруги цього пакета (" BATTERY_SCALE_TXT "). "
      "Власний паливомір пакета тут не джерело: після заміни елементів він "
      "описує старі банки." },
    { "rsense", "Шунт вимірювача струму", "мОм", 2, true,
      "У різних моделей шунт відрізняється майже вдвічі (0.025 проти 0.046 Ом). "
      "Чужий шунт робить неправильними струм, залишок і знос одразу. Беремо той, "
      "що записаний у моніторі САМОГО пакета. Якщо свого немає (монітор чистий "
      "або шунт занулено) — впишіть опір вручну чи візьміть його з еталона "
      "потрібної моделі." },
    { "adcoff", "OFFSET АЦП струму", "", 2, true,
      "Заводське калібрування нуля вимірювача струму, індивідуальне для кожного "
      "екземпляра. Значення донора зсуне всі виміри струму цього пакета." },
    { "etm",    "Наробіток (дата першого користування)", "діб", 2, false,
      "Рація показує «дату першого користування» як «зараз мінус наробіток». "
      "Вимкнено — лічильник почнеться з нуля (пакет із новими елементами — новий). "
      "Увімкнено — збережеться реальний вік цього пакета." },
    { "rated",  "Паспортна ємність", "мА·год", 1, false,
      "Ємність, яку пакет носить у собі (DS2433, 0x008). Зазвичай збігається з "
      "еталоном моделі; розбіжність означає інший підваріант. Якщо після заміни "
      "ви поставили банки іншої ємності — впишіть її вручну: за нею ж "
      "перерахується й паливомір." },
    { "crypt",  "Дати й лічильники — під ROM цього чипа", "", 1, true,
      "Частина полів DS2433 зашифрована, і ключ береться з ROM-ID самого чипа. "
      "Еталон знято з ЧУЖОГО акумулятора, тож рація розшифрує його поля своїм "
      "ключем і побачить сміття: на реальному пакеті це була дата виготовлення "
      "«2107-13-21», 3548 циклів при 1097 і знос 76 % замість 34 %. Правка "
      "перешифровує ті самі числа під ROM цього чипа — значення не міняються, "
      "міняється лише те, хто здатен їх прочитати." },
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
    int      ratedTpl;      // паспортна за еталоном/таблицею моделі
    int      ratedPack;     // що каже сам пакет (DS2433[0x008]), 0 — не читається
    int      ratedUser;     // введена ВРУЧНУ після заміни банок, 0 — не вводили
    int      ratedMah;      // ЕФЕКТИВНА — саме за нею рахується паливомір
    float    rsPack, rsTpl; // шунт пакета й еталона, Ом (0 — немає)
    uint16_t rsRawPack, rsRawTpl;   // ті самі числа, як вони лежать у чипі
    long     rsUser;        // вписаний ВРУЧНУ або взятий із бібліотеки, 0 — ні
    char     rsSrc[16];     // звідки взято ручний шунт: модель або "" (вручну)
    float    rsUse;         // ЕФЕКТИВНИЙ шунт — за ним рахується паливомір
    uint8_t  icaTpl, icaPack, icaUse;

    // --- шифрування ---------------------------------------------------------
    bool     haveRom;       // чи відомий ROM DS2433 цього чипа (тільки на пристрої)
    uint8_t  romK1, romK2;  // ключ ЦЬОГО чипа: ROM[1] і ROM[6]
    bool     cryptSrcOk;    // чи вдалось визначити ключ, яким зашифровано вміст
    uint8_t  srcK1, srcK2;
    bool     cryptWrong;    // вміст зашифровано ЧУЖИМ ключем — рація бачить сміття
    ImpresCryptFields cf;   // справжні значення, прочитані ключем джерела
    int      mfgY, mfgM, mfgD;         // справжня дата виготовлення
    int      seenY, seenM, seenD;      // яку дату бачить рація ЗАРАЗ
    int      mfgUserY, mfgUserM, mfgUserD;   // вписана ВРУЧНУ, 0 — не вписували

    RestoreFix fx[RPF_COUNT];
};

// Дата як одне число YYYYMMDD — щоб класти її в long-поля правки й показувати
// однаково в усіх клієнтах.
inline long restoreDateNum(int y, int m, int d) {
    return (y > 0) ? (long)y * 10000 + m * 100 + d : 0;
}

// Правдоподібна напруга Li-Ion 2S: нижче — обрив/не читалось, вище — сміття.
#define RP_MV_MIN 3000
#define RP_MV_MAX 9500
// Наробіток більше 20 років — не дані, а сміття (або чужий монітор).
#define RP_ETM_MAX (20UL * 365UL * 24UL * 3600UL)
// Шунт у чипі — це Ом × 100000, тобто мОм × 100. Реальні IMPRES-шунти лежать
// між 0.025 і 0.046 Ом; межі беремо ширші (1..200 мОм), щоб не заважати
// нестандартним збіркам, але й не дати вписати нуль чи явне сміття.
#define RP_RS_MIN_RAW 100L
#define RP_RS_MAX_RAW 20000L

inline uint16_t restoreRsRaw(const uint8_t *d38) {
    return d38 ? (uint16_t)(d38[56] | (d38[57] << 8)) : 0;
}
inline uint16_t restoreAdcOff(const uint8_t *d38) {
    return d38 ? (uint16_t)(d38[0x0D] | (d38[0x0E] << 8)) : 0;
}

inline void restorePlanRecalc(RestorePlan &p);   // визначена нижче

// Скласти план. tpl33/tpl38 — еталон моделі (tpl38 може бути nullptr), pack33/
// pack38 — те, що зараз у пакеті (теж може бути nullptr, якщо чіп не читається).
inline void restorePlanBuild(RestorePlan &p, const char *model,
                             const uint8_t *tpl33, const uint8_t *tpl38,
                             const uint8_t *pack33, const uint8_t *pack38,
                             const uint8_t *packRom33 = nullptr) {
    memset(&p, 0, sizeof(p));
    snprintf(p.model, sizeof(p.model), "%s", model ? model : "");
    p.haveTpl33 = tpl33 != nullptr;
    p.haveTpl38 = tpl38 != nullptr;
    p.havePack33 = pack33 != nullptr;
    p.havePack38 = pack38 != nullptr;

    // Паспортна ємність за еталоном — властивість моделі; далі її може
    // перекрити те, що носить сам пакет, або введена вручну після заміни банок.
    p.ratedTpl = impresRatedMahFor(tpl33, p.model);
    p.ratedPack = impresRatedFromDump(pack33);
    p.ratedUser = 0;
    p.ratedMah = p.ratedTpl;

    p.rsTpl  = tpl38  ? impresBmsRsense(tpl38)  : 0.0f;
    p.rsPack = pack38 ? impresBmsRsense(pack38) : 0.0f;
    p.tplMv  = tpl38  ? impresVoltageMv(tpl38)  : 0;
    p.packMv = pack38 ? impresVoltageMv(pack38) : 0;

    // --- заряд ------------------------------------------------------------
    RestoreFix &c = p.fx[RPF_CHARGE];
    p.icaTpl  = tpl38  ? tpl38[0x0C]  : 0;
    p.icaPack = pack38 ? pack38[0x0C] : 0;
    c.avail  = pack38 && p.packMv >= RP_MV_MIN && p.packMv <= RP_MV_MAX;
    p.packPct = c.avail ? impresPercentFromMv(p.packMv) : -1;
    c.on      = c.avail && RESTORE_FIX_DOC[RPF_CHARGE].defOn;
    c.packVal = p.packPct;

    // --- шунт --------------------------------------------------------------
    // Свій шунт є не завжди: у «чистому» моніторі 0x38..0x39 нулі. Тоді правку
    // все одно показуємо — але значення для неї треба дати ззовні (вручну або
    // з еталона іншої моделі), і робить це restorePlanSetRsense().
    RestoreFix &r = p.fx[RPF_RSENSE];
    p.rsRawTpl  = restoreRsRaw(tpl38);
    p.rsRawPack = restoreRsRaw(pack38);
    p.rsUser    = 0;
    p.rsSrc[0]  = '\0';
    r.on        = (p.rsRawPack > 0) && RESTORE_FIX_DOC[RPF_RSENSE].defOn;

    // --- OFFSET АЦП ---------------------------------------------------------
    RestoreFix &a = p.fx[RPF_ADCOFF];
    uint16_t off = restoreAdcOff(pack38);
    a.avail   = pack38 && off != 0x0000 && off != 0xFFFF;
    a.on      = a.avail && RESTORE_FIX_DOC[RPF_ADCOFF].defOn;
    a.tplVal  = restoreAdcOff(tpl38);
    a.packVal = off;

    // --- наробіток ----------------------------------------------------------
    RestoreFix &e = p.fx[RPF_ETM];
    uint32_t etm = pack38 ? impresEtm(pack38) : 0;
    e.avail   = pack38 && etm > 0 && etm < RP_ETM_MAX;
    e.on      = e.avail && RESTORE_FIX_DOC[RPF_ETM].defOn;
    e.tplVal  = 0;                       // без правки наробіток обнуляється
    e.packVal = (long)etm;

    // --- шифрування ----------------------------------------------------------
    // Зашифровані поля читаються ключем із ROM чипа DS2433. Еталон знято з
    // ЧУЖОГО пакета, тож його поля зашифровані ROM'ом донора — і рація,
    // розшифрувавши їх СВОЇМ ключем, побачить сміття (dumps/16). Тут ми лише
    // дивимось, чи так воно і є; перешифрування робить restorePlanApply().
    p.haveRom = packRom33 != nullptr;
    if (p.haveRom) { p.romK1 = packRom33[1]; p.romK2 = packRom33[6]; }
    RestoreFix &cr = p.fx[RPF_CRYPT];
    if (pack33) {
        p.cryptSrcOk = impresCryptSourceKey(pack33, pack38, &p.srcK1, &p.srcK2);
        if (p.cryptSrcOk) {
            impresCryptRead(pack33, p.srcK1, p.srcK2, &p.cf);
            p.mfgY = p.cf.mfgY; p.mfgM = p.cf.mfgM; p.mfgD = p.cf.mfgD;
            if (p.haveRom) {
                p.cryptWrong = impresCryptKeyDiffers(p.srcK1, p.srcK2, p.romK1, p.romK2);
                // Що бачить рація ЗАРАЗ — тими самими байтами, але СВОЇМ ключем.
                ImpresCryptFields seen;
                impresCryptRead(pack33, p.romK1, p.romK2, &seen);
                p.seenY = seen.mfgY; p.seenM = seen.mfgM; p.seenD = seen.mfgD;
            }
        }
    }
    cr.on = p.cryptWrong && RESTORE_FIX_DOC[RPF_CRYPT].defOn;

    // --- паспортна ємність ---------------------------------------------------
    RestoreFix &m = p.fx[RPF_RATED];
    m.tplVal  = p.ratedTpl;
    m.packVal = p.ratedPack;
    m.on      = false;                   // вмикається лише свідомо (див. нижче)

    restorePlanRecalc(p);
}

// Округлити введену ємність до того, що взагалі можна записати: байт 0x008 —
// це мА·год / 25, тож проміжні значення однаково не збережуться. Нуль означає
// «не вводили».
inline int restoreRatedClamp(long mah) {
    if (mah <= 0) return 0;
    long v = ((mah + IMPRES_RATED_STEP / 2) / IMPRES_RATED_STEP) * IMPRES_RATED_STEP;
    if (v < IMPRES_RATED_MIN_MAH) v = IMPRES_RATED_MIN_MAH;
    if (v > IMPRES_RATED_MAX_MAH) v = IMPRES_RATED_MAX_MAH;
    return (int)v;
}

// Перерахувати все, що залежить від увімкнених правок. Викликається після
// кожної зміни: паливомір залежить і від шунта, і від паспортної ємності, тож
// рахувати його «десь один раз» не можна.
inline void restorePlanRecalc(RestorePlan &p) {
    RestoreFix &m = p.fx[RPF_RATED];
    // Правку ємності пропонуємо, коли є що запропонувати: або пакет каже своє
    // й інше, або власник вписав ємність нових банок. Збіг нічого не міняє, а
    // правка-пустушка привчає не читати список.
    long want = p.ratedUser > 0 ? p.ratedUser : p.ratedPack;
    m.avail   = (p.ratedUser > 0) || (p.ratedPack > 0 && p.ratedTpl > 0 && p.ratedPack != p.ratedTpl);
    m.packVal = want;
    if (!m.avail) m.on = false;
    // Введена вручну ємність — це свідома дія, вмикаємо одразу: інакше людина
    // вписала б число й нічого б не сталося.
    if (p.ratedUser > 0) m.on = true;

    // Шунт: своє значення пакета, або вписане вручну / взяте з еталона іншої
    // моделі. Ручний ввід — свідома дія, тож правку вмикаємо одразу: інакше
    // людина вписала б опір і нічого б не сталося.
    RestoreFix &r38 = p.fx[RPF_RSENSE];
    r38.avail   = (p.rsUser > 0) || (p.rsRawPack > 0);
    r38.tplVal  = (long)p.rsRawTpl;
    r38.packVal = p.rsUser > 0 ? p.rsUser : (long)p.rsRawPack;
    if (!r38.avail) r38.on = false;
    if (p.rsUser > 0) r38.on = true;

    // ⚑ ЕФЕКТИВНА паспортна ємність: саме за нею рахується паливомір. Якщо
    // після заміни поставили банки на 3000 замість 2150 — 77 % це вже інший
    // ICA, і рахувати його за старим числом означало б знову записати чуже.
    p.ratedMah = (m.on && want > 0) ? (int)want : p.ratedTpl;

    // Шунт беремо той, що РЕАЛЬНО опиниться в чипі: якщо правку шунта знято,
    // паливомір має рахуватись за шунтом еталона — інакше число записалось би
    // за одним шунтом, а читалось за іншим.
    long rsWant = p.rsUser > 0 ? p.rsUser : (long)p.rsRawPack;
    float rs = (r38.on && rsWant > 0) ? rsWant / 100000.0f : p.rsTpl;
    if (rs <= 0.0f) rs = (rsWant > 0) ? rsWant / 100000.0f : p.rsTpl;
    p.rsUse = rs;

    p.icaUse = (p.fx[RPF_CHARGE].on && p.packPct >= 0)
                 ? impresIcaFromPercentRs(p.packPct, p.ratedMah, rs)
                 : p.icaTpl;

    // Що каже еталон про заряд — показуємо за ЙОГО ж шунтом і ємністю моделі.
    p.fx[RPF_CHARGE].tplVal = p.haveTpl38
        ? impresPercentFromIca(p.icaTpl, p.ratedTpl,
                               p.rsTpl > 0.0f ? p.rsTpl : DS2438_RSENSE_OHM)
        : -1;

    // Шифрування. Правку пропонуємо, коли є ROM цього чипа і або вміст
    // зашифровано чужим ключем, або власник вписав дату виготовлення вручну
    // (на «свіжому» хвості її взяти нізвідки).
    RestoreFix &cr = p.fx[RPF_CRYPT];
    bool haveUserDate = p.mfgUserY > 0;
    cr.avail  = p.haveRom && (p.cryptWrong || (haveUserDate && p.cryptSrcOk));
    cr.tplVal = restoreDateNum(p.seenY, p.seenM, p.seenD);   // що бачить рація зараз
    cr.packVal = haveUserDate ? restoreDateNum(p.mfgUserY, p.mfgUserM, p.mfgUserD)
                              : restoreDateNum(p.mfgY, p.mfgM, p.mfgD);
    if (!cr.avail) cr.on = false;
    if (haveUserDate && cr.avail) cr.on = true;

    for (int i = 0; i < RPF_COUNT; i++)
        p.fx[i].useVal = p.fx[i].on ? p.fx[i].packVal
                                    : (i == RPF_ETM ? 0 : p.fx[i].tplVal);
}

// Увімкнути/вимкнути правки маскою бітів (біт i = RPF_i). Недоступну правку
// увімкнути не можна: брати значення нема звідки.
inline void restorePlanSetMask(RestorePlan &p, uint32_t mask) {
    for (int i = 0; i < RPF_COUNT; i++)
        p.fx[i].on = p.fx[i].avail && (mask & (1UL << i));
    // Ручна ємність — окреме поле, а не галочка: якщо її ввели, але «rated» у
    // масці немає, вважаємо, що користувач передумав писати ємність.
    if (!(mask & (1UL << RPF_RATED)))  p.ratedUser = 0;
    if (!(mask & (1UL << RPF_RSENSE))) { p.rsUser = 0; p.rsSrc[0] = '\0'; }
    if (!(mask & (1UL << RPF_CRYPT)))  { p.mfgUserY = p.mfgUserM = p.mfgUserD = 0; }
    // avail шунта залежить від rsUser, тож маску треба накласти ще раз — уже
    // після того, як ручне значення прибрано.
    p.fx[RPF_RSENSE].on = (mask & (1UL << RPF_RSENSE)) && (p.rsRawPack > 0);
    restorePlanRecalc(p);
}

// Округлити шунт до того, що взагалі можна записати: у чипі це ціле Ом×100000.
inline long restoreRsClamp(long raw) {
    if (raw <= 0) return 0;
    if (raw < RP_RS_MIN_RAW) return RP_RS_MIN_RAW;
    if (raw > RP_RS_MAX_RAW) return RP_RS_MAX_RAW;
    return raw;
}

// Задати шунт вручну або з бібліотеки еталонів. raw — Ом×100000 (== мОм×100),
// 0 — прибрати ручне значення й повернутись до того, що в пакеті. src — звідки
// взято (назва моделі), порожньо/nullptr — вписано руками.
inline void restorePlanSetRsense(RestorePlan &p, long raw, const char *src = nullptr) {
    p.rsUser = restoreRsClamp(raw);
    if (p.rsUser > 0 && src && *src) snprintf(p.rsSrc, sizeof(p.rsSrc), "%s", src);
    else                             p.rsSrc[0] = '\0';
    restorePlanRecalc(p);
}

// Вписати дату виготовлення вручну (рік 0 — прибрати). Межі — ті самі, що в
// impresBmsDateSane(): поза ними дата все одно вважалась би сміттям.
inline void restorePlanSetMfg(RestorePlan &p, int y, int m, int d) {
    if (y < 2005 || y > 2035 || m < 1 || m > 12 || d < 1 || d > 31) y = m = d = 0;
    p.mfgUserY = y; p.mfgUserM = m; p.mfgUserD = d;
    restorePlanRecalc(p);
}

// Вписати ємність нових банок вручну (0 — прибрати, лишити як є).
inline void restorePlanSetRated(RestorePlan &p, long mah) {
    p.ratedUser = restoreRatedClamp(mah);
    restorePlanRecalc(p);
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
//
// onlyEnabled = true — режим «застосувати ЛИШЕ правки» до того, що вже в чипах,
// без перезапису еталона. Тоді вимкнені правки не чіпаються взагалі: інакше
// наробіток пакета обнулився б просто тому, що галочку не поставили, — а тут
// нікому його відновлювати, бо еталон не пишеться.
inline void restorePlanApply(const RestorePlan &p, uint8_t *d33, uint8_t *d38,
                             bool onlyEnabled = false) {
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
        if (p.fx[RPF_ETM].on || !onlyEnabled) {
            uint32_t etm = (uint32_t)(p.fx[RPF_ETM].on ? p.fx[RPF_ETM].useVal : 0);
            d38[8]  = (uint8_t)(etm & 0xFF);
            d38[9]  = (uint8_t)((etm >> 8) & 0xFF);
            d38[10] = (uint8_t)((etm >> 16) & 0xFF);
            d38[11] = (uint8_t)((etm >> 24) & 0xFF);
        }
        if (p.fx[RPF_CHARGE].on || !onlyEnabled) d38[0x0C] = p.icaUse;
    }
    if (d33 && p.fx[RPF_RATED].on) {
        d33[IMPRES_RATED_BYTE] = (uint8_t)(p.fx[RPF_RATED].useVal / IMPRES_RATED_STEP);
        impresFixHeader(d33);
    }
    // Перешифрування — ОСТАННІМ по DS2433: воно лагодить суми своїх блоків, і
    // будь-яка правка після нього ці суми знову зламала б.
    if (d33 && p.fx[RPF_CRYPT].on && p.haveRom) {
        ImpresCryptFields f = p.cf;
        if (p.mfgUserY > 0) {
            f.haveDat = f.haveDat || impresCryptBlockUsable(d33, impresCryptAddr(d33, BMS_V_DATE), 6);
            f.mfgY = p.mfgUserY; f.mfgM = p.mfgUserM; f.mfgD = p.mfgUserD;
        }
        impresCryptWrite(d33, p.romK1, p.romK2, &f);
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
        case RPF_CRYPT:  v ? snprintf(out, n, "%04ld-%02ld-%02ld",
                                      v / 10000, (v / 100) % 100, v % 100)
                           : snprintf(out, n, "—"); break;
        default:         snprintf(out, n, "%ld", v); break;
    }
}

#endif // RESTORE_PLAN_H
