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

// ---- Проблеми, які виявляє аналіз (бітова маска) --------------------------
enum {
    ISS_NO_CHIP      = 1u << 0,  // жоден чіп не відповідає на шині
    ISS_BLANK33      = 1u << 1,  // DS2433 порожній/стертий (усе 0xFF)
    ISS_HDR_BAD      = 1u << 2,  // заголовок DS2433 Σ≠0x41
    ISS_NO_MODEL     = 1u << 3,  // немає запису моделі 0x0B
    ISS_MIRROR_BAD   = 1u << 4,  // дзеркало DS2438<->DS2433 розійшлось
    ISS_CCA_OVERFLOW = 1u << 5,  // CCA=0xFFFF (лічильник «залочено»)
    ISS_NO_2438      = 1u << 6,  // DS2438 відсутній/не читається
    ISS_NEEDS_CALIB  = 1u << 7,  // структурно валідна, але потрібне калібрування
    ISS_HEALTH_LOW   = 1u << 8,  // здоров'я/ємність нижче порогу
};

// ---- Дії Майстра ----------------------------------------------------------
enum {
    ACT_NONE = 0,
    ACT_READ,            // перечитати чіпи
    ACT_RESTORE,         // відновити еталон verbatim (потребує моделі)
    ACT_REPAIR,          // ремонт цілісності (суми + дзеркало)
    ACT_RECAL,           // підготовка до калібрування (після заміни банок)
    ACT_RESET,           // скидання лічильників
    ACT_CLEAN,           // очистка історії
    ACT_SETCHARGE_AUTO,  // виставити заряд із напруги
    ACT_CHARGE_STATION,  // ЗОВНІШНІЙ крок: калібрування на IMPRES-ЗП
    ACT_VERIFY,          // фінальна перевірка
};

// ---- База правил: проблема -> діагноз/пропозиція/дія -----------------------
struct RecoveryRule {
    uint32_t    issue;      // біт проблеми
    uint8_t     severity;   // 0=інфо, 1=увага, 2=критично
    const char *problem;    // що не так (людською)
    const char *fix;        // що пропонуємо
    uint8_t     action;     // рекомендована дія
};

static const RecoveryRule RECOVERY_RULES[] = {
    { ISS_NO_CHIP,      2, "Жоден чіп не відповідає на 1-Wire шині",        "Перевірте контакти АКБ і повторіть зчитування",           ACT_READ },
    { ISS_BLANK33,      2, "DS2433 порожній (стертий у 0xFF)",              "Відновити еталон обраної моделі байт-у-байт",              ACT_RESTORE },
    { ISS_NO_MODEL,     2, "Відсутній запис моделі (0x0B)",                 "Відновити еталон обраної моделі (модель+калібрування)",    ACT_RESTORE },
    { ISS_HDR_BAD,      2, "Пошкоджений заголовок DS2433 (Σ≠0x41)",         "Ремонт цілісності (перерахунок суми заголовка)",           ACT_REPAIR },
    { ISS_MIRROR_BAD,   1, "Дзеркало калібрування DS2438<->DS2433 розійшлось","Ремонт цілісності (синхронізувати дзеркало)",            ACT_REPAIR },
    { ISS_CCA_OVERFLOW, 1, "Лічильник заряду CCA переповнений (залочено)",  "Підготовка + калібрування на IMPRES-ЗП",                   ACT_RECAL },
    { ISS_NEEDS_CALIB,  1, "Потрібне калібрування ємності",                 "Підготовка + калібрування на IMPRES-ЗП",                   ACT_RECAL },
    { ISS_HEALTH_LOW,   1, "Низьке показане здоров'я/ємність",              "Скидання зносу до 100% або калібрування на ЗП",            ACT_RESET },
    { ISS_NO_2438,      1, "DS2438 (монітор) відсутній/не читається",       "Перевірте чіп; відновлення DS2433 можливе окремо",         ACT_NONE },
};
static const int RECOVERY_RULE_COUNT = sizeof(RECOVERY_RULES) / sizeof(RECOVERY_RULES[0]);

