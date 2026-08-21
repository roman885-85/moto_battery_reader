#ifndef RECOVERY_H
#define RECOVERY_H
// ===========================================================================
//  МАЙСТЕР ВІДНОВЛЕННЯ (Recovery Wizard) — двигун + база правил.
//
//  Спільне «серце» автоматичного відновлення для ВСІХ поверхонь (екранне меню,
//  Wi-Fi-веб, USB-веб, десктоп-GUI). Клієнти лише малюють той самий стан у JSON.
//
//  Потік:  1) АНАЛІЗ стану чіпів -> набір проблем (бітова маска);
//          2) ДІАГНОЗ: людські формулювання проблем + пропозиції (база правил);
//          3) ПЛАН: впорядкована послідовність кроків відновлення;
//          4) ВИКОНАННЯ кроку з детальним результатом.
//
//  СТАБІЛЬНИЙ ПЛАН: послідовність дій фіксується в журналі, коли користувач
//  почав виконання. Далі кроки виконуються саме за цим планом (а не
//  перебудовуються щоразу з поточних проблем), тож прогрес не «стрибає» після
//  кожного виправлення.
//
//  Багатоетапність із зарядною станцією: коли план містить «зовнішній» крок
//  (калібрування на IMPRES-ЗП), стан зберігається в журнал у пам'яті ESP32,
//  ПРИВ'ЯЗАНИЙ до ROM-серійника DS2438. Після повернення АКБ Майстер читає
//  журнал, порівнює знімок ключових полів (CCA/DCA/ICA/health) до і після — і
//  розуміє, що зроблено й що робити далі, без запису службових даних у сам АКБ.
//
//  База правил (RECOVERY_RULES) — таблиця: проблема -> діагноз/пропозиція/дія.
//  Розширювати логіку = додати рядок у таблицю.
//
//  Включається в кінці web_server.h (після усіх perform*-функцій і аналітичних
//  хелперів), тож усі символи вже визначені.
// ===========================================================================

struct BatteryDiag {
    uint32_t issues;
    int      capPct;        // здоров'я/ємність, % (-1 якщо невідомо)
    char     model[16];
    int      fmt;           // 0 невідомий / 2014 / 2017 / 2021
    bool     have33, have38;
    bool     hdrOk, mirrorOk, genuine;
    int      tail;          // IMPRES_TAIL_BLANK / _VALID / _BROKEN
};

#include "wizard_rules.h"    // проблеми, дії, таблиця діагнозів і сам план

// ---- Поточний план: масив кодів дій ---------------------------------------
static uint8_t g_wizActs[WIZ_MAX_STEPS];
static int     g_wizActN = 0;

// Тонкий викликач: сам план рахує чиста wizPlanIssues() з wizard_rules.h.
static void wizComputeActions(const BatteryDiag &d) {
    g_wizActN = wizPlanIssues(d.issues, g_wizActs, WIZ_MAX_STEPS);
}

// Журнал продовження (у пам'яті ESP32, прив'язаний до серійника DS2438).
struct WizJournal {
    bool     active;
    char     serial[17];    // hex ROM DS2438
    char     model[16];
    uint8_t  acts[WIZ_MAX_STEPS];
    int      nActs;
    int      done;
    bool     awaitCharge;
    long     snapCCA, snapDCA, snapICA, snapHealth;
};
static WizJournal g_wizJ;
static bool g_wizJInit = false;
static void wizJZero() { memset(&g_wizJ, 0, sizeof(g_wizJ)); g_wizJInit = true; }

// ------------------------------------------------------------------ утиліти
static void wizSerialHex(char *out, size_t n) {
    out[0] = '\0';
    if (!battery.hasRom2438()) return;
    const uint8_t *r = battery.rom2438();
    // Умова саме на 3 байти: sprintf кладе дві цифри ПЛЮС нуль-термінатор, тож
    // місця треба до індексу i*2+2 включно. Стара перевірка (i*2+1 < n)
    // резервувала лише дві — на буфері ПАРНОГО розміру термінатор ліг би на
    // байт за межею. Зараз усі виклики дають 17 і не страждають, але умова
    // мусить бути правильною сама по собі, а не завдяки виклику.
    for (int i = 0; i < 8 && (size_t)(i * 2 + 2) < n; i++) sprintf(out + i * 2, "%02X", r[i]);
}

