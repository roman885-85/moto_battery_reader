#ifndef SERIAL_API_H
#define SERIAL_API_H

// ---------------------------------------------------------------------------
// Командний протокол по USB-Serial (115200) — дублює функціонал веб-API,
// щоб Windows-клієнт (Web Serial / нативний) працював по COM-порту.
// Працює паралельно з Wi-Fi: loop() викликає і server.handleClient(), і
// serialTask().
//
// Формат: клієнт надсилає один рядок "CMD [аргумент]\n". Пристрій відповідає
// рівно одним рядком відповіді з префіксом "#R#" + JSON. Відладкові рядки
// (без префікса) клієнт ігнорує.
//
// Команди:
//   PING                 -> {"ok":true,"dev":"MotoBatteryReader","ver":3,"authed":..}
//   AUTH <пароль>        -> (опційно) звірити пароль ADMIN_PASSWORD, підсвітити статус
//   READ                 -> {"ok":..,"ds2433":..,"ds2438":..}  (зчитати чіпи)
//   INFO                 -> усі декодовані поля (модель/%/цикли/цілісність/DS2438)
//   GET33 / GET38        -> {"ok":true,"hex":"AA BB .."}  (сирий дамп)
//   WRITE33 <hex512>     -> запис DS2433 як є (для еталонного дампа)
//   WRITEFIX33 <hex512>  -> запис DS2433 + автовиправлення суми заголовка і дзеркала
//   WRITE38 <hex64>      -> запис DS2438
//   RESET                -> скидання лічильників (рекалібрування)
//   REPAIR               -> ремонт цілісності (суми + дзеркало)
//   CLEAN                -> очистка (стерти все, крім ідентичності/калібрування)
//   WIPE33               -> ПОВНЕ стирання DS2433 (крайній випадок, все у 0xFF)
//   WIPE38               -> ПОВНЕ стирання DS2438 (крайній випадок, все у 0xFF)
//   SETCAP <0..100>      -> змінити ємність/знос %
//   SETMAH <мА·год>      -> змінити залишкову ємність (регістр ICA)
//   SETMODEL <NAME>      -> ручний запис моделі (part number, 3..9 A-Z0-9)
//   SETETM <сек>         -> ETM (наробіток) -> «дата першого користування» у рації
//   SETHEALTH <1..100>   -> знос/здоров'я, % (пише CTS у зашифрований блок RECOND)
//   TEMPLATES            -> список вшитих моделей для ініціалізації (без пароля)
//   OPS                  -> каталог операцій (спільний для екрана/веба/клієнта)
//   CLOCK [РРРРММДД]     -> системна дата пристрою (без аргументу — прочитати).
//                          RTC/NTP тут немає: дату приносить клієнт, вона
//                          зберігається в SPIFFS і потрібна для наробітку
//   DISCHARGE [мВ]       -> почати керований розряд (типово до 7200 мВ)
//   DISCHARGE STOP       -> зупинити розряд;  DISCHARGE ? -> стан розряду
//   CHARGE [%]           -> почати керований заряд (ШІМ на ключ) до обраного відсотка
//                           (типово 100, мінімум CHARGE_TARGET_PCT_MIN)
//   CHARGE STOP          -> зупинити заряд;  CHARGE ? -> стан заряду
//   CHARGE WAKE          -> примусове ПРОБУДЖЕННЯ пакета, що не читається
//                           (після заміни елементів): коротко тримає на клемах
//                           напругу зарядника, доки контролер не відпустить
//                           пакет. Межі — у settings.h, блок «ПРИМУСОВЕ
//                           ПРОБУДЖЕННЯ»; зупиняється тим самим CHARGE STOP
//   INITBAT <MODEL> <мАг>-> ініціалізувати порожній чип як новий АКБ моделі
//   HDRFIX               -> добудувати заголовок DS2433 із дзеркала DS2438
//                          (коли зарядна станція сама почала, але не завершила)
//   CHARGE MA=<мА>       -> ручна уставка струму заряду (0 = автомат)
//   MIRROR [APPLY|TAKE=|BYTE=|RATED=|TODAY=] -> синхронізація дзеркала 2438 -> 2433
//   SAMPLES              -> вбудовані зразки моніторів копій (для CLONE)
//   CLONE <hex128> [RATED=] [RSENSE=] [MODEL=] [MFG=] [USE=] [HEALTH=] [ID33=1]
//                  [ZERO=0] [RECHECK=0]
//                        -> КРАЙНІЙ ЗАСІБ: відновлення за зразком китайської
//                           копії — монітор зі зразка, DS2433 стерто; ID33=1
//                           додатково пише ЕКСПЕРИМЕНТАЛЬНУ ідентичність
//   RESTORE <MODEL> [VERBATIM] [FIXES=..] [RATED=мАг] [RSENSE=мОм*100|RSMODEL=..] [MFG=..]
//            [TAIL=FRESH|ERASE] [HEALTH=%] [USE=РРРРММДД] [CAL=] [CYC=] [NONIMP=]
//            [TODAY=РРРРММДД] [ETMSRC=USE|PACK] -> відновити модельну
//                          частину еталона (без чужого навченого хвоста), з
//                          правками під цей пакет; VERBATIM — байт-у-байт
//   RESTOREPLAN <MODEL> [NOREAD] [FIXES=..] [RATED=мАг] [RSENSE=..|RSMODEL=..] [MFG=..] [HEALTH=%] [USE=РРРРММДД] [CAL=] [CYC=] [NONIMP=] [TODAY=..] [ETMSRC=..] -> що саме буде
//                          виправлено в еталоні під цей пакет (нічого не пише)
//   FIXES <MODEL> [FIXES=..] [RATED=мАг] [RSENSE=..|RSMODEL=..] [MFG=..] [HEALTH=%] [USE=РРРРММДД] [CAL=] [CYC=] [NONIMP=] [TODAY=..] [ETMSRC=..] -> записати ЛИШЕ правки до того, що вже
//                          в чипах; еталон і навчена калібровка не чіпаються
//   WIZARD               -> Майстер: зчитати + аналіз/проблеми/план (JSON)
//   WIZSTEP <idx> [MODEL]-> Майстер: виконати крок плану (model для відновлення)
//   WIZRESET             -> Майстер: скинути журнал продовження поточного АКБ
//   WIZLIST              -> Майстер: усі збережені журнали (серійник + план)
//   WIZDEL <serial>      -> Майстер: видалити журнал за серійником
//   RECAL [DEEP]         -> ремонт після заміни елементів (стерти навчений хвіст);
//                          DEEP — додатково записи ємності й журнал використання
//   REBOOT               -> перезавантаження ESP32
//
// Пароль по USB — ОПЦІЙНИЙ: фізичний доступ до кабелю = дозвіл на запис, тож
// команди запису працюють і без "AUTH". "AUTH <пароль>" лише звіряє пароль і
// підсвічує статус у клієнті. (Мережевий веб-інтерфейс пароль вимагає.)
// ---------------------------------------------------------------------------

#include "web_server.h"   // dump-буфери, readAllChips/performReset/repairDumps,
                          // hexToBytes/fixHeaderChecksum/mirrorOk/headerChecksumOk

#include "bt_link.h"      // той самий протокол по Bluetooth SPP

// ── ТРАНСПОРТ ──────────────────────────────────────────────────────────────
//  Протокол один, а каналів два: USB і Bluetooth. Обидва — Stream, тож усе,
//  що нижче, працює через покажчик і не знає, куди саме пише.
//
//  ⚑ ВІДПОВІДЬ ІДЕ ТУДИ, ЗВІДКИ ПРИЙШЛА КОМАНДА. Це не дрібниця: якби sResp()
//  завжди писав у Serial, клієнт по Bluetooth не бачив би ЖОДНОЇ відповіді, а
//  чужі відповіді сипались би в USB-консоль. g_serOut перемикається на час
//  виконання команди й повертається назад.
//
//  ⚑ І БУФЕРИ ВХОДУ ОКРЕМІ. Один спільний накопичувач означав би, що дві
//  команди, які прийшли одночасно різними каналами, склеяться в одну — рідко,
//  недетерміновано й дуже неприємно для пошуку.
static Stream *g_serOut   = &Serial;   // куди відповідати ЗАРАЗ
static bool    g_serViaBt = false;     // чи прийшла поточна команда по радіо