// ---- Стан аналізу ---------------------------------------------------------
struct BatteryDiag {
    uint32_t issues;
    int      capPct;        // здоров'я/ємність, % (-1 якщо невідомо)
    char     model[16];
    int      fmt;           // 0 невідомий / 2014 / 2017 / 2021
    bool     have33, have38;
    bool     hdrOk, mirrorOk, genuine;
};

// ---- Крок плану -----------------------------------------------------------
struct WizStep {
    uint8_t action;
    bool    external;       // виконується на ЗП (потрібна пауза)
    char    title[36];
    char    detail[88];
};

#define WIZ_MAX_STEPS 8
static WizStep g_wizPlan[WIZ_MAX_STEPS];
static int     g_wizPlanLen = 0;

// Журнал продовження (у пам'яті ESP32, прив'язаний до серійника DS2438).
struct WizJournal {
    bool     active;
    char     serial[17];    // hex ROM DS2438
    char     model[16];
    int      total;
    int      done;
    bool     awaitCharge;
    long     snapCCA, snapDCA, snapICA, snapHealth;
};
static WizJournal g_wizJ = { false, "", "", 0, 0, false, 0, 0, 0, 0 };

// ------------------------------------------------------------------ утиліти
static void wizSerialHex(char *out, size_t n) {
    out[0] = '\0';
    if (!battery.hasRom2438()) return;
    const uint8_t *r = battery.rom2438();
    for (int i = 0; i < 8 && (size_t)(i * 2 + 1) < n; i++) sprintf(out + i * 2, "%02X", r[i]);
}

static int wizDetectFormat() {
    if (!hasDump) return 0;
    auto has = [](const char *s, int l) {
        for (int i = 0; i + l <= (int)DUMP_SIZE; i++) {
            int k = 0; while (k < l && batteryDump[i + k] == (uint8_t)s[k]) k++;
            if (k == l) return true;
        }
        return false;
    };
    if (has("COPYRIGHT2021", 13)) return 2021;
    if (has("COPYRIGHT2014", 13)) return 2014;
    if (has("MOTOROLA", 8))       return 2017;
    return 0;
}

// ------------------------------------------------------------------ АНАЛІЗ
static void wizAnalyze(BatteryDiag &d) {
    memset(&d, 0, sizeof(d));
    d.capPct = -1;
    d.have33 = hasDump;
    d.have38 = hasDump2438;

    if (!hasDump && !hasDump2438) { d.issues |= ISS_NO_CHIP; return; }
    if (!hasDump2438)             d.issues |= ISS_NO_2438;

    if (hasDump) {
        // Порожній/стертий DS2433: перші 0x20 байт усе 0xFF.
        bool blank = true;
        for (int i = 0; i < 0x20; i++) if (batteryDump[i] != 0xFF) { blank = false; break; }
        if (blank) d.issues |= ISS_BLANK33;

        d.hdrOk = headerChecksumOk(batteryDump);
        if (!d.hdrOk && !blank) d.issues |= ISS_HDR_BAD;

        if (decodeModel(d.model, sizeof(d.model))) { /* модель є */ }
        else { d.issues |= ISS_NO_MODEL; d.model[0] = '\0'; }

        d.fmt = wizDetectFormat();

        int cap = -1, wear = -1;
        if (decodeCapacity(&cap, &wear)) { d.capPct = cap; if (cap < 40) d.issues |= ISS_HEALTH_LOW; }
    }

    if (hasDump && hasDump2438) {
        d.mirrorOk = mirrorOk(batteryDump, batteryDump2438);
        if (!d.mirrorOk) d.issues |= ISS_MIRROR_BAD;
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        if (cca == 0xFFFF) d.issues |= ISS_CCA_OVERFLOW;
    }

    const char *reason = "";
    d.genuine = batteryGenuine(&reason);

    // Евристика «потрібне калібрування»: структурно валідна (заголовок+модель),
    // але лічильник переповнений або паливомір порожній / здоров'я низьке.
    bool structOk = d.hdrOk && !(d.issues & (ISS_BLANK33 | ISS_NO_MODEL));
    if (structOk && hasDump2438) {
        uint8_t ica = batteryDump2438[12];
        if ((d.issues & ISS_CCA_OVERFLOW) || ica == 0 || (d.issues & ISS_HEALTH_LOW))
            d.issues |= ISS_NEEDS_CALIB;
    }
}

