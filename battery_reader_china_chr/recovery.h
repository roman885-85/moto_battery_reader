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
    ISS_TAIL_FOREIGN = 1u << 9,  // ⚑ навчена калібровка ЧУЖОГО пакета (див. нижче)
    ISS_TAIL_ERASED  = 1u << 10, // ⛔ хвіст стерто у 0xFF — ЗП не зможе відкалібрувати
    ISS_NEVER_CALIB  = 1u << 11, // пакет жодного разу не калібрувався (CTS = 0)
    // ── шифрування й узгодженість даних ────────────────────────────────────
    //  Досі Майстер дивився лише на СТРУКТУРУ: суми, модель, дзеркало, хвіст.
    //  Але найчастіша скарга («невідомий акумулятор») народжується не з
    //  поламаної структури, а зі ЗМІСТУ: поля зашифровані ключем чужого чипа,
    //  дати суперечать одна одній, монітор від іншого пакета. Структурно такий
    //  дамп бездоганний — і Майстер мовчав.
    ISS_CRYPT_WRONG  = 1u << 12, // вміст зашифровано ЧУЖИМ ключем (рація бачить сміття)
    ISS_CRYPT_UNKNOWN= 1u << 13, // ключ вмісту не визначається взагалі
    ISS_BLOCK_SUM    = 1u << 14, // побита сума одного з блоків BMS
    ISS_DATE_INSANE  = 1u << 15, // дата виготовлення неправдоподібна
    ISS_ETM_FOREIGN  = 1u << 16, // наробіток більший за вік пакета -> чужий DS2438
    ISS_USE_BEFORE_CHG = 1u << 17,// пакет «вмикали», але жодного разу не заряджали
    // ⚑ Одномісний зарядний WPLN4226A сам дописує в стертий DS2433 дзеркало
    // заголовка з DS2438 (26 байт, 0x01..0x1A), але НЕ виправляє контрольну
    // суму — і на цьому зупиняється: профіль, модель і блоки лишаються 0xFF.
    // Структурно це «побитий заголовок», але дані в ньому вже правильні,
    // тому лікується не переписуванням, а добудовою (див. ACT_HDRFIX).
    ISS_CHARGER_PARTIAL = 1u << 18,
    ISS_DCA_INSANE   = 1u << 19, // регістр розряду монітора побитий (DCA >> CCA)
};

// ---- Дії Майстра ----------------------------------------------------------
enum {
    ACT_NONE = 0,
    ACT_READ,            // перечитати чіпи
    ACT_RESTORE,         // відновити модельну частину еталона (потребує моделі)
    ACT_REPAIR,          // ремонт цілісності (суми + дзеркало)
    ACT_RECAL,           // підготовка до калібрування (після заміни банок)
    ACT_RECAL_DEEP,      // те саме + стерти навчені записи ємності й журнал
    ACT_RESET,           // скидання лічильників
    ACT_CLEAN,           // очистка історії
    ACT_SETCHARGE_AUTO,  // виставити заряд із напруги
    ACT_CHARGE_STATION,  // ЗОВНІШНІЙ крок: калібрування на IMPRES-ЗП
    ACT_HDRFIX,          // добудувати заголовок DS2433 із дзеркала DS2438
    ACT_CRYPT,           // перешифрувати дати й лічильники під ROM цього чипа
    ACT_VERIFY,          // фінальна перевірка
};

// ---- База правил: проблема -> діагноз/пропозиція/дія -----------------------
struct RecoveryRule {
    uint32_t    issue;      // біт проблеми
    uint8_t     severity;   // 0=інфо, 1=увага, 2=критично
    const char *problem;    // що не так (людською)
    const char *fix;        // що пропонуємо
};