// ------------------------------------------------------------------ АНАЛІЗ
// Поріг «низьке здоров'я», %. 40 % — це вже пакет, який рація показує як
// зношений; нижче ремонт прошивки не допоможе, потрібні банки.
#ifndef WIZ_HEALTH_LOW_PCT
  #define WIZ_HEALTH_LOW_PCT 40
#endif

static void wizAnalyze(BatteryDiag &d) {
    memset(&d, 0, sizeof(d));
    d.capPct = -1;
    d.have33 = hasDump;
    d.have38 = hasDump2438;

    if (!hasDump && !hasDump2438) { d.issues |= ISS_NO_CHIP; return; }
    if (!hasDump2438)             d.issues |= ISS_NO_2438;

    if (hasDump) {
        bool blank = true;
        for (int i = 0; i < 0x20; i++) if (batteryDump[i] != 0xFF) { blank = false; break; }
        if (blank) d.issues |= ISS_BLANK33;

        d.hdrOk = headerChecksumOk(batteryDump);
        if (!d.hdrOk && !blank) d.issues |= ISS_HDR_BAD;

        // «Станція почала добудову» проти просто побитого заголовка. Саме
        // правило — в impres_format.h, ОДНИМ екземпляром: тут воно жило поруч
        // із дослівною копією в тесті, і копія пережила виправлення оригіналу.
        if (impresChargerPartial(batteryDump, batteryDump2438, hasDump2438)) {
            d.issues |= ISS_CHARGER_PARTIAL;
            d.issues &= ~ISS_HDR_BAD;   // конкретніший діагноз — той самий крок ремонту,
        }                               // але людині зрозуміліше, що саме сталося.

        if (!decodeModel(d.model, sizeof(d.model))) { d.issues |= ISS_NO_MODEL; d.model[0] = '\0'; }

        // ⚑ БУЛО ДРУГОЮ КОПІЄЮ. Тут стояв wizDetectFormat() — дослівно те
        //  саме правило (три маркери COPYRIGHT/MOTOROLA), що й
        //  impresFormatYear() в impres_format.h. Копія жила в файлі, який на
        //  хості не збирається, тобто перевіряли ми одну, а працювала інша.
        d.fmt = impresFormatYear(batteryDump);

        // ⚑ ЗДОРОВ'Я. Раніше тут стояв decodeCapacity(), який ПРИНЦИПОВО
        // повертає false (байт заводської таблиці — стала моделі, а не знос).
        // Через це ISS_HEALTH_LOW не спрацьовував ЖОДНОГО разу, а разом із ним
        // мовчав і ISS_NEEDS_CALIB: із трьох його умов лишалась фактично одна
        // (ICA == 0). Саме тому частина сценаріїв Майстра «не бачила» проблему.
        //
        // Тепер знос беремо там, де він насправді лежить: поле CTS блока
        // калібрування, розшифроване ключем із ROM-ID чипа (impres_bms.h).
        // Перевірено проти показань рації: 34 / 97 / 99 / 100 %.
        const ImpresBms &wb = impresBmsOf(batteryDump,
                                          hasDump2438 ? batteryDump2438 : nullptr,
                                          hasSN2433 ? chipSN2433 : nullptr,
                                          DS2438_RSENSE_OHM);
        if (wb.ok && wb.haveKey && wb.potentialMah > 0) {
            d.capPct = wb.health;
            if (wb.health < WIZ_HEALTH_LOW_PCT) d.issues |= ISS_HEALTH_LOW;
        }
    }

    if (hasDump && hasDump2438) {
        d.mirrorOk = mirrorOk(batteryDump, batteryDump2438);
        if (!d.mirrorOk) d.issues |= ISS_MIRROR_BAD;
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) d.issues |= ISS_CCA_OVERFLOW;
    }

    // ---- навчений калібрувальний хвіст 0x18A..0x1FF -----------------------
    // Саме він вирішує, чи прийме рація пакет після заміни елементів.
    d.tail = hasDump ? impresTailState(batteryDump) : IMPRES_TAIL_BLANK;
    // Сама наявність навченої калібровки — НЕ проблема (у робочому АКБ вона й
    // має бути), тож у issues не потрапляє: інакше кожен справний АКБ показувався
    // б як «нездоровий». Стан хвоста віддаємо окремим полем d.tail.
    // Стертий хвіст — окрема проблема: рація АКБ приймає, але ЗП не має куди
    // писати навчені значення, тож калібрування завжди завершується помилкою.
    if (hasDump && d.tail == IMPRES_TAIL_BLANK && !(d.issues & ISS_BLANK33))
        d.issues |= ISS_TAIL_ERASED;
    if (hasDump && d.tail == IMPRES_TAIL_VALID) {
        // ЧУЖА калібровка: навчені дані про реальні банки присутні, а монітор
        // каже «пакет щойно почав жити» (напрацювання < доби). Такий збіг
        // означає, що хвіст дістався пакету ззовні — записом еталона/донора або
        // лишився від старих банок після скидання лічильників. На всіх 49
        // дампах з dumps/ цей критерій спрацьовує рівно на тих АКБ, де власник
        // зафіксував «невідомий акумулятор» після запису еталона чи скидання
        // (08-nova-batareya 01/02/03, 07-skydannya-vidnovlennya), і не дає
        // хибних спрацювань на жодному робочому АКБ.
        if (hasDump2438 && impresEtm(batteryDump2438) < 86400UL)
            d.issues |= ISS_TAIL_FOREIGN;
    }

    const char *reason = "";
    d.genuine = batteryGenuine(&reason);

    // CTS == 0 означає «потенційну ємність ще не міряли» — пакет новий або
    // після скидання. Це не поломка, але й не робочий стан: поки станція не
    // проведе цикл, рація не знає ємності.
    if (hasDump) {
        const ImpresBms &cb = impresBmsOf(batteryDump,
                                          hasDump2438 ? batteryDump2438 : nullptr,
                                          hasSN2433 ? chipSN2433 : nullptr,
                                          DS2438_RSENSE_OHM);
        if (cb.ok && cb.haveKey && cb.cts == 0) d.issues |= ISS_NEVER_CALIB;
    }

    // ── ШИФРУВАННЯ Й УЗГОДЖЕНІСТЬ ЗМІСТУ ──────────────────────────────────
    //  Сама перевірка — в impres_audit.h: чиста арифметика без вводу-виводу,
    //  щоб той самий код ганявся й на корпусі дампів у тестах. Тут лише
    //  переносимо біти: розрядність AUD_* і ISS_* узгоджена навмисно.
    if (hasDump && !(d.issues & ISS_BLANK33)) {
        int ty = 0, tm = 0, td = 0;
        deviceClockToday(&ty, &tm, &td);          // 0 — годинник не заведено
        uint32_t a = impresAudit(batteryDump,
                                 hasDump2438 ? batteryDump2438 : nullptr,
                                 hasSN2433 ? chipSN2433 : nullptr, ty, tm, td);
        if (a & AUD_CRYPT_WRONG)    d.issues |= ISS_CRYPT_WRONG;
        if (a & AUD_CRYPT_UNKNOWN)  d.issues |= ISS_CRYPT_UNKNOWN;
        if (a & AUD_BLOCK_SUM)      d.issues |= ISS_BLOCK_SUM;
        if (a & AUD_DATE_INSANE)    d.issues |= ISS_DATE_INSANE;
        if (a & AUD_DCA_INSANE)     d.issues |= ISS_DCA_INSANE;
        if (a & AUD_MONITOR_ZEROED) d.issues |= ISS_MONITOR_ZEROED;
        if (a & AUD_ETM_FOREIGN)    d.issues |= ISS_ETM_FOREIGN;
        if (a & AUD_USE_BEFORE_CHG) d.issues |= ISS_USE_BEFORE_CHG;
    }

    bool structOk = d.hdrOk && !(d.issues & (ISS_BLANK33 | ISS_NO_MODEL));
    if (structOk && hasDump2438) {
        uint8_t ica = batteryDump2438[12];
        if ((d.issues & ISS_CCA_OVERFLOW) || ica == 0 ||
            (d.issues & (ISS_HEALTH_LOW | ISS_NEVER_CALIB)))
            d.issues |= ISS_NEEDS_CALIB;
    }
}