// ------------------------------------------------------------------ ПЛАН
static void wizAddStep(uint8_t action, bool external, const char *title, const char *detail) {
    if (g_wizPlanLen >= WIZ_MAX_STEPS) return;
    WizStep &s = g_wizPlan[g_wizPlanLen++];
    s.action = action; s.external = external;
    strncpy(s.title, title, sizeof(s.title) - 1);  s.title[sizeof(s.title) - 1] = '\0';
    strncpy(s.detail, detail, sizeof(s.detail) - 1); s.detail[sizeof(s.detail) - 1] = '\0';
}

// Будує впорядкований план із набору проблем. Детермінований (той самий вхід ->
// той самий план), тож журнал продовження завжди узгоджений після паузи.
static void wizBuildPlan(const BatteryDiag &d) {
    g_wizPlanLen = 0;
    uint32_t is = d.issues;

    if (is & ISS_NO_CHIP) {
        wizAddStep(ACT_READ, false, "Зчитати чіпи", "Перевірте контакти АКБ і повторіть зчитування 1-Wire.");
        return;
    }

    bool restored = false;
    if (is & (ISS_BLANK33 | ISS_NO_MODEL)) {
        wizAddStep(ACT_RESTORE, false, "Відновити еталон",
                   "Записати genuine-еталон обраної моделі байт-у-байт (модель+калібрування). Працює й на порожньому чипі.");
        restored = true;
    } else if (is & (ISS_HDR_BAD | ISS_MIRROR_BAD)) {
        wizAddStep(ACT_REPAIR, false, "Ремонт цілісності",
                   "Перерахувати контрольні суми (заголовок Σ≡0x41) і синхронізувати дзеркало калібрування.");
    }

    if (is & (ISS_CCA_OVERFLOW | ISS_NEEDS_CALIB)) {
        wizAddStep(ACT_RECAL, false, "Підготовка до калібрування",
                   "Стерти learned-калібрування, обнулити лічильники, паливомір -> 0. Пакет стане «валідний, не відкалібрований».");
        wizAddStep(ACT_CHARGE_STATION, true, "Калібрування на ЗП",
                   "Поставте АКБ на IMPRES-зарядну станцію на повний цикл (заряд/розряд/заряд). Майстер продовжить після повернення.");
    }

    (void)restored;
    wizAddStep(ACT_VERIFY, false, "Перевірка результату",
               "Перечитати чіпи й підтвердити цілісність (заголовок, модель, дзеркало, лічильники).");
}

// ------------------------------------------------------------------ ЖУРНАЛ
static const char *WIZ_JOURNAL_PATH = "/wizard.jrn";

static void wizJournalSave() {
    File f = SPIFFS.open(WIZ_JOURNAL_PATH, "w");
    if (!f) return;
    f.printf("serial=%s\nmodel=%s\ntotal=%d\ndone=%d\nawait=%d\ncca=%ld\ndca=%ld\nica=%ld\nhealth=%ld\n",
             g_wizJ.serial, g_wizJ.model, g_wizJ.total, g_wizJ.done,
             g_wizJ.awaitCharge ? 1 : 0, g_wizJ.snapCCA, g_wizJ.snapDCA, g_wizJ.snapICA, g_wizJ.snapHealth);
    f.close();
}

