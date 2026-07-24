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
//   TEMPLATES            -> список вшитих моделей для ініціалізації (без пароля)
//   INITBAT <MODEL> <мАг>-> ініціалізувати порожній чип як новий АКБ моделі
//   RECAL                -> підготовка до рекалібрування (після заміни елементів)
//   REBOOT               -> перезавантаження ESP32
//
// Пароль по USB — ОПЦІЙНИЙ: фізичний доступ до кабелю = дозвіл на запис, тож
// команди запису працюють і без "AUTH". "AUTH <пароль>" лише звіряє пароль і
// підсвічує статус у клієнті. (Мережевий веб-інтерфейс пароль вимагає.)
// ---------------------------------------------------------------------------

#include "web_server.h"   // dump-буфери, readAllChips/performReset/repairDumps,
                          // hexToBytes/fixHeaderChecksum/mirrorOk/headerChecksumOk

static String g_serIn;         // накопичувач вхідного рядка
static bool   g_serAuthed = false;  // чи авторизований клієнт (AUTH <пароль>)

static void sResp(const String &json) {
    Serial.print("#R#");
    Serial.println(json);
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
    j += ",\"has33\":" + String(hasDump ? "true" : "false");
    j += ",\"has38\":" + String(hasDump2438 ? "true" : "false");

    if (hasDump) {
        char model[24];
        String m = decodeModel(model, sizeof(model)) ? String(model) : String("");
        int cap = -1, wear = -1; decodeCapacity(&cap, &wear);
        const char *reason; bool genuine = batteryGenuine(&reason);
        j += ",\"model\":\"" + m + "\"";
        j += ",\"capacity\":" + String(cap);
        j += ",\"wear\":" + String(wear);
        j += ",\"genuine\":" + String(genuine ? "true" : "false");
        j += ",\"authReason\":\"" + String(reason) + "\"";
        j += ",\"headerOk\":" + String(headerChecksumOk(batteryDump) ? "true" : "false");
        j += ",\"mirrorOk\":" + String((hasDump2438 ? mirrorOk(batteryDump, batteryDump2438) : true) ? "true" : "false");
        j += ",\"hex33\":\"" + serHex(batteryDump, DUMP_SIZE) + "\"";
    }
    if (hasDump2438) {
        uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
        int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3;
        int16_t cur = (int16_t)((batteryDump2438[6] << 8) | batteryDump2438[5]);
        float i_mA = (float)cur / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f;
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
        j += ",\"icaMah\":" + String((int)(ica * DS2438_MAH_PER_LSB));
        j += ",\"ccaMah\":" + String((int)(cca * DS2438_MAH_PER_LSB));
        j += ",\"dcaMah\":" + String((int)(dca * DS2438_MAH_PER_LSB));
        j += ",\"ccaCycles\":" + String((int)(cca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH));
        j += ",\"dcaCycles\":" + String((int)(dca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH));
        j += ",\"ratedMah\":" + String((int)BATTERY_RATED_MAH);
        j += ",\"charge\":" + String(charge) + ",\"chargeSrc\":\"" + String(csrc) + "\"";
        j += ",\"serial\":\"" + serial + "\"";
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
    long ica = (long)(mah / DS2438_MAH_PER_LSB + 0.5f);
    if (ica < 0) ica = 0; if (ica > 255) ica = 255;
    batteryDump2438[12] = (uint8_t)ica;
    ledSet(LED_WRITE); displayShow("USB ЄМН mAh");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB mAh OK" : "USB mAh ЗБІЙ");
    sResp(ok ? (String("{\"ok\":true,\"ica\":") + ica + "}") : "{\"ok\":false,\"err\":\"write failed\"}");
}

static void serSetCap(const String &arg) {
    if (!hasDump) { sResp("{\"ok\":false,\"err\":\"read first\"}"); return; }
    int cap = arg.toInt(); if (cap < 0) cap = 0; if (cap > 100) cap = 100;
    int rec = -1;
    for (int i = 0x100; i < 0x1F0 - 23; i++)
        if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) { rec = i; break; }
    if (rec < 0) { sResp("{\"ok\":false,\"err\":\"no 0x17 record\"}"); return; }
    batteryDump[rec + 21] = (uint8_t)cap;
    fixRecordChecksum(batteryDump, rec, 23);
    ledSet(LED_WRITE); displayShow("USB ЄМН...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    ledSet(ok ? LED_OK : LED_ERROR); displayShow(ok ? "USB ЄМН OK" : "USB ЄМН ЗБІЙ");
    sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"write failed\"}");
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
    if (cmd == "AUTH") {
        g_serAuthed = (arg == ADMIN_PASSWORD);
        sResp(g_serAuthed ? "{\"ok\":true,\"authed\":true}"
                          : "{\"ok\":false,\"authed\":false,\"err\":\"невірний пароль\"}");
        return;
    }

    if (cmd == "PING")            sResp(String("{\"ok\":true,\"dev\":\"MotoBatteryReader\",\"ver\":3,\"needAuth\":false,\"authed\":") + (g_serAuthed ? "true" : "false") + "}");
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
                                         sResp(ok ? (String("{\"ok\":true,\"model\":\"") + md + "\"}")
                                                  : "{\"ok\":false,\"err\":\"збій запису\"}"); } }
    else if (cmd == "RECAL")    { bool ok = performRecalPrepare();
                                  sResp(ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"read first / write failed\"}"); }
    else if (cmd == "REBOOT")   { displayShow("ПЕРЕЗАВАНТАЖЕННЯ"); sResp("{\"ok\":true}"); Serial.flush(); delay(200); ESP.restart(); }
    else                          sResp(String("{\"ok\":false,\"err\":\"unknown cmd '") + cmd + "'\"}");
}

// Викликати в loop(): накопичує рядок і виконує команду по \n.
inline void serialTask() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_serIn.length()) { serialExec(g_serIn); g_serIn = ""; }
        } else {
            g_serIn += c;
            if (g_serIn.length() > 4200) g_serIn = "";   // захист от переповнення
        }
    }
}

#endif