static String g_serIn;         // накопичувач USB
static String g_serInBt;       // накопичувач Bluetooth — окремий, див. вище
static bool   g_serAuthed = false;  // чи авторизований клієнт (AUTH <пароль>)

static void sResp(const String &json) {
    g_serOut->print("#R#");
    g_serOut->println(json);
}

static String serHex(const uint8_t *d, int n) {
    String s; s.reserve(n * 3);
    char h[4];
    for (int i = 0; i < n; i++) { sprintf(h, "%02X", d[i]); s += h; if (i + 1 < n) s += ' '; }
    return s;
}

// Повний INFO: об'єднує /api/info і /api/info2438.
static String serBuildInfo() {
    String j = "{\"ok\":true";
    char modelBuf[24] = "";
    decodeModel(modelBuf, sizeof(modelBuf));   // потрібна й для блоку DS2438 (ємність за моделлю)
    j += ",\"has33\":" + String(hasDump ? "true" : "false");
    j += ",\"has38\":" + String(hasDump2438 ? "true" : "false");

    if (hasDump) {
        String m = String(modelBuf);
        int cap = -1, wear = -1; decodeCapacity(&cap, &wear);
        const char *reason; bool genuine = batteryGenuine(&reason);
        j += ",\"model\":\"" + m + "\"";
        j += ",\"capacity\":" + String(cap);
        j += ",\"wear\":" + String(wear);
        j += ",\"genuine\":" + String(genuine ? "true" : "false");
        j += ",\"authReason\":\"" + String(reason) + "\"";
        j += ",\"headerOk\":" + String(headerChecksumOk(batteryDump) ? "true" : "false");
        j += ",\"mirrorOk\":" + String((hasDump2438 ? mirrorOk(batteryDump, batteryDump2438) : true) ? "true" : "false");
        j += ",\"profileOk\":" + String(impresProfileOk(batteryDump) ? "true" : "false");
        j += ",\"copyright\":\"" + String(!impresHasCopyright(batteryDump) ? "none"
                                        : impresRecordOk(batteryDump, IMPRES_COPYRIGHT) ? "ok"
                                        : "broken") + "\"";
        j += ",\"hex33\":\"" + serHex(batteryDump, DUMP_SIZE) + "\"";
    }
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        int16_t cur = (int16_t)((batteryDump2438[6] << 8) | batteryDump2438[5]);
        // Шунт — із чипа (DS2438[56..57]); константа лишається лише запасним
        // варіантом. Див. impres_bms.h: у родини 4409 він удвічі більший.
        const ImpresBms &bms = impresBmsOf(hasDump ? batteryDump : nullptr,
                                           batteryDump2438,
                                           hasSN2433 ? chipSN2433 : nullptr,
                                           DS2438_RSENSE_OHM);
        float rs = bms.rsense > 0.0f ? bms.rsense : DS2438_RSENSE_OHM;
        float i_mA = (float)cur / (4096.0f * rs) * 1000.0f;
        uint8_t ica = batteryDump2438[12];
        uint16_t cca = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
        uint16_t dca = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];
        const char *csrc; int charge = batteryPercent(&csrc);
        String serial = "";
        if (hasSN2438) { char b[3]; for (int i = 0; i < 8; i++) { sprintf(b, "%02X", chipSN2438[i]); serial += b; } }
        j += ",\"voltage\":" + String(vraw * 0.01f, 2);
        j += ",\"temperature\":" + String(traw * 0.03125f, 1);
        j += ",\"currentMa\":" + String(i_mA, 0);
        uint32_t etm = ((uint32_t)batteryDump2438[11] << 24) | ((uint32_t)batteryDump2438[10] << 16) |
                       ((uint32_t)batteryDump2438[9] << 8) | batteryDump2438[8];
        j += ",\"ica\":" + String(ica) + ",\"cca\":" + String(cca) + ",\"dca\":" + String(dca);
        j += ",\"etmSec\":" + String(etm);
        // Паспортна ємність — за МОДЕЛЛЮ (таблиця IMPRES_RATED), а не єдина
        // константа BATTERY_RATED_MAH: через неї цикли й мА·год розходилися
        // з показаннями станції на всіх моделях, крім однієї.
        int ratedMah = impresRatedMahFor(hasDump ? batteryDump : nullptr, modelBuf);
        j += ",\"icaMah\":" + String(impresIcaToMahRs(ica, ratedMah, rs));
        // Ціна розряду CCA/DCA — 15.625 мВ·год (даташит), а не 0.4882 як в ICA.
        j += ",\"ccaMah\":" + String(bms.ccaMah);
        j += ",\"dcaMah\":" + String(bms.dcaMah);
        j += ",\"ccaCycles\":" + String((int)(bms.ccaMah / ratedMah));
        j += ",\"dcaCycles\":" + String((int)(bms.dcaMah / ratedMah));
        j += ",\"rsense\":" + String(rs, 5);
        j += ",\"rsenseChip\":" + String(bms.rsenseFromChip ? 1 : 0);
        j += ",\"ratedMah\":" + String(ratedMah);
        j += ",\"charge\":" + String(charge) + ",\"chargeSrc\":\"" + String(csrc) + "\"";
        // Шкала «заряд за напругою» — з пристрою, щоб клієнти не тримали
        // власних копій чисел і не брехали в підписах після її зміни.
        j += ",\"emptyMv\":" + String(BATTERY_EMPTY_MV);
        j += ",\"fullMv\":"  + String(BATTERY_FULL_MV);
        j += ",\"scaleTxt\":\"" BATTERY_SCALE_TXT "\"";
        // ⚑ Ім'я силового ключа теж віддає ПРИСТРІЙ. Клієнти тримали його
        //  рядком у себе («PNP B772M»), і після заміни на P-MOSFET усі троє
        //  почали називати чуже залізо. Тепер назва одна — з settings.h.
        j += ",\"swName\":\"" CHARGE_SW_NAME "\"";
        j += ",\"serial\":\"" + serial + "\"";
        if (hasSN2433) {
            char b[3]; String s33 = "";
            for (int i = 0; i < 8; i++) { sprintf(b, "%02X", chipSN2433[i]); s33 += b; }
            j += ",\"serial33\":\"" + s33 + "\"";
        }
        // Штатні поля Motorola: цикли — без ключа, решта — після дешифрування.
        if (bms.ok) {
            j += ",\"bms\":{\"kit\":\"" + String(bms.kit) + "\"";
            j += ",\"cycles\":" + String(bms.cycles);
            j += ",\"nonImpresCycles\":" + String(bms.nonImpresCycles);
            j += ",\"haveKey\":" + String(bms.haveKey ? 1 : 0);
            j += ",\"keyGuessed\":" + String(bms.keyGuessed ? 1 : 0);
            if (bms.haveKey) {
                j += ",\"health\":" + String(bms.health);
                j += ",\"potentialMah\":" + String(bms.potentialMah);
                j += ",\"firstUseMah\":" + String(bms.firstUseMah);
                j += ",\"cyclesEnc\":" + String(bms.cyclesEnc);
                j += ",\"calCycles\":" + String(bms.calCycles);
                j += ",\"reverts\":" + String(bms.reverts);
                j += ",\"topOffCycles\":" + String(bms.topOffCycles);
                char d[12];
                snprintf(d, sizeof(d), "%04d-%02d-%02d", bms.mfgY, bms.mfgM, bms.mfgD);
                j += ",\"mfgDate\":\"" + String(d) + "\"";
                if (bms.useY) {
                    snprintf(d, sizeof(d), "%04d-%02d-%02d", bms.useY, bms.useM, bms.useD);
                    j += ",\"firstUseDate\":\"" + String(d) + "\"";
                }
            }
            j += "}";
        }
        j += ",\"hex38\":\"" + serHex(batteryDump2438, DS2438_MEM_SIZE) + "\"";
    }
    j += "}";
    return j;
}