static void wizJournalClear() {
    g_wizJ.active = false; g_wizJ.awaitCharge = false; g_wizJ.done = 0;
    g_wizJ.serial[0] = '\0'; g_wizJ.model[0] = '\0';
    SPIFFS.remove(WIZ_JOURNAL_PATH);
}

// Завантажити журнал, ЯКЩО він для поточного АКБ (збіг серійника).
static void wizJournalLoad() {
    g_wizJ.active = false;
    if (!SPIFFS.exists(WIZ_JOURNAL_PATH)) return;
    File f = SPIFFS.open(WIZ_JOURNAL_PATH, "r");
    if (!f) return;
    WizJournal j = { false, "", "", 0, 0, false, 0, 0, 0, 0 };
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq), v = line.substring(eq + 1);
        if      (k == "serial") strncpy(j.serial, v.c_str(), sizeof(j.serial) - 1);
        else if (k == "model")  strncpy(j.model,  v.c_str(), sizeof(j.model) - 1);
        else if (k == "total")  j.total = v.toInt();
        else if (k == "done")   j.done = v.toInt();
        else if (k == "await")  j.awaitCharge = v.toInt() != 0;
        else if (k == "cca")    j.snapCCA = v.toInt();
        else if (k == "dca")    j.snapDCA = v.toInt();
        else if (k == "ica")    j.snapICA = v.toInt();
        else if (k == "health") j.snapHealth = v.toInt();
    }
    f.close();

    char cur[17]; wizSerialHex(cur, sizeof(cur));
    if (cur[0] && strcmp(cur, j.serial) == 0) { j.active = true; g_wizJ = j; }
    else wizJournalClear();      // журнал від іншого АКБ — прибрати
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
static void jsonEsc(String &o, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
        else o += *p;
    }
}

static const char *wizActionName(uint8_t a) {
    switch (a) {
        case ACT_READ: return "read";           case ACT_RESTORE: return "restore";
        case ACT_REPAIR: return "repair";       case ACT_RECAL: return "recal";
        case ACT_RESET: return "reset";         case ACT_CLEAN: return "clean";
        case ACT_SETCHARGE_AUTO: return "charge";case ACT_CHARGE_STATION: return "station";
        case ACT_VERIFY: return "verify";       default: return "none";
    }
}