static const RecoveryRule RECOVERY_RULES[] = {
    { ISS_NO_CHIP,      2, "Жоден чіп не відповідає на 1-Wire шині",          "Перевірте контакти АКБ і повторіть зчитування" },
    { ISS_BLANK33,      2, "DS2433 порожній (стертий у 0xFF)",                "Відновити еталон обраної моделі байт-у-байт" },
    { ISS_NO_MODEL,     2, "Відсутній запис моделі (0x0B)",                   "Відновити еталон обраної моделі (модель+калібрування)" },
    { ISS_HDR_BAD,      2, "Пошкоджений заголовок DS2433 (Σ≠0x41)",           "Ремонт цілісності (перерахунок суми заголовка)" },
    { ISS_MIRROR_BAD,   1, "Дзеркало калібрування DS2438<->DS2433 розійшлось","Ремонт цілісності (синхронізувати дзеркало)" },
    { ISS_TAIL_FOREIGN, 2, "Навчена калібровка НЕ від цього пакета (0x18A..0x1FF)",
                           "Ремонт після заміни елементів + калібрування на ЗП" },
    { ISS_TAIL_ERASED,  2, "Хвіст 0x18A..0x1FF стерто — ЗП не зможе завершити калібрування",
                           "Ремонт після заміни елементів: відновити скелет записів (чистий хвіст)" },
    { ISS_CCA_OVERFLOW, 1, "Лічильник заряду CCA переповнений (залочено)",    "Підготовка + калібрування на IMPRES-ЗП" },
    { ISS_NEEDS_CALIB,  1, "Потрібне калібрування ємності",                   "Підготовка + калібрування на IMPRES-ЗП" },
    { ISS_HEALTH_LOW,   1, "Низьке показане здоров'я/ємність",                "Скидання зносу або калібрування на ЗП" },
    { ISS_NO_2438,      1, "DS2438 (монітор) відсутній/не читається",         "Перевірте чіп; відновлення DS2433 можливе окремо" },
    { ISS_NEVER_CALIB,  1, "Пакет жодного разу не калібрувався (потенційна ємність = 0)",
                           "Повний цикл на IMPRES-ЗП — саме він виміряє банки" },
    { ISS_CRYPT_WRONG,  2, "Дати й лічильники зашифровані ЧУЖИМ ключем — рація читає сміття",
                           "Перешифрувати під ROM цього чипа (числа ті самі, читати їх зможе рація)" },
    { ISS_CRYPT_UNKNOWN,2, "Ключ зашифрованих полів не визначається — вміст ні з чим не узгоджений",
                           "Вписати дату виготовлення й знос вручну; далі перешифрувати під ROM чипа" },
    { ISS_BLOCK_SUM,    2, "Побита контрольна сума блока даних BMS",
                           "Ремонт цілісності (перерахунок сум блоків)" },
    { ISS_DATE_INSANE,  1, "Неправдоподібна дата виготовлення",
                           "Вписати дату вручну в «Ремонті» — вона піде під ключем цього чипа" },
    { ISS_DCA_INSANE,   2, "Лічильник РОЗРЯДУ монітора побитий: «взято» в рази більше, ніж залито",
                           "Синхронізація дзеркала: узяти DCA з історії DS2433 і записати в монітор" },
    { ISS_ETM_FOREIGN,  2, "Наробіток більший за вік пакета — DS2438 не від цього АКБ",
                           "Перечитайте монітор; якщо пакет щойно був на ЗП — правте наробіток ПІСЛЯ калібрування" },
    { ISS_USE_BEFORE_CHG, 1, "Пакет позначено як увімкнений, але жодного разу не заряджений",
                           "Перешифрувати під ROM цього чипа — запис приведе поля до несуперечливого стану" },
    { ISS_CHARGER_PARTIAL, 2, "Станція частково записала заголовок (дзеркало є, сума й решта чипа — ні)",
                           "Добудувати заголовок із дзеркала; якщо модель невідома — режим копії" },
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
    int      tail;          // IMPRES_TAIL_BLANK / _VALID / _BROKEN
};