// Запис DS2433 з hex-аргументу. fix=true -> автовиправлення суми заголовка+дзеркало.
static void serWrite33(const String &arg, bool fix) {
    static uint8_t buf[DUMP_SIZE];
    if (hexToBytes(arg, buf, DUMP_SIZE) != DUMP_SIZE) { sResp("{\"ok\":false,\"err\":\"need 512 bytes\"}"); return; }
    if (fix) {
        fixHeaderChecksum(buf);
        if (hasDump2438 && mirrorSourceValid(batteryDump2438)) { for (int i = 0; i < 26; i++) buf[1 + i] = batteryDump2438[24 + i]; fixHeaderChecksum(buf); }
    }
    ledSet(LED_WRITE); displayShow("USB ЗАПИС 2433");
    bool ok = battery.writeBattery(buf, DUMP_SIZE);
    if (ok) { memcpy(batteryDump, buf, DUMP_SIZE); hasDump = true; saveDump("/dump.bin", buf, DUMP_SIZE); }
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB 2433 OK" : "USB 2433 ЗБІЙ");
    sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"write failed\"}");
}

static void serWrite38(const String &arg) {
    static uint8_t buf[DS2438_MEM_SIZE];
    if (hexToBytes(arg, buf, DS2438_MEM_SIZE) != DS2438_MEM_SIZE) { sResp("{\"ok\":false,\"err\":\"need 64 bytes\"}"); return; }
    ledSet(LED_WRITE); displayShow("USB ЗАПИС 2438");
    bool ok = battery.writeDS2438(buf, DS2438_MEM_SIZE);
    if (ok) { memcpy(batteryDump2438, buf, DS2438_MEM_SIZE); hasDump2438 = true; saveDump("/dump2438.bin", buf, DS2438_MEM_SIZE); }
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB 2438 OK" : "USB 2438 ЗБІЙ");
    sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"write failed\"}");
}

// Змінити залишкову ємність (заряд) в мА·ч -> регістр ICA DS2438.
static void serSetMah(const String &arg) {
    if (!hasDump2438) { sResp("{\"ok\":false,\"err\":\"read first\"}"); return; }
    long mah = arg.toInt();
    char smModel[16] = "";
    if (hasDump) impresModelName(batteryDump, smModel, sizeof(smModel));
    long ica = impresIcaFromMahRs(mah, impresRatedMahFor(hasDump ? batteryDump : nullptr, smModel),
                                  impresBmsRsense(batteryDump2438));
    if (ica < 0) ica = 0; if (ica > 255) ica = 255;
    batteryDump2438[12] = (uint8_t)ica;
    ledSet(LED_WRITE); displayShow("USB ЄМН mAh");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB mAh OK" : "USB mAh ЗБІЙ");
    sResp(ok ? (String("{\"ok\":true,\"ica\":") + ica + "}") : "{\"ok\":false,\"err\":\"write failed\"}");
}

// Рівень заряду: arg=="auto" — з напруги (шкала BATTERY_SCALE_TXT); інакше pct 0..100.
static void serSetCharge(const String &arg) {
    if (!hasDump2438) { sResp("{\"ok\":false,\"err\":\"read first\"}"); return; }
    int pct;
    if (arg == "auto" || arg == "AUTO") pct = chargePctFromVoltage();
    else pct = arg.toInt();
    ledSet(LED_WRITE); displayShow("USB ЗАРЯД");
    bool ok = performSetChargePct(pct);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "ЗАРЯД OK" : "ЗАРЯД ЗБІЙ");
    sResp(ok ? (String("{\"ok\":true,\"pct\":") + pct + ",\"ica\":" + batteryDump2438[12] + "}")
             : "{\"ok\":false,\"err\":\"write failed\"}");
}

static void serSetCap(const String &arg) {
    if (!hasDump) { sResp("{\"ok\":false,\"err\":\"read first\"}"); return; }
    int cap = arg.toInt(); if (cap < 0) cap = 0; if (cap > 100) cap = 100;
    // РУЧНИЙ РЕЖИМ: правка байта у ЗАВОДСЬКІЙ таблиці моделі @0x129.
    // Це НЕ здоров'я АКБ — рація рахує строк служби сама (див. impres_format.h),
    // тому показання станції від цієї правки не зміняться.
    const int rec = IMPRES_FACTORY_REC;
    if (!impresRecordOk(batteryDump, rec) || batteryDump[rec] != 0x17) {
        sResp("{\"ok\":false,\"err\":\"factory table @0x129 missing/corrupt\"}"); return;
    }
    batteryDump[rec + 21] = (uint8_t)cap;
    impresFixRecord(batteryDump, rec, 0x17);
    ledSet(LED_WRITE); displayShow("USB ЄМН...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB ЄМН OK" : "USB ЄМН ЗБІЙ");
    sResp(ok ? "{\"ok\":true,\"note\":\"factory table byte; radio computes health itself\"}"
             : "{\"ok\":false,\"err\":\"write failed\"}");
}

// SOUND — налаштування звуку через USB. Формати:
//   SOUND               поточні значення, межі й перелік сигналів
//   SOUND SET tempo=150 glide=200 vol=180 en=1 clk=0 atk=30 rel=80 st=-2
//   SOUND TEST ok       прослухати сигнал (нічого не змінює)
//   SOUND RESET         повернути заводські
// Межі затискає buzzSetCfg() — та сама функція, що й для вебу, тож USB не може
// виставити те, чого не дозволяє веб, і навпаки.
static String serSound(const String &argIn) {
    String a = argIn; a.trim();
    String head = a; int sp = a.indexOf(' ');
    if (sp >= 0) head = a.substring(0, sp);
    head.toUpperCase();
    String rest = (sp < 0) ? String("") : a.substring(sp + 1);
    rest.trim();

    if (head == "TEST") {
        rest.toLowerCase();
        // Невідомий ключ і вимкнений звук — різні речі: перше помилка клієнта,
        // друге штатна тиша, яку треба пояснити, а не видати за програвання.
        if (!buzzFindSignal(rest.c_str()))
            return String("{\"ok\":false,\"err\":\"невідомий сигнал '") + rest + "'\"}";
        uint32_t ms = buzzPlayNamed(rest.c_str());
        String r = String("{\"ok\":true,\"name\":\"") + rest + "\",\"ms\":" + (int)ms +
                   ",\"played\":" + (ms ? "true" : "false");
        if (!ms) r += ",\"note\":\"звук вимкнено в налаштуваннях\"";
        return r + "}";
    }
    if (head == "" || head == "?" || head == "GET") {
        String j = soundFullJson(); j.replace("\"status\":\"success\"", "\"ok\":true");
        return j;
    }
    if (head != "SET" && head != "RESET")
        return "{\"ok\":false,\"err\":\"очікується SET / TEST / RESET\"}";

    BuzzCfg c = buzzGetCfg();
    if (head == "RESET") {
        BuzzCfg d = { true, true, BUZZER_VOLUME, 100, 100,
                      BUZZ_ATTACK_MS, BUZZ_RELEASE_MS, 0 };
        c = d;
        rest = "";
    }
    String test;
    // Розбираємо «ключ=значення», розділені пробілами. Невідомий ключ — це
    // помилка, а не мовчазне ігнорування: інакше друкарська помилка виглядала б
    // як «пристрій не слухається».
    while (rest.length()) {
        int s = rest.indexOf(' ');
        String tok = (s < 0) ? rest : rest.substring(0, s);
        rest = (s < 0) ? String("") : rest.substring(s + 1);
        rest.trim();
        int eq = tok.indexOf('=');
        if (eq < 0) return String("{\"ok\":false,\"err\":\"очікується ключ=значення, а не '") + tok + "'\"}";
        String k = tok.substring(0, eq), v = tok.substring(eq + 1);
        k.trim(); k.toLowerCase(); v.trim();
        long n = v.toInt();
        bool b = !(v == "0" || v == "off" || v == "false" || v == "no");
        if      (k == "en"    || k == "enabled")   c.enabled   = b;
        else if (k == "clk"   || k == "click")     c.clickOn   = b;
        else if (k == "vol"   || k == "volume")    c.volume    = (uint8_t)constrain(n, 0, 255);
        else if (k == "tempo")                     c.tempoPct  = (uint16_t)constrain(n, 0, 1000);
        else if (k == "glide")                     c.glidePct  = (uint16_t)constrain(n, 0, 1000);
        else if (k == "atk"   || k == "attack")    c.attackMs  = (uint16_t)constrain(n, 0, 1000);
        else if (k == "rel"   || k == "release")   c.releaseMs = (uint16_t)constrain(n, 0, 1000);
        else if (k == "st"    || k == "semitones") c.semitones = (int8_t)constrain(n, -12, 12);
        else if (k == "test")                      { v.toLowerCase(); test = v; }
        else return String("{\"ok\":false,\"err\":\"невідомий ключ '") + k + "'\"}";
    }
    buzzSetCfg(c);
    bool saved = soundCfgSave();
    uint32_t ms = test.length() ? buzzPlayNamed(test.c_str()) : 0;

    String j = soundFullJson();
    j.replace("\"status\":\"success\"", "\"ok\":true");
    j.remove(j.length() - 1);
    j += ",\"saved\":"; j += saved ? "true" : "false";
    j += ",\"testMs\":"; j += (int)ms; j += "}";
    return j;
}