// Повний стан Майстра: аналіз + проблеми + план + прогрес.
static String wizStatusJson(const char *resultMsg = nullptr, bool resultOk = true) {
    BatteryDiag d; wizAnalyze(d);
    wizBuildPlan(d);
    wizJournalLoad();

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
    o += ",\"healthy\":" + String(d.issues == 0 ? "true" : "false");

    // Проблеми (з бази правил).
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

    // План (кроки).
    o += ",\"steps\":[";
    int doneN = (g_wizJ.active ? g_wizJ.done : 0);
    for (int i = 0; i < g_wizPlanLen; i++) {
        if (i) o += ",";
        o += "{\"idx\":" + String(i);
        o += ",\"action\":\""; o += wizActionName(g_wizPlan[i].action); o += "\"";
        o += ",\"external\":" + String(g_wizPlan[i].external ? "true" : "false");
        o += ",\"done\":" + String(i < doneN ? "true" : "false");
        o += ",\"title\":\"";  jsonEsc(o, g_wizPlan[i].title);  o += "\"";
        o += ",\"detail\":\""; jsonEsc(o, g_wizPlan[i].detail); o += "\"}";
    }
    o += "]";
    o += ",\"total\":" + String(g_wizPlanLen);
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

// Виконати крок плану idx. model — обов'язковий лише для ACT_RESTORE, якщо
// модель невідома. Повертає оновлений стан + результат кроку.
static String wizExecStep(int idx, const String &model) {
    BatteryDiag d; wizAnalyze(d);
    wizBuildPlan(d);
    if (idx < 0 || idx >= g_wizPlanLen)
        return wizStatusJson("Невірний крок", false);

    wizJournalLoad();
    uint8_t act = g_wizPlan[idx].action;
    bool ok = true;
    String msg;

    switch (act) {
        case ACT_READ: { bool a, b; ok = readAllChips(a, b); msg = ok ? "Чіпи зчитано" : "Чіпи не знайдено"; } break;
        case ACT_RESTORE: {
            String m = model; m.trim(); m.toUpperCase();
            if (!m.length() && d.model[0]) m = d.model;
            if (!m.length()) { ok = false; msg = "Оберіть модель для відновлення"; break; }
            if (findTemplate(m.c_str()) < 0) { ok = false; msg = "Немає вшитого шаблону моделі"; break; }
            bool o33 = false, o38 = false;
            ok = performRestoreTemplate(m.c_str(), &o33, &o38);
            msg = ok ? (String("Еталон ") + m + " відновлено" + (o38 ? " (DS2433+DS2438)" : " (лише DS2433)"))
                     : "Збій запису еталона";
            if (ok) strncpy(g_wizJ.model, m.c_str(), sizeof(g_wizJ.model) - 1);
        } break;
        case ACT_REPAIR:        ok = performRepair();        msg = ok ? "Цілісність відновлено" : "Помилка ремонту"; break;
        case ACT_RECAL:         ok = performRecalPrepare();  msg = ok ? "Готово до калібрування на ЗП" : "Помилка підготовки"; break;
        case ACT_RESET:         ok = performReset();         msg = ok ? "Лічильники скинуто" : "Помилка скидання"; break;
        case ACT_CLEAN:         ok = performFactoryClean();  msg = ok ? "Історію очищено" : "Помилка очистки"; break;
        case ACT_SETCHARGE_AUTO:{ int p = chargePctFromVoltage(); ok = (p >= 0) && performSetChargePct(p);
                                  msg = ok ? (String("Заряд ~") + p + "% з напруги") : "Помилка (немає напруги?)"; } break;
        case ACT_CHARGE_STATION: {
            // Зовнішній крок: зберегти знімок і чекати повернення з ЗП.
            wizSerialHex(g_wizJ.serial, sizeof(g_wizJ.serial));
            if (d.model[0]) strncpy(g_wizJ.model, d.model, sizeof(g_wizJ.model) - 1);
            wizSnapshot(g_wizJ.snapCCA, g_wizJ.snapDCA, g_wizJ.snapICA, g_wizJ.snapHealth);
            g_wizJ.active = true; g_wizJ.awaitCharge = true;
            g_wizJ.total = g_wizPlanLen; g_wizJ.done = idx + 1;
            wizJournalSave();
            displayShow("НА ЗАРЯДНУ СТ.");
            return wizStatusJson("Поставте АКБ на IMPRES-ЗП. Майстер продовжить після повернення.", true);
        }
        case ACT_VERIFY: {
            bool a, b; readAllChips(a, b);
            BatteryDiag d2; wizAnalyze(d2);
            ok = (d2.issues == 0);
            msg = ok ? "Відновлення завершено: помилок не виявлено" : "Лишились проблеми — див. аналіз";
            if (ok) wizJournalClear();
        } break;
        default: ok = false; msg = "Дія недоступна"; break;
    }

    // Оновити прогрес журналу (окрім зовнішнього кроку, який вийшов вище).
    if (ok) {
        wizSerialHex(g_wizJ.serial, sizeof(g_wizJ.serial));
        if (d.model[0] && !g_wizJ.model[0]) strncpy(g_wizJ.model, d.model, sizeof(g_wizJ.model) - 1);
        g_wizJ.total = g_wizPlanLen;
        if (idx + 1 > g_wizJ.done) g_wizJ.done = idx + 1;
        g_wizJ.awaitCharge = false;
        g_wizJ.active = (g_wizJ.done < g_wizPlanLen);
        if (g_wizJ.active) wizJournalSave(); else wizJournalClear();
    }
    return wizStatusJson(msg.c_str(), ok);
}

#endif // RECOVERY_H