// ------------------------------------------------------------------ JSON-хелпер
static void jsonEsc(String &o, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
        else o += *p;
    }
}

// ------------------------------------------------------------------ ЖУРНАЛ
// Кожен АКБ має ВЛАСНИЙ файл журналу, названий за ROM-серійником DS2438:
// "/wj_<serial>.jrn". Тож кілька акумуляторів можуть одночасно бути в процесі
// відновлення (напр. один на зарядній станції, з іншим працюємо). Список і
// видалення таких журналів — через wizJournalListJson()/wizJournalDelete().
#define WIZ_JRN_PREFIX "/wj_"
#define WIZ_JRN_MAX    24            // скільки журналів максимум перелічуємо

static void wizJPath(char *buf, size_t n, const char *serialHex) {
    snprintf(buf, n, WIZ_JRN_PREFIX "%s.jrn", serialHex);
}

static void wizJournalSave() {
    if (!g_wizJ.serial[0]) return;
    char path[40]; wizJPath(path, sizeof(path), g_wizJ.serial);
    File f = SPIFFS.open(path, "w");
    if (!f) return;
    String acts;
    for (int i = 0; i < g_wizJ.nActs; i++) { if (i) acts += ","; acts += String((int)g_wizJ.acts[i]); }
    f.printf("serial=%s\nmodel=%s\nacts=%s\ndone=%d\nawait=%d\ncca=%ld\ndca=%ld\nica=%ld\nhealth=%ld\n",
             g_wizJ.serial, g_wizJ.model, acts.c_str(), g_wizJ.done,
             g_wizJ.awaitCharge ? 1 : 0, g_wizJ.snapCCA, g_wizJ.snapDCA, g_wizJ.snapICA, g_wizJ.snapHealth);
    f.close();
}