// Хвіст команд RESTORE / RESTOREPLAN: «<МОДЕЛЬ> [VERBATIM] [NOREAD] [FIXES=a,b]
// [RATED=мАг] [RSENSE=мОм*100] [RSMODEL=МОДЕЛЬ] [MFG=РРРРММДД] [TAIL=FRESH|ERASE]
// [HEALTH=%] [USE=РРРРММДД] [CAL=] [CYC=] [NONIMP=] [TODAY=РРРРММДД]
// [ETMSRC=USE|PACK]». RSENSE/RSMODEL — шунт, коли в
// пакеті свого немає: числом або з бібліотеки еталонів.
// Одна функція на обидві команди — щоб вони не розійшлися в тому, що вважають
// моделлю, а що прапорцем. Модель — перше слово, яке не є прапорцем; регістр
// моделі й прапорців не має значення, а от ключі правок завжди малими.
struct SerRestoreArgs { String model, fixes, rsModel; bool verbatim, reread, haveFixes;
                       long rated, rsense, mfg, useDate, today;
                       int tail, health, cal, cyc, nonImp, etmSrc; };
static SerRestoreArgs serParseRestore(const String &argIn) {
    SerRestoreArgs a; a.verbatim = false; a.reread = true; a.haveFixes = false;
    a.rated = -1; a.rsense = -1; a.mfg = -1; a.tail = 0 /* RTAIL_FRESH */; a.health = -1;
    a.useDate = -1; a.cal = -1; a.cyc = -1; a.nonImp = -1;
    a.today = -1; a.etmSrc = -1;
    String rest = argIn; rest.trim();
    while (rest.length()) {
        int s = rest.indexOf(' ');
        String tok = (s < 0) ? rest : rest.substring(0, s);
        rest = (s < 0) ? String("") : rest.substring(s + 1);
        rest.trim();
        if (!tok.length()) continue;
        String up = tok; up.toUpperCase();
        if      (up == "VERBATIM")        a.verbatim = true;
        else if (up == "NOREAD")          a.reread = false;
        else if (up.startsWith("FIXES=")) { a.fixes = tok.substring(6); a.fixes.toLowerCase(); a.haveFixes = true; }
        else if (up.startsWith("RATED="))  a.rated = up.substring(6).toInt();
        // Шунт: RSENSE=4565 — числом (мОм×100), RSMODEL=PMNN4488A — з бібліотеки.
        else if (up.startsWith("RSENSE="))  a.rsense  = up.substring(7).toInt();
        else if (up.startsWith("RSMODEL=")) a.rsModel = up.substring(8);
        // Дата виготовлення одним числом: MFG=20140522 (0 — прибрати ручну).
        else if (up.startsWith("MFG="))     a.mfg = up.substring(4).toInt();
        // TAIL=ERASE — лишити навчений хвіст стертим (див. RTAIL_* у web_server.h).
        else if (up.startsWith("TAIL="))    a.tail = (up.substring(5) == "ERASE") ? 1 : 0;
        // HEALTH=80 — знос (здоров'я) у відсотках; 0 прибирає ручне значення.
        else if (up.startsWith("HEALTH="))  a.health = up.substring(7).toInt();
        // USE=РРРРММДД — дата першого запуску; CAL/CYC/NONIMP — лічильники,
        // у них 0 є повноцінним значенням, тож «не вписували» — це відсутність.
        else if (up.startsWith("USE="))     a.useDate = up.substring(4).toInt();
        else if (up.startsWith("CAL="))     a.cal    = up.substring(4).toInt();
        else if (up.startsWith("CYC="))     a.cyc    = up.substring(4).toInt();
        else if (up.startsWith("NONIMP="))  a.nonImp = up.substring(7).toInt();
        // TODAY=РРРРММДД — сьогоднішня дата від клієнта: годинника в пристрої
        // немає, а без «сьогодні» наробіток із дати першого запуску не порахувати.
        // ETMSRC=USE — рахувати наробіток із дати запуску, ETMSRC=PACK — лишити свій.
        else if (up.startsWith("TODAY="))   a.today = up.substring(6).toInt();
        else if (up.startsWith("ETMSRC="))  a.etmSrc = (up.substring(7) == "USE") ? 1 : 0;
        else if (!a.model.length())       a.model = up;
    }
    return a;
}

// Ємність нових банок ставимо ДО маски: увімкнення правки ємності міняє й
// паливомір, тож двома незалежними кроками ми показали б проміжне число.
static void serApplyPlanArgs(RestorePlan &p, const SerRestoreArgs &a) {
    restorePlanOverride(p, a.haveFixes ? a.fixes.c_str() : nullptr,
                        a.rated, a.rsense, a.rsModel.c_str(), a.mfg, a.health,
                        a.useDate, a.cal, a.cyc, a.nonImp, a.today, a.etmSrc);
}

// Каталог операцій (operations.h) — щоб десктопний клієнт малював той самий
// список у тому самому порядку, що екран і веб, без власних копій текстів.
static String serBuildOps() {
    String j = "{\"ok\":true,\"ops\":[";
    bool first = true;
    // chips — у яку мікросхему піде запис (див. operations.h). Клієнт показує
    // це поруч із назвою: переплутати DS2433 (ідентичність) із DS2438
    // (монітор) коштує або моделі, або калібрування вимірювача струму.
    auto add = [&](const char *key, const char *title, const char *detail,
                   int danger, const char *model, uint8_t chips) {
        if (!first) j += ",";
        first = false;
        j += "{\"key\":\""; j += key; j += "\",\"title\":\""; j += title;
        j += "\",\"detail\":\""; j += detail;
        j += "\",\"danger\":" + String(danger);
        j += ",\"chips\":" + String((int)chips);
        j += ",\"chipsText\":\""; j += opChipsText(chips); j += "\"";
        j += ",\"model\":\""; j += (model ? model : ""); j += "\"}";
    };
    for (int i = 0; i < OP_BASE_COUNT; i++)
        add(OP_DOC[i].key, OP_DOC[i].title, OP_DOC[i].detail, OP_TEXT[i].danger,
            nullptr, OP_DOC[i].chips);
    for (int t = 0; t < BATTERY_TEMPLATE_COUNT; t++)
        add("model", "Записати модельну частину еталона",
            "Без навченого хвоста донора.", OPD_WRITE, BATTERY_TEMPLATES[t].name,
            BATTERY_TEMPLATES[t].d38 ? OPC_BOTH : OPC_33);
    for (int t = 0; t < BATTERY_TEMPLATE_COUNT; t++)
        add("new", "Новий АКБ з порожнього чипа",
            "Модельна частина + монітор у стані нового пакета.", OPD_WIPE,
            BATTERY_TEMPLATES[t].name, OPC_BOTH);
    for (int e = 0; e < OP_EXPERT_COUNT; e++)
        add(OP_DOC_EXPERT[e].key, OP_DOC_EXPERT[e].title, OP_DOC_EXPERT[e].detail,
            OP_TEXT_EXPERT[e].danger, nullptr, OP_DOC_EXPERT[e].chips);
    j += "]}";
    return j;
}