// ---- Метадані дії (заголовок/опис/зовнішній/куди пише) --------------------
//  chips — у яку мікросхему піде запис (OPC_* із operations.h). Кроки Майстра —
//  це теж пункти запису, і плутанина між DS2433 (ідентичність) і DS2438
//  (монітор із заводським калібруванням вимірювача струму) коштує дорого, тож
//  кожен крок каже це прямо, як і решта операцій.
static void wizActionMeta(uint8_t a, const char **title, const char **detail,
                          bool *external, uint8_t *chips = nullptr) {
    *external = false;
    if (chips) {
        switch (a) {
            case ACT_RESTORE:        *chips = OPC_33;   break;  // + DS2438, якщо є еталон
            case ACT_REPAIR:
            case ACT_RECAL:
            case ACT_RECAL_DEEP:
            case ACT_CLEAN:          *chips = OPC_BOTH; break;
            case ACT_HDRFIX:
            case ACT_CRYPT:          *chips = OPC_33;   break;
            case ACT_RESET:
            case ACT_SETCHARGE_AUTO: *chips = OPC_38;   break;
            default:                 *chips = OPC_NONE; break;  // читання/зовнішні
        }
    }
    switch (a) {
        case ACT_READ:    *title = "Зчитати чіпи";
                          *detail = "Перевірте контакти АКБ і повторіть зчитування 1-Wire."; break;
        case ACT_RESTORE: *title = "Відновити еталон";
                          *detail = "Записати модельну частину еталона (ідентичність, крива, copyright, заводська таблиця, модель). Навчений калібрувальний хвіст НЕ переноситься — інакше пакет отримав би чужу калібровку. Працює й на порожньому чипі."; break;
        case ACT_REPAIR:  *title = "Ремонт цілісності";
                          *detail = "Перерахувати контрольні суми (заголовок Σ≡0x41) і синхронізувати дзеркало калібрування."; break;
        case ACT_RECAL:   *title = "Ремонт після заміни елементів";
                          *detail = "Записати ЧИСТИЙ калібрувальний хвіст DS2433 (0x18A..0x1FF): скелет записів і сталі моделі лишаються, навчені значення обнулені, суми правильні. Обнулити лічильники DS2438, паливомір — за напругою. НЕ стирає хвіст у 0xFF: на стертому хвості ЗП не може записати навчену калібровку і цикл падає в помилку."; break;
        case ACT_RECAL_DEEP: *title = "Глибока підготовка";
                          *detail = "Те саме + стерти навчені записи ємності (0x153..0x189) і журнал використання. Застосовувати, коли після звичайної підготовки ЗП тримається за стару ємність."; break;
        case ACT_RESET:   *title = "Скидання лічильників";
                          *detail = "Обнулити лічильники DS2438 (ETM/CCA/DCA). Навчену калібровку й ідентичність не чіпає."; break;
        case ACT_CLEAN:   *title = "Очистка історії";
                          *detail = "Стерти історію/статистику, лишити ідентичність і калібрування."; break;
        case ACT_SETCHARGE_AUTO: *title = "Заряд із напруги";
                          *detail = "Виставити паливомір за поточною напругою (" BATTERY_SCALE_TXT ")."; break;
        case ACT_HDRFIX:  *title = "Добудувати заголовок";
                          *detail = "Зарядна станція сама дописала в стертий чип дзеркало заголовка з "
                                    "DS2438, але не виправила суму — і на цьому зупинилась. Крок копіює "
                                    "ті самі 26 байт (якщо ще не скопійовані) і виправляє контрольну "
                                    "суму. Профіль, модель і блоки цим НЕ відновлюються — далі потрібен "
                                    "«Відновити еталон» (якщо модель відома) або режим копії."; break;
        case ACT_CRYPT:   *title = "Перешифрувати під ROM чипа";
                          *detail = "Дати й лічильники зашифровані ключем із ROM-ID самого чипа. "
                                    "Якщо вміст прийшов від іншого пакета, рація розшифрує його "
                                    "своїм ключем і побачить сміття. Крок переписує ТІ САМІ числа "
                                    "під ключем цього чипа — значення не міняються, міняється лише "
                                    "те, хто здатен їх прочитати."; break;
        case ACT_CHARGE_STATION: *title = "Калібрування на ЗП"; *external = true;
                          *detail = "Поставте АКБ на IMPRES-зарядну станцію на повний цикл (заряд/розряд/заряд). Майстер продовжить після повернення."; break;
        case ACT_VERIFY:  *title = "Перевірка результату";
                          *detail = "Перечитати чіпи й підтвердити цілісність (заголовок, модель, дзеркало, лічильники)."; break;
        default:          *title = "—"; *detail = ""; break;
    }
}