// Розібрати один файл журналу у структуру. true, якщо файл валідний (є кроки).
static bool wizJournalParse(const char *path, WizJournal &j) {
    memset(&j, 0, sizeof(j));
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq), v = line.substring(eq + 1);
        if      (k == "serial") strncpy(j.serial, v.c_str(), sizeof(j.serial) - 1);
        else if (k == "model")  strncpy(j.model,  v.c_str(), sizeof(j.model) - 1);
        else if (k == "done")   j.done = v.toInt();
        else if (k == "await")  j.awaitCharge = v.toInt() != 0;
        else if (k == "cca")    j.snapCCA = v.toInt();
        else if (k == "dca")    j.snapDCA = v.toInt();
        else if (k == "ica")    j.snapICA = v.toInt();
        else if (k == "health") j.snapHealth = v.toInt();
        else if (k == "acts") {
            j.nActs = 0; int start = 0;
            while (start < (int)v.length() && j.nActs < WIZ_MAX_STEPS) {
                int comma = v.indexOf(',', start);
                String tok = (comma < 0) ? v.substring(start) : v.substring(start, comma);
                tok.trim(); if (tok.length()) j.acts[j.nActs++] = (uint8_t)tok.toInt();
                if (comma < 0) break; start = comma + 1;
            }
        }
    }
    f.close();
    return j.nActs > 0;
}

// Видалити журнал конкретного серійника (керування з UI).
static void wizJournalDelete(const char *serialHex) {
    if (!serialHex || !serialHex[0]) return;
    char path[40]; wizJPath(path, sizeof(path), serialHex);
    SPIFFS.remove(path);
    if (g_wizJ.serial[0] && strcmp(g_wizJ.serial, serialHex) == 0) wizJZero();
}