static void serialExec(const String &line) {
    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? String("") : line.substring(sp + 1);
    cmd.toUpperCase(); cmd.trim();

    // Авторизація по USB — ОПЦІЙНА (фізичний доступ до кабелю = дозвіл на запис).
    // AUTH лише звіряє пароль і виставляє прапорець для індикатора у клієнті;
    // команди запису НЕ блокуються його відсутністю — інакше без пароля запис не
    // відбувався б зовсім. Мережевий веб-інтерфейс, навпаки, вимагає пароль.
    // ⚑ ПО РАДІО ПРАВИЛА ІНШІ. По USB перепусткою є сам кабель, тож усе
    //  відкрито. По Bluetooth у радіусі дії опиняється будь-хто, а серед
    //  команд є WIPE33 і WRITE33 — повне стирання пам'яті пакета. Тому читання
    //  вільне, а зміна чогось — лише після AUTH (правило в bt_link.h, щоб його
    //  міг перевірити хостовий тест).
    if (!serCmdAllowed(cmd.c_str(), g_serViaBt, g_serAuthed)) {
        sResp("{\"ok\":false,\"err\":\"по Bluetooth ця команда потребує AUTH <пароль>\",\"needAuth\":true}");
        return;
    }

    if (cmd == "AUTH") {
        g_serAuthed = (arg == ADMIN_PASSWORD);
        sResp(g_serAuthed ? "{\"ok\":true,\"authed\":true}"
                          : "{\"ok\":false,\"authed\":false,\"err\":\"невірний пароль\"}");
        return;
    }

    // ⚑ ВІДБИТОК ЗБІРКИ ЙДЕ ВЖЕ В РУКОСТИСКАННІ. Перше питання будь-якого
    //  розбору — «а чи та прошивка взагалі в приладі?», і доти жоден клієнт
    //  не міг на нього відповісти: дата збірки лежала тільки в /api/fs, куди
    //  ніхто не заглядає, а USB-клієнти туди й не ходять. PING — єдина
    //  команда, яку виконують ГАРАНТОВАНО й до всього іншого, тож відповідь
    //  видно ще до читання пакета. Дату й час підставляє компілятор.
    if (cmd == "PING")            sResp(String("{\"ok\":true,\"dev\":\"MotoBatteryReader\",\"ver\":3,\"build\":\"" __DATE__ " " __TIME__ "\",\"needAuth\":false,\"authed\":") + (g_serAuthed ? "true" : "false") + "}");
    else if (cmd == "READ")     { bool a, b; readAllChips(a, b);
                                  sResp(String("{\"ok\":") + ((a || b) ? "true" : "false") +
                                        ",\"ds2433\":" + (a ? "true" : "false") +
                                        ",\"ds2438\":" + (b ? "true" : "false") + "}"); }
    else if (cmd == "INFO")       sResp(serBuildInfo());
    else if (cmd == "GET33")      sResp(hasDump    ? (String("{\"ok\":true,\"hex\":\"") + serHex(batteryDump, DUMP_SIZE) + "\"}") : "{\"ok\":false}");
    else if (cmd == "GET38")      sResp(hasDump2438 ? (String("{\"ok\":true,\"hex\":\"") + serHex(batteryDump2438, DS2438_MEM_SIZE) + "\"}") : "{\"ok\":false}");
    else if (cmd == "WRITE33")    serWrite33(arg, false);
    else if (cmd == "WRITEFIX33") serWrite33(arg, true);
    else if (cmd == "WRITE38")    serWrite38(arg);
    else if (cmd == "RESET")    { bool ok = performReset(); sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"reset failed\"}"); }
    else if (cmd == "REPAIR")   { if (!hasDump && !hasDump2438) { sResp("{\"ok\":false,\"err\":\"read first\"}"); }
                                  else { repairDumps(); ledSet(LED_WRITE); displayShow("USB РЕМОНТ");
                                         bool ok = true;
                                         if (hasDump)     ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
                                         if (hasDump2438) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
                                         if (ok) { if (hasDump) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
                                                   if (hasDump2438) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE); }
                                         ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB РЕМОНТ OK" : "USB РЕМОНТ ЗБІЙ");
                                         sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"write failed\"}"); } }
    else if (cmd == "SETCAP")     serSetCap(arg);
    else if (cmd == "SETMAH")     serSetMah(arg);
    else if (cmd == "SETCHG")     serSetCharge(arg);
    else if (cmd == "SETETM")   { if (!hasDump2438) { sResp("{\"ok\":false,\"err\":\"read first\"}"); }
                                  else { uint32_t sec = (uint32_t)strtoul(arg.c_str(), nullptr, 10);
                                         batteryDump2438[8]=sec&0xFF; batteryDump2438[9]=(sec>>8)&0xFF;
                                         batteryDump2438[10]=(sec>>16)&0xFF; batteryDump2438[11]=(sec>>24)&0xFF;
                                         ledSet(LED_WRITE); displayShow("USB ДАТА");
                                         bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
                                         if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
                                         ledSet(ok?LED_OK:LED_ERROR); displayShow(ok?"USB ДАТА OK":"USB ДАТА ЗБІЙ");
                                         sResp(ok ? (String("{\"ok\":true,\"etmSec\":")+sec+"}") : "{\"ok\":false,\"err\":\"write failed\"}"); } }
    else if (cmd == "SETMODEL") { String m = arg; m.trim(); m.toUpperCase();
                                  if (!modelNameValid(m.c_str()))
                                      sResp("{\"ok\":false,\"err\":\"модель: 3-9 символів A-Z0-9\"}");
                                  else if (!hasDump)
                                      sResp("{\"ok\":false,\"err\":\"спочатку зчитайте АКБ (READ)\"}");
                                  else { bool ok = performSetModel(m.c_str());
                                         sResp(ok ? (String("{\"ok\":true,\"model\":\"") + m + "\"}")
                                                  : "{\"ok\":false,\"err\":\"немає запису моделі у дампі (порожній/невідомий чіп) або збій запису — відновіть еталонний дамп\"}"); } }
    else if (cmd == "CLEAN")    { bool ok = performFactoryClean(); sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"clean failed\"}"); }
    else if (cmd == "WIPE33")   { bool ok = performWipe2433();     sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"wipe failed\"}"); }
    else if (cmd == "WIPE38")   { bool ok = performWipe2438();     sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"wipe failed\"}"); }
    else if (cmd == "TEMPLATES"){ String j = "{\"ok\":true,\"models\":[";
                                  for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++) { if (i) j += ","; j += "\""; j += BATTERY_TEMPLATES[i].name; j += "\""; }
                                  j += "]}"; sResp(j); }
    else if (cmd == "INITBAT")  { int s2 = arg.indexOf(' ');
                                  String md = (s2 < 0) ? arg : arg.substring(0, s2);
                                  String mh = (s2 < 0) ? String("") : arg.substring(s2 + 1);
                                  md.trim(); md.toUpperCase(); mh.trim();
                                  long mah = mh.toInt();
                                  if (findTemplate(md.c_str()) < 0) sResp("{\"ok\":false,\"err\":\"немає шаблону моделі\"}");
                                  else if (mah <= 0)                sResp("{\"ok\":false,\"err\":\"вкажіть мА·год (INITBAT <модель> <мАг>)\"}");
                                  else { bool ok = performInitBattery(md.c_str(), mah);
                                         if (!ok) sResp("{\"ok\":false,\"err\":\"збій запису\"}");
                                         else {
                                             // identity=false означає, що ROM чипа не знайшли
                                             // й у пам'яті лишилась шифровка донора.
                                             String r = "{\"ok\":true,\"model\":\""; r += md;
                                             r += "\",\"identity\":";
                                             r += g_initIdentityGen ? "true" : "false";
                                             r += ",\"mfgDate\":"; r += g_initIdentityDate;
                                             sResp(r + "}");
                                         } } }
    // RESTOREPLAN <модель> [NOREAD] [FIXES=...] — що саме буде виправлено в
    // еталоні під ЦЕЙ пакет. Нічого не пише. Типово перечитує чипи, щоб заряд
    // був свіжий; NOREAD — перерахувати на вже прочитаних даних (клієнт смикає
    // це на кожну галочку, і сіпати 1-Wire щоразу не варто).
    else if (cmd == "RESTOREPLAN") { SerRestoreArgs a = serParseRestore(arg);
                                  RestorePlan p;
                                  if (!buildRestorePlanFor(a.model.c_str(), p, a.reread))
                                      sResp("{\"ok\":false,\"err\":\"немає шаблону моделі\"}");
                                  else { serApplyPlanArgs(p, a);
                                         sResp(String("{\"ok\":true,\"plan\":") + restorePlanJson(p) + "}"); } }
    // RESTORE <модель> [VERBATIM] [FIXES=charge,rsense,...] — за замовчуванням
    // навчений хвіст донора НЕ пишеться, а числа, що належать донору (заряд,
    // шунт, OFFSET АЦП), замінюються на реальні з цього пакета. VERBATIM —
    // ручний режим «байт-у-байт» для аналізу, без будь-яких правок.
    else if (cmd == "RESTORE")  { SerRestoreArgs a = serParseRestore(arg);
                                  String md = a.model; bool verbatim = a.verbatim;
                                  if (findTemplate(md.c_str()) < 0) sResp("{\"ok\":false,\"err\":\"немає шаблону моделі\"}");
                                  else { bool o33 = false, o38 = false;
                                         RestorePlan p; const RestorePlan *pp = nullptr;
                                         if (!verbatim && buildRestorePlanFor(md.c_str(), p, true)) {
                                             serApplyPlanArgs(p, a);
                                             pp = &p;
                                         }
                                         bool ok = performRestoreTemplate(md.c_str(), &o33, &o38,
                                                                          verbatim, pp, a.tail);
                                         String r = String("{\"ok\":") + (ok ? "true" : "false")
                                                  + ",\"ds2433\":" + (o33 ? "true" : "false")
                                                  + ",\"ds2438\":" + (o38 ? "true" : "false");
                                         if (pp) r += ",\"plan\":" + restorePlanJson(*pp);
                                         r += ok ? (String(",\"model\":\"") + md + "\"}")
                                                 : String(",\"err\":\"збій запису\"}");
                                         sResp(r); } }
    // FIXES <модель> [FIXES=…] [RATED=…] — застосувати ЛИШЕ правки до того, що
    // вже в чипах, без перезапису еталона (ідентичність не чіпається).
    else if (cmd == "FIXES")    { SerRestoreArgs a = serParseRestore(arg);
                                  RestorePlan p;
                                  if (!buildRestorePlanFor(a.model.c_str(), p, true))
                                      sResp("{\"ok\":false,\"err\":\"немає шаблону моделі\"}");
                                  else { serApplyPlanArgs(p, a);
                                         bool o33 = false, o38 = false;
                                         bool ok = performApplyFixes(p, &o33, &o38);
                                         String r = String("{\"ok\":") + (ok ? "true" : "false")
                                                  + ",\"ds2433\":" + (o33 ? "true" : "false")
                                                  + ",\"ds2438\":" + (o38 ? "true" : "false")
                                                  + ",\"plan\":" + restorePlanJson(p);
                                         if (!ok) r += ",\"err\":\"не обрано правок або збій запису\"";
                                         sResp(r + "}"); } }
    else if (cmd == "OPS")      { sResp(serBuildOps()); }
    else if (cmd == "SOUND")    { sResp(serSound(arg)); }
    // CLOCK — яку дату пристрій вважає сьогоднішньою; CLOCK РРРРММДД — завести
    // годинник. Годинника реального часу в ESP32 немає, а NTP недосяжний:
    // пристрій сам є точкою доступу. Ту саму дату несе TODAY= у RESTORE/FIXES.
    // SETHEALTH <1..100> — знос одним рухом (те саме, що правка «знос» у плані).
    // HDRFIX — добудувати заголовок DS2433 із дзеркала DS2438 (див. ISS_CHARGER_PARTIAL).
    else if (cmd == "HDRFIX")   { String note; bool ok = performHeaderComplete(&note);
                                  String r = "{\"ok\":"; r += ok ? "true" : "false";
                                  r += ",\"note\":\""; r += note; r += "\"}";
                                  sResp(r); }
    // CLONE <hex128> [RATED=] [RSENSE=] [MODEL=] [MFG=] [USE=] [HEALTH=] [ID33=1]
    // Крайній засіб: відновлення за зразком китайської копії. Перший токен —
    // дамп DS2438 копії (64 Б = 128 hex-символів), решта — ручні значення.
    // SAMPLES — вбудовані зразки моніторів копій (без пароля, нічого не пише).
    else if (cmd == "SAMPLES")  { String j = "{\"ok\":true,\"samples\":[";
                                  for (int i = 0; i < CLONE_SAMPLE_COUNT; i++) {
                                      if (i) j += ",";
                                      j += "{\"name\":\""; j += CLONE_SAMPLES[i].name;
                                      j += "\",\"note\":\""; j += CLONE_SAMPLES[i].note;
                                      j += "\",\"rated\":"; j += impresCloneRatedFrom38(CLONE_SAMPLES[i].d38);
                                      j += ",\"hex\":\"";
                                      char b2[3];
                                      for (int k = 0; k < DS2438_MEM_SIZE; k++) {
                                          sprintf(b2, "%02X", CLONE_SAMPLES[i].d38[k]); j += b2; }
                                      j += "\"}";
                                  }
                                  sResp(j + "]}"); }
    else if (cmd == "CLONE")    { String rest = arg; rest.trim();
                                  int sp = rest.indexOf(' ');
                                  String hx = (sp < 0) ? rest : rest.substring(0, sp);
                                  String tail = (sp < 0) ? String("") : rest.substring(sp + 1);
                                  uint8_t src[DS2438_MEM_SIZE];
                                  if (hexToBytes(hx, src, DS2438_MEM_SIZE) != DS2438_MEM_SIZE)
                                      sResp("{\"ok\":false,\"err\":\"потрібно рівно 64 байти DS2438\"}");
                                  else {
                                      long rt = 0, rs = 0, mf = 0, us = 0; int hp = 0; bool id33 = false;
                                      bool zc = true, rc = true;   // скидати лічильники, перевіряти заряд
                                      String md;
                                      while (tail.length()) {
                                          int q = tail.indexOf(' ');
                                          String tok = (q < 0) ? tail : tail.substring(0, q);
                                          tail = (q < 0) ? String("") : tail.substring(q + 1);
                                          tail.trim();
                                          String up = tok; up.toUpperCase();
                                          if      (up.startsWith("RATED="))  rt = up.substring(6).toInt();
                                          else if (up.startsWith("RSENSE=")) rs = up.substring(7).toInt();
                                          else if (up.startsWith("MFG="))    mf = up.substring(4).toInt();
                                          else if (up.startsWith("USE="))    us = up.substring(4).toInt();
                                          else if (up.startsWith("HEALTH=")) hp = up.substring(7).toInt();
                                          else if (up.startsWith("MODEL="))  md = up.substring(6);
                                          else if (up.startsWith("ID33="))   id33 = (up.substring(5) == "1");
                                          else if (up.startsWith("ZERO="))   zc = (up.substring(5) != "0");
                                          else if (up.startsWith("RECHECK=")) rc = (up.substring(8) != "0");
                                      }
                                      String note;
                                      bool ok = performCloneRestore(src, (int)rt, rs, id33, md.c_str(),
                                                    (int)(mf / 10000), (int)((mf / 100) % 100), (int)(mf % 100),
                                                    (int)(us / 10000), (int)((us / 100) % 100), (int)(us % 100),
                                                    hp, &note, zc, rc);
                                      String r = "{\"ok\":"; r += ok ? "true" : "false";
                                      r += ",\"note\":\""; r += note; r += "\"}";
                                      sResp(r);
                                  } }
    else if (cmd == "SETHEALTH"){ String h = arg; h.trim();
                                  int pct = h.toInt();
                                  if (!hasDump)        sResp("{\"ok\":false,\"err\":\"спочатку READ\"}");
                                  else if (!hasSN2433) sResp("{\"ok\":false,\"err\":\"ROM DS2433 невідомий\"}");
                                  else if (pct < 1 || pct > 100) sResp("{\"ok\":false,\"err\":\"знос 1..100\"}");
                                  else {
                                      char md[16] = ""; decodeModel(md, sizeof(md));
                                      RestorePlan hp;
                                      if (!md[0] || !buildRestorePlanFor(md, hp, false))
                                          sResp("{\"ok\":false,\"err\":\"немає еталона моделі\"}");
                                      else {
                                          restorePlanSetHealth(hp, pct);
                                          if (!hp.fx[RPF_CRYPT].on)
                                              sResp("{\"ok\":false,\"err\":\"блок RECOND не читається\"}");
                                          else {
                                              restorePlanApply(hp, batteryDump, nullptr, true);
                                              bool w = battery.writeBattery(batteryDump, DUMP_SIZE);
                                              if (w) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
                                              String r = "{\"ok\":"; r += w ? "true" : "false";
                                              r += ",\"health\":"; r += pct;
                                              r += ",\"cts\":";    r += (int)hp.ctsUse;
                                              sResp(r + "}");
                                          }
                                      }
                                  } }
    else if (cmd == "CLOCK")    { String a3 = arg; a3.trim();
                                  if (a3.length() && !deviceClockSetNum(a3.toInt()))
                                      sResp("{\"ok\":false,\"err\":\"потрібна дата РРРРММДД\"}");
                                  else { String r = "{\"ok\":true,\"today\":";
                                         r += deviceClockNum();
                                         r += ",\"src\":\""; r += deviceClockSrcName();
                                         sResp(r + "\"}"); } }
    // DISCHARGE [мВ] — почати розряд; DISCHARGE STOP — зупинити; DISCHARGE? — стан;
    // DISCHARGE MA=<мА> — ручний струм (0 = автомат);
    // DISCHARGE SMART | DISCHARGE AUTO — профіль (0.2C за ємністю або лінійка).
    else if (cmd == "DISCHARGE"){ String a2 = arg; a2.trim(); a2.toUpperCase();
                                  if (a2 == "STOP") { dischargeStop(DISR_USER); sResp(String("{\"ok\":true,\"discharge\":") + dischargeJson() + "}"); }
                                  else if (a2 == "?" || a2 == "STATUS") sResp(String("{\"ok\":true,\"discharge\":") + dischargeJson() + "}");
                                  // DISCHARGE DISMISS — прибрати підсумок завершеного розряду.
                                  else if (a2 == "DISMISS") { dischargeDismiss();
                                      sResp(String("{\"ok\":true,\"discharge\":") + dischargeJson() + "}"); }
                                  // DISCHARGE MA=<мА> — ручний струм; MA=0 повертає автомат.
                                  else if (a2.startsWith("MA=")) {
                                      long want = a2.substring(3).toInt();
                                      if (want < 0) want = 0;
                                      uint16_t got = dischargeSetManualMa((uint16_t)want);
                                      String r = "{\"ok\":true,\"manualMa\":"; r += got;
                                      r += ",\"asked\":"; r += want;
                                      r += ",\"discharge\":"; r += dischargeJson(); r += "}";
                                      sResp(r); }
                                  // DISCHARGE SMART / AUTO — профіль розряду.
                                  else if (a2 == "SMART" || a2 == "AUTO") {
                                      uint8_t got = dischargeSetProfile(a2 == "SMART" ? DIS_PROF_SMART
                                                                                      : DIS_PROF_FACTORY);
                                      String r = "{\"ok\":true,\"profile\":"; r += got;
                                      r += ",\"discharge\":"; r += dischargeJson(); r += "}";
                                      sResp(r); }
                                  else { const char *e = dischargeStart((uint16_t)a2.toInt());
                                         if (e) { String r = "{\"ok\":false,\"err\":\""; r += e; r += "\"}"; sResp(r); }
                                         else sResp(String("{\"ok\":true,\"discharge\":") + dischargeJson() + "}"); } }
    // CHARGE [%] — почати заряд до обраного відсотка (без аргументу -> 100 %);
    // CHARGE STOP — зупинити; CHARGE ? — стан; CHARGE MA=<мА> — ручний струм
    // (0 = автомат); CHARGE MV=<мВ> — ручна ціль у вольтах (0 = за відсотком);
    // CHARGE SMART | CHARGE AUTO — профіль (CC/CV за ємністю пакета або
    // заводська таблиця); CHARGE WAKE — примусове пробудження.
    else if (cmd == "CHARGE")   { String a3 = arg; a3.trim(); a3.toUpperCase();
                                  if (a3 == "STOP") { chargeStop(CHGR_USER); sResp(String("{\"ok\":true,\"charge\":") + chargeJson() + "}"); }
                                  else if (a3 == "?" || a3 == "STATUS") sResp(String("{\"ok\":true,\"charge\":") + chargeJson() + "}");
                                  // CHARGE DISMISS — прибрати підсумок завершеної операції.
                                  // Перед гейтом «версії без заряду» навмисно: саме вимикач
                                  // і створює підсумок, зупиняючи заряд, що йшов.
                                  else if (a3 == "DISMISS") { chargeDismiss();
                                      sResp(String("{\"ok\":true,\"charge\":") + chargeJson() + "}"); }
                                  // Версія пристрою без заряду: усе, крім «покажи стан» і
                                  // «зупинись», відхиляємо однією й тією ж причиною з прошивки.
                                  // Стан НЕ відхиляємо навмисно — саме з нього клієнт і
                                  // дізнається, чому кнопки сірі.
                                  else if (!chargeAvailable()) {
                                      String r = "{\"ok\":false,\"err\":\""; r += chargeUnavailText();
                                      r += "\",\"charge\":"; r += chargeJson(); r += "}"; sResp(r); }
                                  // CHARGE MA=<мА> — ручна уставка струму; MA=0 повертає автомат.
                                  // Діє і під час заряду: струм саме тоді й підбирають.
                                  else if (a3.startsWith("MA=")) {
                                      long want = a3.substring(3).toInt();
                                      if (want < 0) want = 0;
                                      uint16_t got = chargeSetManualMa((uint16_t)want);
                                      String r = "{\"ok\":true,\"manualMa\":"; r += got;
                                      r += ",\"asked\":"; r += want;
                                      r += ",\"charge\":"; r += chargeJson(); r += "}";
                                      sResp(r); }
                                  // CHARGE MV=<мВ> — ручна ЦІЛЬ у вольтах; MV=0 повертає ціль за відсотком.
                                  else if (a3.startsWith("MV=")) {
                                      long want = a3.substring(3).toInt();
                                      if (want < 0) want = 0;
                                      uint16_t got = chargeSetManualMv((uint16_t)want);
                                      String r = "{\"ok\":true,\"manualMv\":"; r += got;
                                      r += ",\"asked\":"; r += want;
                                      r += ",\"charge\":"; r += chargeJson(); r += "}";
                                      sResp(r); }
                                  // CHARGE SMART / AUTO — профіль заряду: розумний CC/CV за
                                  // паспортною ємністю пакета або заводська таблиця.
                                  else if (a3 == "SMART" || a3 == "AUTO") {
                                      uint8_t got = chargeSetProfile(a3 == "SMART" ? CHG_PROF_SMART
                                                                                   : CHG_PROF_FACTORY);
                                      String r = "{\"ok\":true,\"profile\":"; r += got;
                                      r += ",\"charge\":"; r += chargeJson(); r += "}";
                                      sResp(r); }
                                  // CHARGE WAKE — примусове пробудження пакета, що не читається.
                                  // Слово, а не число: режим працює без контролю температури,
                                  // і випадково набрати його не можна.
                                  else if (a3 == "WAKE") {
                                      const char *e = chargeWakeStart();
                                      if (e) { String r = "{\"ok\":false,\"err\":\""; r += e; r += "\"}"; sResp(r); }
                                      else sResp(String("{\"ok\":true,\"charge\":") + chargeJson() + "}"); }
                                  else { const char *e = chargeStart((uint8_t)a3.toInt());
                                         if (e) { String r = "{\"ok\":false,\"err\":\""; r += e; r += "\"}"; sResp(r); }
                                         else sResp(String("{\"ok\":true,\"charge\":") + chargeJson() + "}"); } }
    // MIRROR                      -> план синхронізації дзеркала (нічого не пише)
    // MIRROR TAKE=ALL|NONE         -> правка перед синхронізацією
    // MIRROR BYTE=<0..25> ON=0|1   -> те саме, по одному байту
    // MIRROR RATED=<мА·год>        -> вписати паспортну ємність руками (0 — скасувати)
    // MIRROR VAL=<0..2> ON=0|1     -> значеннєвий рядок (0 ETM, 1 CCA, 2 DCA)
    // MIRROR VSET=<0..2> V=<число> -> вписати число руками (доби або цикли; -1 — скасувати)
    // MIRROR APPLY                 -> записати те, що показано в плані
    // TODAY=РРРРММДД можна додати до будь-якої з форм — так само, як до плану
    // відновлення. Без дати не порахувати вік пакета, а без віку не сказати,
    // чи монітор узагалі від нього (див. mirrorPlanFactsRefresh).
    else if (cmd == "MIRROR")   { String a4 = arg; a4.trim(); a4.toUpperCase();
                                  { // дату виймаємо ПЕРШОЮ: вона потрібна вже під час побудови плану
                                      int tp = a4.indexOf("TODAY=");
                                      if (tp >= 0) {
                                          int te = a4.indexOf(' ', tp);
                                          String tv = (te < 0) ? a4.substring(tp + 6) : a4.substring(tp + 6, te);
                                          long had = deviceClockNum();
                                          mirrorPlanClock(tv.toInt());
                                          if (g_mirPlanReady && had != deviceClockNum())
                                              mirrorPlanFactsRefresh();   // план не чіпаємо: галочки людські
                                          a4 = a4.substring(0, tp) + ((te < 0) ? String("") : a4.substring(te + 1));
                                          a4.trim();
                                      } }
                                  if (!hasDump) { sResp("{\"ok\":false,\"err\":\"спочатку READ\"}"); }
                                  else {
                                      if (!g_mirPlanReady) mirrorPlanRefresh();
                                      if (a4 == "APPLY") {
                                          ledSet(LED_WRITE); displayShow("USB СИНХР 2433");
                                          int n = mirrorPlanApply(g_mirPlan, batteryDump);
                                          // лічильники циклів — теж у DS2433
                                          int nCyc = mirrorPlanApply33Vals(g_mirPlan, batteryDump);
                                          bool w = battery.writeBattery(batteryDump, DUMP_SIZE);
                                          if (w) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
                                          // Значеннєві рядки живуть у ДРУГОМУ чипі — пишемо і його.
                                          int n38 = 0;
                                          if (w && hasDump2438) {
                                              n38 = mirrorPlanApply38(g_mirPlan, batteryDump2438);
                                              if (n38) {
                                                  displayShow("USB СИНХР 2438");
                                                  w = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
                                                  if (w) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
                                              }
                                          }
                                          ledSet(w ? LED_OK : LED_ERROR);
                                          displayShow(w ? "USB СИНХР OK" : "USB СИНХР ЗБІЙ");
                                          mirrorPlanRefresh();
                                          String r = "{\"ok\":"; r += w ? "true" : "false";
                                          r += ",\"changed\":"; r += n;
                                          r += ",\"changedCyc\":"; r += nCyc;
                                          r += ",\"changed38\":"; r += n38;
                                          r += ",\"plan\":"; r += mirrorPlanJson(); r += "}";
                                          sResp(r);
                                      } else {
                                          // Розбір ключів у тому ж стилі, що й у CLONE/WIZSTEP.
                                          String tail = a4;
                                          while (tail.length()) {
                                              int q = tail.indexOf(' ');
                                              String tok = (q < 0) ? tail : tail.substring(0, q);
                                              tail = (q < 0) ? String("") : tail.substring(q + 1);
                                              tail.trim();
                                              if      (tok.startsWith("TAKE="))
                                                  mirrorPlanTakeAll(g_mirPlan, tok.substring(5) == "ALL");
                                              else if (tok.startsWith("RATED="))
                                                  mirrorPlanSetRated(g_mirPlan, tok.substring(6).toInt());
                                              else if (tok.startsWith("BYTE=")) {
                                                  int idx = tok.substring(5).toInt();
                                                  // ON= може йти наступним токеном; типово вмикаємо.
                                                  bool on = true;
                                                  if (tail.startsWith("ON=")) {
                                                      on = (tail.substring(3, 4) == "1");
                                                      int q2 = tail.indexOf(' ');
                                                      tail = (q2 < 0) ? String("") : tail.substring(q2 + 1);
                                                      tail.trim();
                                                  }
                                                  mirrorPlanTakeOne(g_mirPlan, idx, on);
                                              }
                                              // Значеннєві рядки — тим самим розбором: VAL= бере
                                              // наступний ON=, VSET= — наступний V=.
                                              else if (tok.startsWith("VAL=")) {
                                                  int idx = tok.substring(4).toInt();
                                                  bool on = true;
                                                  if (tail.startsWith("ON=")) {
                                                      on = (tail.substring(3, 4) == "1");
                                                      int q2 = tail.indexOf(' ');
                                                      tail = (q2 < 0) ? String("") : tail.substring(q2 + 1);
                                                      tail.trim();
                                                  }
                                                  mirrorValTake(g_mirPlan, idx, on);
                                              }
                                              else if (tok.startsWith("VSET=")) {
                                                  int idx = tok.substring(5).toInt();
                                                  long v = -1;
                                                  if (tail.startsWith("V=")) {
                                                      int q2 = tail.indexOf(' ');
                                                      v = ((q2 < 0) ? tail.substring(2) : tail.substring(2, q2)).toInt();
                                                      tail = (q2 < 0) ? String("") : tail.substring(q2 + 1);
                                                      tail.trim();
                                                  }
                                                  mirrorValSetUser(g_mirPlan, idx, v);
                                              }
                                          }
                                          sResp(mirrorPlanJson());
                                      }
                                  } }
    else if (cmd == "WIZARD")   { sResp(wizStart()); }
    // WIZSTEP <idx> [MODEL] [FIXES=…] [RATED=…] [RSENSE=…|RSMODEL=…] [MFG=…] [HEALTH=…] [USE=…] [CAL=…] [CYC=…] [NONIMP=…] [TODAY=…] [ETMSRC=USE|PACK]
    else if (cmd == "WIZSTEP")  { int s2 = arg.indexOf(' ');
                                  String si = (s2 < 0) ? arg : arg.substring(0, s2);
                                  String rest = (s2 < 0) ? String("") : arg.substring(s2 + 1);
                                  si.trim();
                                  SerRestoreArgs wa = serParseRestore(rest);
                                  sResp(wizExecStep(si.toInt(), wa.model, wa.fixes, wa.rated,
                                                    wa.rsense, wa.rsModel, wa.mfg, wa.tail,
                                                    wa.health, wa.useDate, wa.cal,
                                                    wa.cyc, wa.nonImp,
                                                    wa.today, wa.etmSrc)); }
    else if (cmd == "WIZRESET") { wizJournalClear(); sResp("{\"ok\":true}"); }
    else if (cmd == "WIZLIST")  { sResp(wizJournalListJson()); }
    else if (cmd == "WIZDEL")   { String s = arg; s.trim(); s.toUpperCase();
                                  if (!s.length()) sResp("{\"ok\":false,\"err\":\"no serial\"}");
                                  else { wizJournalDelete(s.c_str()); sResp("{\"ok\":true}"); } }
    // RECAL [DEEP] — DEEP додатково стирає навчені записи ємності й журнал.
    else if (cmd == "RECAL")    { String a = arg; a.trim(); a.toUpperCase();
                                  bool ok = performRecalPrepare(a == "DEEP");
                                  sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"read first / write failed\"}"); }
    else if (cmd == "REBOOT")   { displayShow("ПЕРЕЗАВАНТАЖЕННЯ"); sResp("{\"ok\":true}");
                                  g_serOut->flush(); Serial.flush(); delay(200); ESP.restart(); }
    else                          sResp(String("{\"ok\":false,\"err\":\"unknown cmd '") + cmd + "'\"}");
}

// Викликати в loop(): накопичує рядок і виконує команду по \n.
// Один прохід по каналу: добрати символи, і на кінці рядка виконати команду,
// перемкнувши відповідь на ЦЕЙ канал.
static void serialPump(Stream &io, String &buf, bool viaBt) {
    while (io.available()) {
        char c = (char)io.read();
        if (c == '\n' || c == '\r') {
            if (buf.length()) {
                Stream *prevOut = g_serOut; bool prevBt = g_serViaBt;
                g_serOut = &io; g_serViaBt = viaBt;
                serialExec(buf);
                g_serOut = prevOut; g_serViaBt = prevBt;
                buf = "";
            }
        } else {
            buf += c;
            if (buf.length() > 4200) buf = "";   // захист от переповнення
        }
    }
}

inline void serialTask() {
    serialPump(Serial, g_serIn, false);
#ifdef BT_ENABLED
    // Поки ніхто не під'єднаний, читати нема чого — і питати теж не варто.
    if (btUp() && SerialBT.hasClient()) serialPump(SerialBT, g_serInBt, true);
#endif
}

#endif