// ---- Поточний план: масив кодів дій ---------------------------------------
#define WIZ_MAX_STEPS 8
static uint8_t g_wizActs[WIZ_MAX_STEPS];
static int     g_wizActN = 0;

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

        d.fmt = wizDetectFormat();

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

// ------------------------------------------------------------------ ПЛАН
// Детерміновано будує послідовність КОДІВ дій із набору проблем.
static void wizComputeActions(const BatteryDiag &d) {
    g_wizActN = 0;
    auto add = [](uint8_t a) { if (g_wizActN < WIZ_MAX_STEPS) g_wizActs[g_wizActN++] = a; };
    uint32_t is = d.issues;

    if (is & ISS_NO_CHIP) { add(ACT_READ); return; }

    // Добудова — ПЕРШИМ кроком: без валідного заголовка інші перевірки чипа
    // (модель, профіль) однаково нічого не скажуть, а дзеркало вже на місці —
    // шкоди від зайвого кроку немає.
    if (is & ISS_CHARGER_PARTIAL) add(ACT_HDRFIX);

    if (is & (ISS_BLANK33 | ISS_NO_MODEL))       add(ACT_RESTORE);
    else if (is & (ISS_HDR_BAD | ISS_MIRROR_BAD | ISS_BLOCK_SUM)) add(ACT_REPAIR);

    // Перешифрування — ПІСЛЯ ремонту структури (він може полагодити суми
    // блоків) і ДО калібрування: на ЗП має їхати пакет, чиї поля рація вже
    // читає правильно. Відновлення еталона робить це саме, тож після нього
    // окремий крок зайвий.
    if (!(is & (ISS_BLANK33 | ISS_NO_MODEL)) &&
        (is & (ISS_CRYPT_WRONG | ISS_USE_BEFORE_CHG))) add(ACT_CRYPT);

    // ЧУЖА навчена калібровка — головна причина «невідомого акумулятора» після
    // заміни елементів. Прибираємо її ПЕРШОЮ, далі обов'язково цикл на ЗП.
    if (is & (ISS_TAIL_FOREIGN | ISS_TAIL_ERASED | ISS_CCA_OVERFLOW | ISS_NEEDS_CALIB)) {
        add(ACT_RECAL); add(ACT_CHARGE_STATION);
    }
    else if (is & ISS_HEALTH_LOW)                   add(ACT_RESET);

    add(ACT_VERIFY);
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
static const char *wizActionName(uint8_t a) {
    switch (a) {
        case ACT_READ: return "read";            case ACT_RESTORE: return "restore";
        case ACT_REPAIR: return "repair";        case ACT_RECAL: return "recal";
        case ACT_RECAL_DEEP: return "recaldeep";
        case ACT_RESET: return "reset";          case ACT_CLEAN: return "clean";
        case ACT_SETCHARGE_AUTO: return "charge";case ACT_CHARGE_STATION: return "station";
        case ACT_HDRFIX: return "hdrfix";
        case ACT_CRYPT: return "crypt";
        case ACT_VERIFY: return "verify";        default: return "none";
    }
}

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
        o += ",\"action\":\""; o += wizActionName(acts[i]); o += "\"";
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