// Завершити/прибрати журнал ПОТОЧНОГО АКБ.
static void wizJournalClear() {
    char cur[17];
    if (g_wizJ.serial[0]) { strncpy(cur, g_wizJ.serial, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0'; }
    else wizSerialHex(cur, sizeof(cur));
    if (cur[0]) { char path[40]; wizJPath(path, sizeof(path), cur); SPIFFS.remove(path); }
    wizJZero();
}

// Завантажити журнал ПОТОЧНОГО АКБ (за його серійником). Інші журнали не чіпає.
static void wizJournalLoad() {
    if (!g_wizJInit) wizJZero();
    g_wizJ.active = false;
    char cur[17]; wizSerialHex(cur, sizeof(cur));
    if (!cur[0]) return;                         // немає DS2438 -> немає журналу
    char path[40]; wizJPath(path, sizeof(path), cur);
    WizJournal j;
    if (wizJournalParse(path, j)) { j.active = true; g_wizJ = j; }
}

// Поточний знімок ключових полів (для порівняння «до/після ЗП»).
static void wizSnapshot(long &cca, long &dca, long &ica, long &health) {
    cca = dca = ica = health = -1;
    if (hasDump2438) {
        cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        ica = batteryDump2438[12];
    }
    int cap = -1, wear = -1;
    if (decodeCapacity(&cap, &wear)) health = cap;
}

// ------------------------------------------------------------------ JSON
// Список усіх збережених журналів відновлення (за серійниками) з описом
// ЗАПЛАНОВАНИХ (ще не виконаних) кроків — для екрана керування пам'яттю в UI.
static String wizJournalListJson() {
    // 1) зібрати імена файлів журналів (без вкладеного відкриття під час обходу)
    String names[WIZ_JRN_MAX]; int n = 0;
    File root = SPIFFS.open("/");
    if (root) {
        File e = root.openNextFile();
        while (e && n < WIZ_JRN_MAX) {
            String nm = e.name();
            int sl = nm.lastIndexOf('/');
            String base = (sl >= 0) ? nm.substring(sl + 1) : nm;
            if (base.startsWith("wj_") && base.endsWith(".jrn")) names[n++] = String("/") + base;
            e = root.openNextFile();
        }
        root.close();
    }
    // 2) розібрати кожен і сформувати JSON
    String o = "{\"ok\":true,\"journals\":[";
    bool first = true;
    for (int i = 0; i < n; i++) {
        WizJournal j;
        if (!wizJournalParse(names[i].c_str(), j)) continue;
        if (!first) o += ","; first = false;
        o += "{\"serial\":\""; jsonEsc(o, j.serial); o += "\"";
        o += ",\"model\":\"";  jsonEsc(o, j.model);  o += "\"";
        o += ",\"done\":" + String(j.done);
        o += ",\"total\":" + String(j.nActs);
        o += ",\"await\":" + String(j.awaitCharge ? "true" : "false");
        o += ",\"planned\":[";
        for (int s = j.done; s < j.nActs; s++) {
            const char *t, *dt; bool ext; wizActionMeta(j.acts[s], &t, &dt, &ext);
            if (s > j.done) o += ",";
            o += "\""; jsonEsc(o, t); o += "\"";
        }
        o += "]}";
    }
    o += "]}";
    return o;
}

// Повний стан Майстра: аналіз + проблеми + план + прогрес.
static String wizStatusJson(const char *resultMsg = nullptr, bool resultOk = true) {
    BatteryDiag d; wizAnalyze(d);
    wizJournalLoad();

    // Операційний план: якщо є активний журнал для цього АКБ — беремо стабільну
    // послідовність з нього; інакше — свіжий план з поточних проблем.
    const uint8_t *acts; int nActs, doneN;
    if (g_wizJ.active) { acts = g_wizJ.acts; nActs = g_wizJ.nActs; doneN = g_wizJ.done; }
    else { wizComputeActions(d); acts = g_wizActs; nActs = g_wizActN; doneN = 0; }

    // Чи виявив Майстер, що калібрування на ЗП відбулось (знімок змінився)?
    bool chargeDone = false;
    if (g_wizJ.active && g_wizJ.awaitCharge) {
        long cca, dca, ica, health; wizSnapshot(cca, dca, ica, health);
        if (cca != g_wizJ.snapCCA || dca != g_wizJ.snapDCA || ica != g_wizJ.snapICA || health != g_wizJ.snapHealth)
            chargeDone = true;
    }

    String o = "{\"ok\":true";
    o += ",\"have33\":" + String(d.have33 ? "true" : "false");
    o += ",\"have38\":" + String(d.have38 ? "true" : "false");
    o += ",\"model\":\""; jsonEsc(o, d.model); o += "\"";
    o += ",\"fmt\":" + String(d.fmt);
    o += ",\"health\":" + String(d.capPct);
    o += ",\"hdrOk\":" + String(d.hdrOk ? "true" : "false");
    o += ",\"mirrorOk\":" + String(d.mirrorOk ? "true" : "false");
    o += ",\"genuine\":" + String(d.genuine ? "true" : "false");
    // Стан навченого калібрувального хвоста 0x18A..0x1FF — ключове поле для
    // ремонту після заміни елементів. blank = рація прийме як «не калібрований».
    o += ",\"tail\":\"" + String(d.tail == IMPRES_TAIL_BLANK ? "blank"
                               : d.tail == IMPRES_TAIL_FRESH ? "fresh"
                               : d.tail == IMPRES_TAIL_VALID ? "learned" : "broken") + "\"";
    o += ",\"healthy\":" + String((d.issues == 0 && !g_wizJ.active) ? "true" : "false");

    o += ",\"problems\":[";
    bool first = true;
    for (int i = 0; i < RECOVERY_RULE_COUNT; i++) {
        if (!(d.issues & RECOVERY_RULES[i].issue)) continue;
        if (!first) o += ","; first = false;
        o += "{\"sev\":" + String(RECOVERY_RULES[i].severity);
        o += ",\"problem\":\""; jsonEsc(o, RECOVERY_RULES[i].problem); o += "\"";
        o += ",\"fix\":\"";     jsonEsc(o, RECOVERY_RULES[i].fix);     o += "\"}";
    }
    o += "]";

    o += ",\"steps\":[";
    for (int i = 0; i < nActs; i++) {
        const char *ttl, *det; bool ext; uint8_t ch;
        wizActionMeta(acts[i], &ttl, &det, &ext, &ch);
        if (i) o += ",";
        o += "{\"idx\":" + String(i);
        o += ",\"action\":\""; o += wizActionId(acts[i]); o += "\"";
        o += ",\"external\":" + String(ext ? "true" : "false");
        o += ",\"chips\":" + String((int)ch);
        o += ",\"chipsText\":\""; o += opChipsText(ch); o += "\"";
        o += ",\"done\":" + String(i < doneN ? "true" : "false");
        o += ",\"title\":\"";  jsonEsc(o, ttl); o += "\"";
        o += ",\"detail\":\""; jsonEsc(o, det); o += "\"}";
    }
    o += "]";
    o += ",\"total\":" + String(nActs);
    o += ",\"progress\":" + String(doneN);
    o += ",\"awaitCharge\":" + String((g_wizJ.active && g_wizJ.awaitCharge) ? "true" : "false");
    o += ",\"chargeDone\":" + String(chargeDone ? "true" : "false");
    o += ",\"needModel\":" + String((d.issues & (ISS_BLANK33 | ISS_NO_MODEL)) ? "true" : "false");
    if (resultMsg) { o += ",\"msg\":\""; jsonEsc(o, resultMsg); o += "\""; o += ",\"result\":" + String(resultOk ? "true" : "false"); }
    o += "}";
    return o;
}

// ------------------------------------------------------------------ ВИКОНАННЯ
// Старт/оновлення: перечитати чіпи й повернути стан.
static String wizStart() {
    bool a, b; readAllChips(a, b);
    return wizStatusJson();
}

// Виконати крок операційного плану idx. model — для ACT_RESTORE, якщо модель
// невідома. Повертає оновлений стан + результат кроку.
// fixes — набір правок під конкретний пакет для кроку «відновити еталон»
// (той самий рядок, що й у /api/restore). Без нього крок бере ТИПОВИЙ набір, і
// галочки, поставлені в картці правок, для Майстра нічого б не значили —
// власник саме на це й наштовхнувся з наробітком.
static String wizExecStep(int idx, const String &model, const String &fixes = String(),
                          long ratedMah = -1, long rsRaw = -1,
                          const String &rsModel = String(), long mfg = -1,
                          int tailMode = 0 /* RTAIL_FRESH */, int health = -1,
                          long useDate = -1, int cal = -1, int cyc = -1, int nonImp = -1,
                          long today = -1, int etmSrc = -1) {
    BatteryDiag d; wizAnalyze(d);
    wizJournalLoad();

    // Зафіксувати план у журналі при ПЕРШОМУ виконанні (робить його стабільним).
    if (!g_wizJ.active) {
        wizComputeActions(d);
        if (g_wizActN == 0) return wizStatusJson("Немає кроків для виконання", false);
        wizJZero();
        wizSerialHex(g_wizJ.serial, sizeof(g_wizJ.serial));
        if (d.model[0]) strncpy(g_wizJ.model, d.model, sizeof(g_wizJ.model) - 1);
        g_wizJ.nActs = g_wizActN;
        for (int i = 0; i < g_wizActN; i++) g_wizJ.acts[i] = g_wizActs[i];
        g_wizJ.done = 0; g_wizJ.active = true;
        wizJournalSave();
    }

    if (idx < 0 || idx >= g_wizJ.nActs) return wizStatusJson("Невірний крок", false);
    uint8_t act = g_wizJ.acts[idx];
    bool ok = true;
    String msg;

    switch (act) {
        case ACT_READ: { bool a, b; ok = readAllChips(a, b); msg = ok ? "Чіпи зчитано" : "Чіпи не знайдено"; } break;
        case ACT_RESTORE: {
            String m = model; m.trim(); m.toUpperCase();
            if (!m.length() && d.model[0]) m = d.model;
            if (!m.length() && g_wizJ.model[0]) m = g_wizJ.model;
            if (!m.length()) { ok = false; msg = "Оберіть модель для відновлення"; break; }
            if (findTemplate(m.c_str()) < 0) { ok = false; msg = "Немає вшитого шаблону моделі"; break; }
            bool o33 = false, o38 = false;
            RestorePlan wp; const RestorePlan *wpp = nullptr;
            if (buildRestorePlanFor(m.c_str(), wp, true)) {
                restorePlanOverride(wp, fixes.length() ? fixes.c_str() : nullptr,
                                    ratedMah, rsRaw, rsModel.c_str(), mfg, health,
                                    useDate, cal, cyc, nonImp, today, etmSrc);
                wpp = &wp;
            }
            ok = performRestoreTemplate(m.c_str(), &o33, &o38, false, wpp, tailMode);
            msg = ok ? (String("Еталон ") + m + " відновлено" + (o38 ? " (DS2433+DS2438)" : " (лише DS2433)"))
                     : "Збій запису еталона";
            if (ok) strncpy(g_wizJ.model, m.c_str(), sizeof(g_wizJ.model) - 1);
        } break;
        case ACT_REPAIR:        ok = performRepair();        msg = ok ? "Цілісність відновлено" : "Помилка ремонту"; break;
        case ACT_HDRFIX:        { String hn; ok = performHeaderComplete(&hn); msg = hn.length() ? hn : (ok ? "Заголовок добудовано" : "Помилка добудови"); } break;
        case ACT_RECAL:         ok = performRecalPrepare(false);
                                msg = ok ? "Навчений хвіст стерто, лічильники скинуто — готово до калібрування на ЗП"
                                         : "Помилка підготовки"; break;
        case ACT_RECAL_DEEP:    ok = performRecalPrepare(true);
                                msg = ok ? "Глибока підготовка виконана — готово до калібрування на ЗП"
                                         : "Помилка підготовки"; break;
        case ACT_RESET:         ok = performReset();         msg = ok ? "Лічильники скинуто" : "Помилка скидання"; break;
        case ACT_DCAFIX:        { String dm; ok = performDcaFix(&dm); msg = dm; } break;
        case ACT_CLEAN:         ok = performFactoryClean();  msg = ok ? "Історію очищено" : "Помилка очистки"; break;
        case ACT_SETCHARGE_AUTO:{ int p = chargePctFromVoltage(); ok = (p >= 0) && performSetChargePct(p);
                                  msg = ok ? (String("Заряд ~") + p + "% з напруги") : "Помилка (немає напруги?)"; } break;
        case ACT_CHARGE_STATION: {
            // Зовнішній крок: зберегти знімок і чекати повернення з ЗП.
            wizSnapshot(g_wizJ.snapCCA, g_wizJ.snapDCA, g_wizJ.snapICA, g_wizJ.snapHealth);
            g_wizJ.awaitCharge = true;
            if (idx + 1 > g_wizJ.done) g_wizJ.done = idx + 1;
            wizJournalSave();
            displayShow("НА ЗАРЯДНУ СТ.");
            return wizStatusJson("Поставте АКБ на IMPRES-ЗП. Майстер продовжить після повернення.", true);
        }
        // Перешифрування під ROM цього чипа. Робимо ТИМ САМИМ кодом, що й правка
        // «crypt» у плані «Ремонту»: друга реалізація одного дня розійшлася б.
        case ACT_CRYPT: {
            if (!hasDump)   { ok = false; msg = "Спочатку зчитайте DS2433"; break; }
            if (!hasSN2433) { ok = false; msg = "ROM-ID чипа невідомий — ключ узяти нізвідки"; break; }
            char cm[16] = "";
            if (!decodeModel(cm, sizeof(cm)) || !cm[0]) {
                if (g_wizJ.model[0]) strncpy(cm, g_wizJ.model, sizeof(cm) - 1);
            }
            RestorePlan cp;
            if (!cm[0] || !buildRestorePlanFor(cm, cp, /*refresh=*/false)) {
                ok = false; msg = "Немає вшитого еталона моделі — перешифрувати нема від чого";
                break;
            }
            if (!cp.fx[RPF_CRYPT].avail) {
                // Ключ уже свій і поля несуперечливі — крок не потрібен.
                ok = true; msg = "Перешифрування не потрібне: ключ уже свій";
                break;
            }
            cp.fx[RPF_CRYPT].on = true;
            restorePlanApply(cp, batteryDump, nullptr, /*onlyEnabled=*/true);
            ledSet(LED_WRITE); displayShow("ПЕРЕШИФРУВАННЯ");
            ok = battery.writeBattery(batteryDump, DUMP_SIZE);
            if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
            displayShow(ok ? "ШИФР OK" : "ШИФР ЗБІЙ");
            ledSet(ok ? LED_OK : LED_ERROR);
            msg = ok ? "Дати й лічильники перешифровано під ROM цього чипа"
                     : "Збій запису DS2433";
        } break;
        case ACT_VERIFY: {
            bool a, b; readAllChips(a, b);
            BatteryDiag d2; wizAnalyze(d2);
            ok = (d2.issues == 0);
            msg = ok ? "Відновлення завершено: помилок не виявлено" : "Лишились проблеми — див. аналіз";
            wizJournalClear();      // сеанс завершено (успіх або треба почати заново)
            return wizStatusJson(msg.c_str(), ok);
        }
        default: ok = false; msg = "Дія недоступна"; break;
    }

    if (ok) {
        g_wizJ.awaitCharge = false;
        if (idx + 1 > g_wizJ.done) g_wizJ.done = idx + 1;
        if (g_wizJ.done >= g_wizJ.nActs) wizJournalClear();
        else wizJournalSave();
    }
    return wizStatusJson(msg.c_str(), ok);
}

// ------------------------------------------------------------------ ЕКРАННИЙ МАЙСТЕР
// Заповнює глобали дисплея (g_wiz*, оголошені в display.h/display_color.h) з
// поточного аналізу й плану. Викликається з .ino після зчитування чіпів.
inline void wizDeviceRefresh() {
    BatteryDiag d; wizAnalyze(d);
    wizJournalLoad();
    const uint8_t *acts; int nActs, doneN;
    if (g_wizJ.active) { acts = g_wizJ.acts; nActs = g_wizJ.nActs; doneN = g_wizJ.done; }
    else { wizComputeActions(d); acts = g_wizActs; nActs = g_wizActN; doneN = 0; }

    int pc = 0; g_wizTop[0] = '\0';
    for (int i = 0; i < RECOVERY_RULE_COUNT; i++) {
        if (!(d.issues & RECOVERY_RULES[i].issue)) continue;
        if (pc == 0) { strncpy(g_wizTop, RECOVERY_RULES[i].problem, sizeof(g_wizTop) - 1); g_wizTop[sizeof(g_wizTop) - 1] = '\0'; }
        pc++;
    }
    g_wizProblems = pc;
    g_wizHealthy  = (d.issues == 0 && !g_wizJ.active);
    g_wizTotal    = nActs;
    g_wizProg     = doneN;
    g_wizAwait    = (g_wizJ.active && g_wizJ.awaitCharge);
    g_wizNext[0]  = '\0';
    if (doneN < nActs) {
        const char *t, *dt; bool ext; wizActionMeta(acts[doneN], &t, &dt, &ext);
        strncpy(g_wizNext, t, sizeof(g_wizNext) - 1); g_wizNext[sizeof(g_wizNext) - 1] = '\0';
    }
    g_wizBusy = false;
}

// Виконує наступний крок плану (для екранного Майстра). Модель для відновлення
// бере з поточного аналізу (детекована). Оновлює глобали дисплея.
inline void wizDeviceRunNext() {
    BatteryDiag d; wizAnalyze(d);
    wizJournalLoad();
    int idx = g_wizJ.active ? g_wizJ.done : 0;
    String model = d.model[0] ? String(d.model) : String("");
    wizExecStep(idx, model);          // виконує + оновлює журнал (JSON ігноруємо)
    wizDeviceRefresh();
}

#endif // RECOVERY_H
