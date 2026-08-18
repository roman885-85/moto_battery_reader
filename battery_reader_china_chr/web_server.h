#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "battery_reader.h"
#include "settings.h"
#include "impres_format.h"     // структура прошивки IMPRES (єдине джерело правди)
#include "impres_bms.h"        // штатний декодер Motorola (лічильники, знос, дати)
#include "operations.h"        // єдиний каталог операцій для всіх поверхонь
#include "restore_plan.h"      // правки еталона під конкретний пакет перед записом
#include "device_clock.h"     // системна дата пристрою (джерело «сьогодні»)
#include "impres_audit.h"     // аудит змісту: шифрування й узгодженість даних
#include "impres_clone.h"     // крайній засіб: відновлення за зразком копії
#include "discharge.h"         // керований розряд навантаженням (MOSFET)
#include "charge.h"            // керований заряд понижувачем на власному ключі (PNP B772M)
#include "leds.h"
#include "display.h"
#include "templates.h"

// ⚑ Визначене у скетчі НИЖЧЕ за цей include, тож потрібне оголошення.
//  Без нього збірка падає на «resetReasonText was not declared» — і причина
//  була б неочевидна: функція ж «є», просто ще не видима в цій точці.
const char *resetReasonText(esp_reset_reason_t r);

extern WebServer server;
extern BatteryReader battery;
extern uint8_t batteryDump[DUMP_SIZE];
extern bool hasDump;
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern bool hasDump2438;
extern uint8_t chipSN2438[8];
extern bool hasSN2438;
extern uint8_t chipSN2433[8];
extern bool hasSN2433;

// Збереження дампа в SPIFFS (перезапис файлу).
static void saveDump(const char *path, const uint8_t *data, size_t size) {
    SPIFFS.remove(path);
    delay(50);
    File f = SPIFFS.open(path, "w");
    if (f) {
        size_t written = f.write(data, size);
        f.flush();
        f.close();
        delay(50);
        Serial.printf("Saved %s: %d bytes\n", path, written);
    } else {
        Serial.printf("ERROR: cannot open %s for writing\n", path);
    }
}

// HEX-превью перших n байт в JSON-рядок ("AA BB CC ...").
static String hexPreview(const uint8_t *data, size_t n) {
    String s;
    for (size_t i = 0; i < n; i++) {
        char hex[4];
        sprintf(hex, "%02X", data[i]);
        s += hex;
        if (i + 1 < n) s += " ";
    }
    return s;
}

// ---------------------------------------------------------------------------
// Цілісність прошивки IMPRES (з’ясовано аналізом дампів, див. README):
//   * Заголовок DS2433: сума байт 0x00..0x1F ≡ 0x41; байт 0x1F — контрольний.
//   * TLV-записи: сума усіх байт записи (разом з її контрольним байтом) ≡ 0x5A.
//   * Блок калібрування дзеркалиться: DS2438[24:50] == DS2433[1:27]. Він однаковий для
//     усіх батарей однієї моделі (4488A і 4493A збігаються; 4409A відрізняється) —
//     тобто прив'язки до серійному номеру чипа Немає, прошивка прив'язана до Моделі.
// Звідси механізм ремонту: перерахувати контрольну суму заголовка і
// синхронізувати дзеркало з уцілілого DS2438 у DS2433 (або навпаки).
// ---------------------------------------------------------------------------

// контрольна сума заголовка DS2433 (0x00..0x1F ≡ 0x41).
static void fixHeaderChecksum(uint8_t *d) {
    int s = 0;
    for (int i = 0; i < 0x1F; i++) s += d[i];
    d[0x1F] = (0x41 - s) & 0xFF;
}
static bool headerChecksumOk(const uint8_t *d) {
    int s = 0;
    for (int i = 0; i <= 0x1F; i++) s += d[i];
    return (s & 0xFF) == 0x41;
}

// Синхронізація дзеркала калібрування: DS2438[24:50] -> DS2433[1:27] (+ контр. сума
// заголовка). DS2438 переживає стирання DS2433, тому це основний шлях ремонту.
static void syncMirrorFrom2438(uint8_t *d33, const uint8_t *d38) {
    for (int i = 0; i < 26; i++) d33[1 + i] = d38[24 + i];
    fixHeaderChecksum(d33);
}
static bool mirrorOk(const uint8_t *d33, const uint8_t *d38) {
    for (int i = 0; i < 26; i++) if (d33[1 + i] != d38[24 + i]) return false;
    return true;
}
// Чи придатне дзеркало DS2438[24:50] як ДЖЕРЕЛО для відновлення DS2433[1:27]:
// лише якщо це реальні дані (не суцільні 0x00 чи 0xFF). У R7 (PMNN4809A/APLI4810C,
// формат 2021) дзеркала в DS2438 може не бути — тоді синхронізація НЕ виконується,
// інакше вона затерла б заголовок DS2433 нулями і зіпсувала ідентичність.
static bool mirrorSourceValid(const uint8_t *d38) {
    bool allZero = true, allFF = true;
    for (int i = 24; i < 50; i++) { if (d38[i] != 0x00) allZero = false; if (d38[i] != 0xFF) allFF = false; }
    return !allZero && !allFF;
}

// ---------------------------------------------------------------------------
// Перевірка пароля адміністратора. РАНІШЕ: `if (hasArg && arg != PW)` —
// якщо клієнт просто НЕ надсилав аргумент password, перевірка проходила і
// запис виконувався БЕЗ пароля (діра в безпеці). Тепер пароль ОБОВ'ЯЗКОВИЙ:
// має бути присутній І збігатися. Усі веб-обробники запису викликають
// requireAdmin() першим рядком.
static bool adminOk() { return true; }
// Пароль адміністратора ВИМКНЕНО: пристрій працює без нього (фізичний доступ до
// АКБ/пристрою вже є дозволом). requireAdmin() лишено як no-op, щоб не чіпати
// десятки місць виклику; воно завжди дозволяє.
static bool requireAdmin() { return true; }

// Логотип: віддаємо /logo.png з SPIFFS, якщо він завантажений (інакше 404 -> в вебі
// показується вбудований SVG-тризуб). Дозволяє використати точний логотип НГУ.
void handleLogo() {
    if (SPIFFS.exists("/logo.png")) {
        File f = SPIFFS.open("/logo.png", "r");
        server.streamFile(f, "image/png");
        f.close();
    } else {
        server.send(404, "text/plain", "no logo");
    }
}

// обробник головної сторінки
void handleRoot() {
    File file = SPIFFS.open("/index.html", "r");
    if (!file) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

// Читання обох мікросхем (DS2433 + DS2438) з збереженням в SPIFFS і на дисплей.
// Повертає true, якщо зчитана хоча б одна мікросхема.
bool readAllChips(bool &ok2433, bool &ok2438) {
    ledSet(LED_READ);
    displayShow("ЗЧИТУВАННЯ...");

    // Читаємо в ТИМЧАСОВІ буфери й застосовуємо лише при УСПІХУ: невдале читання
    // (напр. АКБ від'єднано) не повинно затирати попередній добрий дамп у пам'яті.
    static uint8_t tmp33[DUMP_SIZE];
    static uint8_t tmp38[DS2438_MEM_SIZE];
    memset(tmp33, 0, DUMP_SIZE);
    memset(tmp38, 0, DS2438_MEM_SIZE);

    // DS2433 — основний дамп (512 байт).
    ok2433 = battery.readBattery(tmp33, DUMP_SIZE);
    if (ok2433) {
        memcpy(batteryDump, tmp33, DUMP_SIZE);
        hasDump = true;
        saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    }

    // DS2438 — монітор батареї (64 байта).
    ok2438 = battery.readDS2438(tmp38, DS2438_MEM_SIZE);
    if (ok2438) {
        memcpy(batteryDump2438, tmp38, DS2438_MEM_SIZE);
        hasDump2438 = true;
        saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    }

    // Серійні номери чипів (лазерні ROM-ID). ROM DS2433 — це ще й ключ до
    // зашифрованих лічильників і дат у прошивці АКБ (див. impres_bms.h).
    // ⚑ Дзеркалимо стан драйвера ОБОМА гілками. Раніше «else» не було, і
    // серійник лишався від ПОПЕРЕДНЬОГО пакета, якщо цей не визначився. Для
    // показу це дрібниця, а для шифрування — ні: ROM DS2433 і є ключем, і
    // старий ROM означав би, що дані цього пакета зашифровано чужим.
    if (battery.hasRom2438()) {
        memcpy(chipSN2438, battery.rom2438(), 8);
        hasSN2438 = true;
    } else {
        hasSN2438 = false;
        memset(chipSN2438, 0, sizeof(chipSN2438));
    }
    if (battery.hasRom2433()) {
        memcpy(chipSN2433, battery.rom2433(), 8);
        hasSN2433 = true;
    } else {
        hasSN2433 = false;
        memset(chipSN2433, 0, sizeof(chipSN2433));
    }

    char st[40];
    if (ok2433 || ok2438) {
        snprintf(st, sizeof(st), "ЧИТ 2433:%s 2438:%s", ok2433 ? "OK" : "-", ok2438 ? "OK" : "-");
    } else {
        // ⚑ «Нема чіпа» — це ВИСНОВОК ПРО ПАКЕТ, і робити його, не перевіривши
        // власну обв'язку, не можна: коротке на землю в лінії enable/підтяжки
        // дає рівно ту саму картину. Перевіряємо лінію й називаємо винного.
        // ⚑ «Нема чіпа» — це висновок ПРО ПАКЕТ, і робити його, не перевіривши
        // власну обв'язку, не можна: відсутня підтяжка, коротке на землю чи
        // відсутня спільна земля дають рівно ту саму картину. Спершу питаємо
        // залізо, і лише якщо воно справне — звинувачуємо пакет.
        uint8_t enl = battery.enableLineCheck();
        uint8_t bus = battery.busLineCheck();
        if (bus == BatteryReader::BUS_NO_EXT_PULLUP) {
            snprintf(st, sizeof(st), "ПОМИЛКА: нема підтяжки 4.7к");
        } else if (bus == BatteryReader::BUS_NO_PULLUP) {
            snprintf(st, sizeof(st), "ПОМИЛКА: лінія даних (GPIO%d)", (int)DS_PIN);
        } else if (enl != BatteryReader::ENL_OK) {
            snprintf(st, sizeof(st), "ПОМИЛКА: enable (GPIO%d)", (int)PULLUP_PIN);
        } else {
            snprintf(st, sizeof(st), "ПОМИЛКА: нема чіпа");
        }
        if (enl != BatteryReader::ENL_OK)
            Serial.printf("ЧИТАННЯ: %s\n", BatteryReader::enableLineText(enl));
        if (bus != BatteryReader::BUS_OK)
            Serial.printf("ЧИТАННЯ: %s\n", BatteryReader::busLineText(bus));
    }
    displayShow(st);

    ledSet((ok2433 || ok2438) ? LED_OK : LED_ERROR);
    // Прогрес читання оновлював лише футер (без блимання). Тепер ОДИН повний
    // перемальовок — показати оновлені дані АКБ (%, напруга тощо) на дисплеї.
    displayRender();
    return ok2433 || ok2438;
}

// Обробник читання дампа: зчитуємо обидві мікросхеми (DS2433 + DS2438).
void handleReadDump() {
    Serial.println("Starting battery read...");

    bool ok2433, ok2438;
    readAllChips(ok2433, ok2438);

    if (ok2433 || ok2438) {
        String json = String("{\"status\":\"success\",\"ds2433\":") + (ok2433 ? "true" : "false") +
                      ",\"ds2438\":" + (ok2438 ? "true" : "false") + "}";
        server.send(200, "application/json", json);
    } else {
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read battery\"}");
    }
}

// Обробник завантаження дампа
void handleDownloadDump() {
    if (!hasDump) {
        server.send(404, "text/plain", "No dump available");
        return;
    }
    
    File file = SPIFFS.open("/dump.bin", "r");
    if (!file) {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }
    
    server.sendHeader("Content-Disposition", "attachment; filename=battery_dump.bin");
    server.streamFile(file, "application/octet-stream");
    file.close();
}

// Обробник завантаження дампа DS2438
void handleDownloadDump2438() {
    if (!hasDump2438) {
        server.send(404, "text/plain", "No DS2438 dump available");
        return;
    }

    File file = SPIFFS.open("/dump2438.bin", "r");
    if (!file) {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }

    server.sendHeader("Content-Disposition", "attachment; filename=ds2438_dump.bin");
    server.streamFile(file, "application/octet-stream");
    file.close();
}

// Upload-колбек (ufn) для файлу DS2438 -> /upload2438.bin
void handleUploadDump2438() {
    static File uploadFile;

    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("\n=== DS2438 upload started: %s ===\n", upload.filename.c_str());
        if (uploadFile) { uploadFile.close(); delay(20); }
        if (SPIFFS.exists("/upload2438.bin")) { SPIFFS.remove("/upload2438.bin"); delay(20); }
        uploadFile = SPIFFS.open("/upload2438.bin", "w");
        if (!uploadFile) Serial.println("CRITICAL ERROR: Cannot create /upload2438.bin!");

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);

    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.flush();
            uploadFile.close();
            delay(50);
            Serial.printf("DS2438 upload finished (%d bytes)\n", upload.totalSize);
        }

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) uploadFile.close();
        Serial.println("DS2438 upload aborted!");
    }
}

#ifdef DISPLAY_SPLASH_SPIFFS
#ifdef DISPLAY_SPLASH_JPEG
#include <TJpg_Decoder.h>   // приймальник сам питає розміри JPEG (getFsJpgSize),
                            // тож залежність його власна, а не успадкована
#endif
#include "splash.h"   // явно, а не транзитом через display.h: приймальник
                      // мусить розбирати заголовок ТИМ САМИМ кодом, що й
                      // дисплей, і залежати тут від чужого include не варто
// ── ЗАВАНТАЖЕННЯ КОЛЬОРОВОЇ ЗАСТАВКИ У SPIFFS ─────────────────────────────
//  Формат і перевірка — у splash.h (той самий код, що читає її дисплей і
//  ганяє хостовий тест). Тут — приймання й запис.
static bool     g_splashUpOpen = false;
static uint32_t g_splashUpBytes = 0;
static bool     g_splashUpOverflow = false;

void handleUploadSplash() {
    static File up;
    HTTPUpload &u = server.upload();

    if (u.status == UPLOAD_FILE_START) {
        Serial.printf("\n=== Splash upload started: %s ===\n", u.filename.c_str());
        if (up) { up.close(); delay(20); }
        // ⚑ ПИШЕМО У ТИМЧАСОВИЙ ФАЙЛ, а не одразу в бойовий. Приймання може
        //  обірватись посеред передачі, і затерти робочу заставку недописаним
        //  шматком означало б зламати те, що працювало, — при спробі, яка
        //  навіть не дійшла до кінця. Перейменуємо лише після перевірки.
        if (SPIFFS.exists(DISPLAY_SPLASH_PATH ".tmp")) SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
        up = SPIFFS.open(DISPLAY_SPLASH_PATH ".tmp", "w");
        g_splashUpOpen = (bool)up;
        g_splashUpBytes = 0;
        g_splashUpOverflow = false;
        if (!up) Serial.println("Splash: не вдалося створити тимчасовий файл");

    } else if (u.status == UPLOAD_FILE_WRITE) {
        // Стелю перевіряємо НА ХОДУ, а не наприкінці: інакше надто великий
        // файл спершу заповнив би SPIFFS (витіснивши index.html), і лише
        // потім був би відхилений.
        if (up && !g_splashUpOverflow) {
            if (g_splashUpBytes + u.currentSize > (uint32_t)DISPLAY_SPLASH_MAX_BYTES) {
                // (стеля сирого формату; для JPEG діє менша — перевіряється
                //  після приймання, коли вже відомий тип файла)
                g_splashUpOverflow = true;
                up.close(); up = File();
                SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
                Serial.println("Splash: файл більший за стелю — приймання припинено");
            } else {
                up.write(u.buf, u.currentSize);
                g_splashUpBytes += u.currentSize;
            }
        }

    } else if (u.status == UPLOAD_FILE_END) {
        if (up) { up.flush(); up.close(); delay(30); }
        Serial.printf("Splash upload finished (%lu bytes)\n", (unsigned long)g_splashUpBytes);

    } else if (u.status == UPLOAD_FILE_ABORTED) {
        if (up) up.close();
        SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
        Serial.println("Splash upload aborted!");
    }
}

void handleUploadSplashDone() {
    if (g_splashUpOverflow) {
        char m[160];
        snprintf(m, sizeof(m),
                 "{\"status\":\"error\",\"message\":\"Файл більший за стелю %lu байтів\"}",
                 (unsigned long)DISPLAY_SPLASH_MAX_BYTES);
        server.send(400, "application/json", m);
        return;
    }
    if (!SPIFFS.exists(DISPLAY_SPLASH_PATH ".tmp")) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Файл не прийнято\"}");
        return;
    }

    // Перевіряємо ТИМЧАСОВИЙ файл тим самим розбором, що й дисплей. Один код
    // на приймання й на показ — інакше рано чи пізно приймальник почне
    // пропускати те, чого дисплей не покаже.
    File f = SPIFFS.open(DISPLAY_SPLASH_PATH ".tmp", "r");
    uint8_t hdr[SPLASH_HDR_BYTES];
    size_t  nHdr = f ? f.read(hdr, sizeof(hdr)) : 0;
    size_t  sz   = f ? f.size() : 0;
    uint16_t w = 0, h = 0;
    // ⚑ ТИП — ЗА МАГІЄЮ, а не за розширенням чи полем форми: розширення
    //  бреше, перші байти — ні.
    int kind = splashSniff(hdr, nHdr);
    if (f) f.close();

#ifdef DISPLAY_SPLASH_JPEG
    if (kind == SPLASH_KIND_JPEG) {
        // Для JPEG діє власна, менша стеля: сенс формату — саме економія
        // місця, і файл на сотні кілобайтів означає або фотографію в повній
        // роздільності, або взагалі не те, що мали на увазі.
        if (sz > (size_t)DISPLAY_SPLASH_JPG_MAX_BYTES) {
            SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
            char m[180];
            snprintf(m, sizeof(m),
                     "{\"status\":\"error\",\"message\":\"JPEG більший за %lu КБ — стисніть сильніше\"}",
                     (unsigned long)DISPLAY_SPLASH_JPG_MAX_BYTES / 1024UL);
            server.send(400, "application/json", m);
            return;
        }
        // Розміри питаємо в декодера, не декодуючи кадр цілком. Це ще й
        // перевірка «а чи це справді JPEG»: магія збігтись може випадково,
        // а розібрати заголовок сміття декодер не зможе.
        uint16_t jw = 0, jh = 0;
        if (TJpgDec.getFsJpgSize(&jw, &jh, DISPLAY_SPLASH_PATH ".tmp", SPIFFS) != JDR_OK) {
            SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
            server.send(400, "application/json",
                        "{\"status\":\"error\",\"message\":\"Файл починається як JPEG, але декодер його не розібрав\"}");
            return;
        }
        // ⚑ ЗАВЕЛИКЕ НЕ ВІДХИЛЯЄМО — пристрій зменшить сам, кратно 2, зі
        //  збереженням пропорцій (splashJpegScaleFor у splash.h). Відмова
        //  лишається тільки там, де не рятує навіть /8.
        uint8_t sc = splashJpegScaleFor(jw, jh, TFT_W, TFT_H);
        if (!sc) {
            SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
            char m[240];
            snprintf(m, sizeof(m),
                     "{\"status\":\"error\",\"message\":\"JPEG %ux%u більший за екран %dx%d понад увосьмеро — "
                     "зменшити кратно 2 вже не вийде. Зменште зображення заздалегідь\"}",
                     jw, jh, (int)TFT_W, (int)TFT_H);
            server.send(400, "application/json", m);
            return;
        }
        // Приймаємо. Інший формат прибираємо: два файли поруч означали б, що
        // «поточна заставка» — питання порядку перевірок, а не вибору людини.
        if (SPIFFS.exists(DISPLAY_SPLASH_JPG_PATH)) SPIFFS.remove(DISPLAY_SPLASH_JPG_PATH);
        if (!SPIFFS.rename(DISPLAY_SPLASH_PATH ".tmp", DISPLAY_SPLASH_JPG_PATH)) {
            SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
            server.send(500, "application/json",
                        "{\"status\":\"error\",\"message\":\"Не вдалося зберегти у SPIFFS\"}");
            return;
        }
        if (SPIFFS.exists(DISPLAY_SPLASH_PATH)) SPIFFS.remove(DISPLAY_SPLASH_PATH);
        uint16_t dw = splashScaled(jw, sc), dh = splashScaled(jh, sc);
        Serial.printf("Splash JPEG збережено: %ux%u, %u байтів%s\n", jw, jh, (unsigned)sz,
                      sc == 1 ? "" : " (буде зменшено при показі)");
        char m[300];
        snprintf(m, sizeof(m),
                 "{\"status\":\"success\",\"kind\":\"jpeg\",\"w\":%u,\"h\":%u,\"scale\":%u,\"bytes\":%u,"
                 "\"message\":\"JPEG %ux%u збережено%s — буде видно при наступному запуску\"}",
                 dw, dh, sc, (unsigned)sz, jw, jh,
                 sc == 1 ? "" : " і буде зменшено до розміру екрана (пропорції збережено)");
        server.send(200, "application/json", m);
        return;
    }
#else
    if (kind == SPLASH_KIND_JPEG) {
        SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Ця прошивка зібрана без підтримки JPEG "
                    "(DISPLAY_SPLASH_JPEG). Надішліть підготовлений .bin\"}");
        return;
    }
#endif

    int rc = splashParse(hdr, nHdr, sz, TFT_W, TFT_H, &w, &h);
    if (rc != SPLASH_OK) {
        SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
        char m[220];
        snprintf(m, sizeof(m),
                 "{\"status\":\"error\",\"message\":\"%s (екран %dx%d)\"}",
                 splashErrText(rc), (int)TFT_W, (int)TFT_H);
        Serial.printf("Splash ВІДХИЛЕНО: %s (%u байтів)\n", splashErrText(rc), (unsigned)sz);
        server.send(400, "application/json", m);
        return;
    }

    // Аж тепер підміняємо бойовий файл — і прибираємо інший формат, щоб
    // «поточна заставка» не залежала від порядку перевірок при показі.
    if (SPIFFS.exists(DISPLAY_SPLASH_PATH)) SPIFFS.remove(DISPLAY_SPLASH_PATH);
#ifdef DISPLAY_SPLASH_JPEG
    if (SPIFFS.exists(DISPLAY_SPLASH_JPG_PATH)) SPIFFS.remove(DISPLAY_SPLASH_JPG_PATH);
#endif
    if (!SPIFFS.rename(DISPLAY_SPLASH_PATH ".tmp", DISPLAY_SPLASH_PATH)) {
        SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
        server.send(500, "application/json",
                    "{\"status\":\"error\",\"message\":\"Не вдалося зберегти у SPIFFS\"}");
        return;
    }

    Serial.printf("Splash збережено: %ux%u, %u байтів\n", w, h, (unsigned)sz);
    char m[200];
    snprintf(m, sizeof(m),
             "{\"status\":\"success\",\"w\":%u,\"h\":%u,\"bytes\":%u,"
             "\"message\":\"Заставку збережено — буде видно при наступному запуску\"}",
             w, h, (unsigned)sz);
    server.send(200, "application/json", m);
}

// Стан заставки: чи є файл, які розміри, скільки місця лишилось.
void handleSplashInfo() {
    uint16_t w = 0, h = 0; size_t sz = 0; int rc = SPLASH_ERR_SHORT;
    const char *kind = "none";
    bool present = false;
#ifdef DISPLAY_SPLASH_JPEG
    if (SPIFFS.exists(DISPLAY_SPLASH_JPG_PATH)) {
        present = true; kind = "jpeg";
        File jf = SPIFFS.open(DISPLAY_SPLASH_JPG_PATH, "r");
        if (jf) { sz = jf.size(); jf.close(); }
        uint16_t jw = 0, jh = 0;
        uint8_t sc = 0;
        if (TJpgDec.getFsJpgSize(&jw, &jh, DISPLAY_SPLASH_JPG_PATH, SPIFFS) == JDR_OK &&
            (sc = splashJpegScaleFor(jw, jh, TFT_W, TFT_H)) != 0) {
            w = splashScaled(jw, sc); h = splashScaled(jh, sc); rc = SPLASH_OK;
        } else rc = SPLASH_ERR_MAGIC;
    } else
#endif
    if (SPIFFS.exists(DISPLAY_SPLASH_PATH)) {
        present = true; kind = "raw";
        File f = SPIFFS.open(DISPLAY_SPLASH_PATH, "r");
        if (f) {
            uint8_t hdr[SPLASH_HDR_BYTES];
            size_t n = f.read(hdr, sizeof(hdr));
            sz = f.size();
            rc = splashParse(hdr, n, sz, TFT_W, TFT_H, &w, &h);
            f.close();
        }
    }
    size_t total = SPIFFS.totalBytes(), used = SPIFFS.usedBytes();
#ifdef DISPLAY_SPLASH_JPEG
    const bool jpegOk = true;
    const unsigned long jpgMax = (unsigned long)DISPLAY_SPLASH_JPG_MAX_BYTES;
#else
    const bool jpegOk = false;
    const unsigned long jpgMax = 0;
#endif
    char j[420];
    snprintf(j, sizeof(j),
             "{\"present\":%s,\"ok\":%s,\"kind\":\"%s\",\"jpeg\":%s,"
             "\"w\":%u,\"h\":%u,\"bytes\":%u,"
             "\"panelW\":%d,\"panelH\":%d,\"maxBytes\":%lu,\"jpgMaxBytes\":%lu,"
             "\"fsFree\":%lu,\"err\":\"%s\"}",
             present ? "true" : "false",
             rc == SPLASH_OK ? "true" : "false",
             kind, jpegOk ? "true" : "false",
             w, h, (unsigned)sz, (int)TFT_W, (int)TFT_H,
             (unsigned long)DISPLAY_SPLASH_MAX_BYTES, jpgMax,
             (unsigned long)(total > used ? total - used : 0),
             rc == SPLASH_OK ? "" : splashErrText(rc));
    server.send(200, "application/json", j);
}

// Прибрати завантажену заставку — повернутись до типової.
void handleSplashDelete() {
    bool had = SPIFFS.exists(DISPLAY_SPLASH_PATH);
    if (had) SPIFFS.remove(DISPLAY_SPLASH_PATH);
#ifdef DISPLAY_SPLASH_JPEG
    if (SPIFFS.exists(DISPLAY_SPLASH_JPG_PATH)) { SPIFFS.remove(DISPLAY_SPLASH_JPG_PATH); had = true; }
#endif
    SPIFFS.remove(DISPLAY_SPLASH_PATH ".tmp");
    server.send(200, "application/json",
                had ? "{\"status\":\"success\",\"message\":\"Заставку прибрано — буде типова\"}"
                    : "{\"status\":\"success\",\"message\":\"Завантаженої заставки не було\"}");
}
#endif  // DISPLAY_SPLASH_SPIFFS

// обробник запиту /upload2438 (fn): надсилає відповідь після приймання файлу.
void handleUploadDone2438() {
    if (SPIFFS.exists("/upload2438.bin")) {
        File file = SPIFFS.open("/upload2438.bin", "r");
        size_t size = file ? file.size() : 0;
        if (file) file.close();

        if (size == DS2438_MEM_SIZE) {
            server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"File uploaded\"}");
            return;
        }
        Serial.printf("DS2438 upload size mismatch: %d bytes (expected %d)\n", size, DS2438_MEM_SIZE);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Upload failed\"}");
}

// Обробник записи дампа в DS2438
void handleWriteDump2438() {
    if (!requireAdmin()) return;

    Serial.println("\n=== DS2438 write request received ===");

    if (!SPIFFS.exists("/upload2438.bin")) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }

    File file = SPIFFS.open("/upload2438.bin", "r");
    if (!file) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }

    size_t fileSize = file.size();
    if (fileSize != DS2438_MEM_SIZE) {
        file.close();
        Serial.printf("DS2438 invalid file size: %d (expected %d)\n", fileSize, DS2438_MEM_SIZE);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }

    uint8_t buffer[DS2438_MEM_SIZE];
    memset(buffer, 0, DS2438_MEM_SIZE);
    file.seek(0);
    size_t bytesRead = file.read(buffer, DS2438_MEM_SIZE);
    file.close();

    if (bytesRead != DS2438_MEM_SIZE) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read uploaded file\"}");
        return;
    }

    Serial.println("Writing to DS2438 chip...");
    ledSet(LED_WRITE);
    displayShow("ЗАПИС 2438...");
    if (battery.writeDS2438(buffer, DS2438_MEM_SIZE)) {
        memcpy(batteryDump2438, buffer, DS2438_MEM_SIZE);
        hasDump2438 = true;
        saveDump("/dump2438.bin", buffer, DS2438_MEM_SIZE);

        Serial.println("✓✓✓ DS2438 WRITE SUCCESSFUL ✓✓✓");
        displayShow("2438 ЗАПИС OK");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"DS2438 written successfully\"}");

        ledSet(LED_OK);
    } else {
        Serial.println("✗✗✗ DS2438 WRITE FAILED ✗✗✗");
        displayShow("2438 ЗАПИС ЗБІЙ");
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write DS2438\"}");
        ledSet(LED_ERROR);
    }
    Serial.println("=== DS2438 write request completed ===\n");
}

// Інформація о DS2438: превью + розшифровані напруга/температура (стр. 0).
void handleDumpInfo2438() {
    if (!hasDump2438) {
        server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"No dump available\"}");
        return;
    }

    // Сторінка 0: [1]=Temp LSB, [2]=Temp MSB, [3]=V LSB, [4]=V MSB, [5]=I LSB, [6]=I MSB
    uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
    float voltage = vraw * 0.01f; // 10 мВ/LSb

    int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3; // 13-біт
    float temperature = traw * 0.03125f; // °C/LSb

    int16_t current = (int16_t)((batteryDump2438[6] << 8) | batteryDump2438[5]); // сире значення

    // ⚑ Шунт беремо З ЧИПА (DS2438[56..57]), а не з константи. У пакетів
    // PMNN4409A/B він ~45.2..46.1 мОм, у 4488A/4493A/4809A ~24.9..25.6 мОм —
    // тобто єдина константа 0.025 Ом занижувала опір і завищувала струм і
    // мА·год майже вдвічі на всій родині 4409. Перевірено проти показань
    // фірмового ПЗ Motorola (див. impres_bms.h).
    const ImpresBms &bms = impresBmsOf(hasDump ? batteryDump : nullptr,
                                       batteryDump2438,
                                       hasSN2433 ? chipSN2433 : nullptr,
                                       DS2438_RSENSE_OHM);
    float rs = bms.rsense > 0.0f ? bms.rsense : DS2438_RSENSE_OHM;

    float    i_mA = (float)current / (4096.0f * rs) * 1000.0f;
    uint8_t  ica  = batteryDump2438[12];
    uint16_t cca  = ((uint16_t)batteryDump2438[61] << 8) | batteryDump2438[60];
    uint16_t dca  = ((uint16_t)batteryDump2438[63] << 8) | batteryDump2438[62];

    const char *csrc;
    int charge = batteryPercent(&csrc);

    String serial = "";
    if (hasSN2438) {
        char b[3];
        for (int i = 0; i < 8; i++) { sprintf(b, "%02X", chipSN2438[i]); serial += b; }
    }

    String json = "{\"size\":64,\"hasData\":true";
    json += ",\"voltage\":" + String(voltage, 2);
    json += ",\"temperature\":" + String(temperature, 1);
    json += ",\"currentRaw\":" + String(current);
    json += ",\"currentMa\":" + String(i_mA, 0);
    json += ",\"ica\":" + String(ica);
    json += ",\"cca\":" + String(cca);
    json += ",\"dca\":" + String(dca);
    // Паспортна ємність — ЗА МОДЕЛЛЮ (таблиця IMPRES_RATED в impres_format.h),
    // а не одна константа на всі АКБ. Через єдину BATTERY_RATED_MAH=2500 цикли
    // й залишок у мА·год не збігалися з тим, що показує станція, на всіх
    // моделях, крім однієї.
    char rmModel[16] = "";
    decodeModel(rmModel, sizeof(rmModel));
    int ratedMah = impresRatedMahFor(hasDump ? batteryDump : nullptr, rmModel);
    json += ",\"icaMah\":" + String(impresIcaToMahRs((uint8_t)ica, ratedMah, rs));
    // Ціна розряду CCA/DCA — 15.625 мВ·год, а НЕ 0.4882 як у ICA (даташит
    // DS2438). Раніше тут стояла константа ICA, і накопичений заряд виходив
    // у 32 рази меншим за дійсний.
    json += ",\"ccaMah\":" + String(bms.ccaMah);
    json += ",\"dcaMah\":" + String(bms.dcaMah);
    json += ",\"ccaCycles\":" + String((int)(bms.ccaMah / ratedMah));
    json += ",\"dcaCycles\":" + String((int)(bms.dcaMah / ratedMah));
    json += ",\"rsense\":" + String(rs, 5);
    json += ",\"rsenseChip\":" + String(bms.rsenseFromChip ? 1 : 0);
    json += ",\"ratedMah\":" + String(ratedMah);
    json += ",\"charge\":" + String(charge);
    json += ",\"chargeSrc\":\"" + String(csrc) + "\"";
    // Шкала «заряд за напругою» — з пристрою, щоб клієнти не тримали власних
    // копій чисел і не брехали в підписах після зміни BATTERY_EMPTY_MV.
    json += ",\"emptyMv\":" + String(BATTERY_EMPTY_MV);
    json += ",\"fullMv\":"  + String(BATTERY_FULL_MV);
    json += ",\"scaleTxt\":\"" BATTERY_SCALE_TXT "\"";
    // ETM (DS2438[8..11], сек наробітку). Рація показує «дату першого користування»
    // як (свій поточний час − ETM) — перевірено діффом до/після калібрування.
    uint32_t etm = ((uint32_t)batteryDump2438[11] << 24) | ((uint32_t)batteryDump2438[10] << 16) |
                   ((uint32_t)batteryDump2438[9] << 8) | batteryDump2438[8];
    json += ",\"etmSec\":" + String(etm);
    json += ",\"serial\":\"" + serial + "\"";
    if (hasSN2433) {
        char b[3]; String s33 = "";
        for (int i = 0; i < 8; i++) { sprintf(b, "%02X", chipSN2433[i]); s33 += b; }
        json += ",\"serial33\":\"" + s33 + "\"";
    }
    // ── штатні поля Motorola (impres_bms.h) ────────────────────────────────
    //  cycles     — цикли заряду IMPRES; рахуються з гістограми і ключа НЕ
    //               потребують: саме це число показує фірмове ПЗ.
    //  решта      — зашифровані; ключ або з ROM DS2433, або підібраний.
    if (bms.ok) {
        json += ",\"bms\":{\"kit\":\"" + String(bms.kit) + "\"";
        json += ",\"cycles\":" + String(bms.cycles);
        json += ",\"nonImpresCycles\":" + String(bms.nonImpresCycles);
        json += ",\"haveKey\":" + String(bms.haveKey ? 1 : 0);
        json += ",\"keyGuessed\":" + String(bms.keyGuessed ? 1 : 0);
        if (bms.haveKey) {
            json += ",\"health\":" + String(bms.health);
            json += ",\"potentialMah\":" + String(bms.potentialMah);
            json += ",\"firstUseMah\":" + String(bms.firstUseMah);
            json += ",\"cyclesEnc\":" + String(bms.cyclesEnc);
            json += ",\"calCycles\":" + String(bms.calCycles);
            json += ",\"reverts\":" + String(bms.reverts);
            json += ",\"topOffCycles\":" + String(bms.topOffCycles);
            char d[12];
            snprintf(d, sizeof(d), "%04d-%02d-%02d", bms.mfgY, bms.mfgM, bms.mfgD);
            json += ",\"mfgDate\":\"" + String(d) + "\"";
            if (bms.useY) {
                snprintf(d, sizeof(d), "%04d-%02d-%02d", bms.useY, bms.useM, bms.useD);
                json += ",\"firstUseDate\":\"" + String(d) + "\"";
            }
        }
        json += "}";
    }
    json += ",\"preview\":\"" + hexPreview(batteryDump2438, 16) + "\"";
    json += ",\"hex\":\"" + hexPreview(batteryDump2438, 64) + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

// обробник завантаження файлу - перероблений для коректної роботи з ESP32 WebServer
void handleUploadDump() {
    static File uploadFile;
    static size_t uploadedBytes = 0;
    
    HTTPUpload &upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("\n=== Upload started: %s ===\n", upload.filename.c_str());
        
        // Перевіряємо вільне місце
        size_t totalBytes = SPIFFS.totalBytes();
        size_t usedBytes = SPIFFS.usedBytes();
        size_t freeBytes = totalBytes - usedBytes;
        
        Serial.printf("SPIFFS Status: Total=%d, Used=%d, Free=%d\n", totalBytes, usedBytes, freeBytes);
        
        // Закриваємо старий файл якщо він еще відкритий
        if (uploadFile) {
            uploadFile.close();
            delay(50);
        }
        
        // Видаляємо старий файл
        if (SPIFFS.exists("/upload.bin")) {
            SPIFFS.remove("/upload.bin");
            delay(100);
        }
        
        // Відкриваємо новий файл
        uploadFile = SPIFFS.open("/upload.bin", "w");
        if (!uploadFile) {
            Serial.println("CRITICAL ERROR: Cannot create /upload.bin!");
            return;
        }
        
        uploadedBytes = 0;
        Serial.println("Upload file opened");
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!uploadFile) {
            Serial.println("ERROR: Upload file not open!");
            return;
        }
        
        // Пишемо дані в файл
        size_t written = uploadFile.write(upload.buf, upload.currentSize);
        uploadedBytes += written;
        
        Serial.printf("Chunk received: %d bytes, written: %d bytes (total: %d)\n", 
                     upload.currentSize, written, uploadedBytes);
        
        if (written != upload.currentSize) {
            Serial.printf("ERROR: Write mismatch! Expected %d, wrote %d\n", 
                         upload.currentSize, written);
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.flush();
            uploadFile.close();
            delay(200);  // Критична затримка для синхронізації SPIFFS
            
            Serial.printf("Upload finished: %s (%d bytes total)\n", 
                         upload.filename.c_str(), uploadedBytes);
            
            // Перевіряємо результат
            delay(100);
            if (SPIFFS.exists("/upload.bin")) {
                File file = SPIFFS.open("/upload.bin", "r");
                if (file) {
                    size_t size = file.size();
                    Serial.printf("✓ File created: %d bytes\n", size);
                    
                    // Перевіряємо перші байти
                    uint8_t header[16];
                    file.seek(0);
                    size_t read = file.read(header, 16);
                    Serial.printf("Header (%d bytes): ", read);
                    for (int i = 0; i < read; i++) {
                        Serial.printf("%02X ", header[i]);
                    }
                    Serial.println();
                    
                    file.close();
                } else {
                    Serial.println("✗ Cannot open /upload.bin for verification");
                }
            } else {
                Serial.println("✗ CRITICAL: File not found in SPIFFS!");
            }
            
            Serial.println("=== Upload completed ===\n");
        }
        // Відповідь надсилає handleUploadDone() (обробник запиту), а не upload-колбек.

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) {
            uploadFile.close();
        }
        Serial.println("Upload aborted!");
    }
}

// обробник запиту /upload (fn): викликається після того, як upload-колбек
// (ufn) повністю прийняв тело multipart-форми. Надсилає HTTP-відповідь.
void handleUploadDone() {
    if (SPIFFS.exists("/upload.bin")) {
        File file = SPIFFS.open("/upload.bin", "r");
        size_t size = file ? file.size() : 0;
        if (file) file.close();

        if (size == DUMP_SIZE) {
            server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"File uploaded\"}");
            return;
        }
        Serial.printf("Upload size mismatch: %d bytes (expected %d)\n", size, DUMP_SIZE);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Upload failed\"}");
}

// Обробник записи дампа
void handleWriteDump() {
    // Перевіряємо пароль
    if (!requireAdmin()) return;
    
    Serial.println("\n=== Write request received ===");
    
    // Перевіряємо наявність файлу
    if (!SPIFFS.exists("/upload.bin")) {
        Serial.println("✗ /upload.bin does not exist - upload a file first");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    // Відкриваємо і перевіряємо розмір
    File file = SPIFFS.open("/upload.bin", "r");
    if (!file) {
        Serial.println("✗ Cannot open /upload.bin");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    size_t fileSize = file.size();
    Serial.printf("File size: %d bytes (expected: %d bytes)\n", fileSize, DUMP_SIZE);
    
    if (fileSize != DUMP_SIZE) {
        file.close();
        Serial.printf("✗ Invalid file size\n");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    
    // Читаємо весь файл в буфер
    uint8_t buffer[DUMP_SIZE];
    memset(buffer, 0, DUMP_SIZE);
    
    file.seek(0);
    size_t bytesRead = file.read(buffer, DUMP_SIZE);
    file.close();
    
    Serial.printf("Bytes read: %d\n", bytesRead);
    
    if (bytesRead != DUMP_SIZE) {
        Serial.printf("✗ Read mismatch! Expected %d, got %d\n", DUMP_SIZE, bytesRead);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read uploaded file\"}");
        return;
    }
    
    // Виводимо перші байти
    Serial.printf("Data: ");
    for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();
    
    // Пишемо в батарею
    Serial.println("Writing to battery chip...");
    ledSet(LED_WRITE);
    displayShow("ЗАПИС 2433...");
    if (battery.writeBattery(buffer, DUMP_SIZE)) {
        memcpy(batteryDump, buffer, DUMP_SIZE);
        hasDump = true;
        
        // Зберігаємо як поточний дамп
        SPIFFS.remove("/dump.bin");
        delay(50);
        File dumpFile = SPIFFS.open("/dump.bin", "w");
        if (dumpFile) {
            size_t written = dumpFile.write(buffer, DUMP_SIZE);
            dumpFile.flush();
            dumpFile.close();
            delay(50);
            Serial.printf("Current dump saved: %d bytes\n", written);
        }
        
        Serial.println("✓✓✓ WRITE SUCCESSFUL ✓✓✓");
        displayShow("2433 ЗАПИС OK");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Firmware written successfully\"}");
        
        // Індикація успіху
        ledSet(LED_OK);
    } else {
        Serial.println("✗✗✗ WRITE FAILED ✗✗✗");
        displayShow("2433 ЗАПИС ЗБІЙ");
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write battery\"}");
        ledSet(LED_ERROR);
    }
    Serial.println("=== Write request completed ===\n");
}

// Обробник інформації о дампі
void handleDumpInfo() {
    if (!hasDump) {
        server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"No dump available\"}");
        return;
    }
    
    char model[24];
    String modelStr = decodeModel(model, sizeof(model)) ? String(model) : String("");
    int cap = -1, wear = -1;
    decodeCapacity(&cap, &wear);

    const char *reason;
    bool genuine = batteryGenuine(&reason);

    bool hdrOk = headerChecksumOk(batteryDump);
    bool mirOk = hasDump2438 ? mirrorOk(batteryDump, batteryDump2438) : true;

    String json = "{\"size\":512,\"hasData\":true";
    json += ",\"model\":\"" + modelStr + "\"";
    json += ",\"capacity\":" + String(cap);
    json += ",\"wear\":" + String(wear);
    json += ",\"genuine\":" + String(genuine ? "true" : "false");
    json += ",\"authReason\":\"" + String(reason) + "\"";
    json += ",\"headerOk\":" + String(hdrOk ? "true" : "false");
    json += ",\"mirrorOk\":" + String(mirOk ? "true" : "false");
    // Друга 32-байтна сума (блок профілю моделі) і запис COPYRIGHT — раніше не
    // перевірялись узагалі. copyright: "ok" / "broken" / "none" (немає — це
    // норма для 4409A та APLI4810C, вони його штатно не мають).
    json += ",\"profileOk\":" + String(impresProfileOk(batteryDump) ? "true" : "false");
    json += ",\"copyright\":\"" + String(!impresHasCopyright(batteryDump) ? "none"
                                       : impresRecordOk(batteryDump, IMPRES_COPYRIGHT) ? "ok"
                                       : "broken") + "\"";
    // Стан навченого калібрувального хвоста 0x18A..0x1FF — ключове поле для
    // ремонту після заміни елементів (див. impres_format.h).
    { int t = impresTailState(batteryDump);
      json += ",\"tail\":\"" + String(t == IMPRES_TAIL_BLANK ? "blank"
                                    : t == IMPRES_TAIL_FRESH ? "fresh"
                                    : t == IMPRES_TAIL_VALID ? "learned" : "broken") + "\""; }
    json += ",\"preview\":\"";
    
    json += hexPreview(batteryDump, 16);
    json += "\",\"hex\":\"" + hexPreview(batteryDump, DUMP_SIZE) + "\"}";

    server.send(200, "application/json", json);
}

// Скидання лічильників використання / зносу для рекалібрування на оригінальної ЗУ.
// Обнуляє РЕАЛЬНІ лічильники використання: CCA/DCA/ETM у DS2438 і скидає
// відображувану ємність (0x17) на 100% (знос 0). Контрольні суми зачеплених
// записів перераховуються (Σ==0x5A).
//
// ⚠️ ВАЖЛИВО: запис 0x0D у DS2433 НЕ чіпаємо. Раніше тут обнулялись байти
// [+4..+7] запису 0x0D як «дзеркало CCA/DCA» — це була ПОМИЛКА: звірка з
// еталоном PMNN4409A показала, що там лежить КАЛІБРУВАННЯ ЄМНОСТІ
// (напр. 0x09B7=2487 ≈ мА·год), а не лічильники (у відкаліброваного дампа з
// CCA=2310 там було 6, тобто це не CCA). Обнуління ламало калібрування —
// рація казала «невідома», а зарядка бачила «заряджений» і не заряджала.
//
// ⚑ І ГОЛОВНЕ — СКИДАННЯ ТЕПЕР УЗГОДЖЕНЕ МІЖ ДВОМА ЧИПАМИ.
//  Скарга власника: «повністю робочий акумулятор після обнулення лічильників,
//  напрацювання, вироблення перестає бачитись як оригінальний». Причина була
//  саме тут. Ми обнуляли лічильники ЛИШЕ в DS2438, а DS2433 лишався як був —
//  із датою першого користування, з гістограмою на сотні циклів, із
//  лічильником калібрувань. Пакет після цього розповідав про себе дві
//  несумісні речі одночасно:
//
//      DS2433: «мене вмикали тоді-то, я пройшов 1097 циклів»
//      DS2438: «я не пропрацював жодної секунди й не прийняв жодного мА·год»
//
//  Такого пакета не буває. У корпусі dumps/ з 52 цілих фірмових ETM дорівнює
//  нулю рівно у ДВОХ — обидва з 08-nova-batareya, тобто саме з того випадку,
//  коли рація сказала «невідомий акумулятор»; у решти 50 напрацювання — сотні
//  мільйонів секунд. Провал 08 списували на чужий навчений хвіст, і це теж
//  правда, але дефекту там два, і другий — оцей.
//
//  Тепер обнуляємо ОБИДВА боки або жоден: історію в DS2433 скидає
//  impresHistoryZero() під ключем ЦЬОГО чипа (impres_crypt.h), зберігаючи дату
//  виготовлення — вона справжня й одна-єдина, затирати її нема за що.
//
//  ⚠️ Без ROM-ID чипа зашифровану історію переписати нічим. Тоді не чіпаємо
//  НІЧОГО: пакет із просто старими лічильниками нікому не шкодить, а пакет із
//  розсинхронізованими чипами рація відкидає. Повертає false — викликач мусить
//  сказати про це користувачеві, а не вдавати успіх.
bool resetBatteryData() {
    // Спершу з'ясовуємо, чи зможемо привести до ладу DS2433: якщо ні, монітор
    // теж лишається недоторканим. Порядок саме такий, щоб не було стану
    // «монітор уже обнулили, а на DS2433 не вистачило ключа».
    bool can33 = hasDump && hasSN2433;
    if (hasDump && !can33) {
        Serial.println("RESET: ROM-ID DS2433 невідомий — історію в DS2433 переписати "
                       "нічим. Лічильники НЕ чіпаємо: обнулений монітор проти повного "
                       "історії DS2433 — це стан, який рація читає як «невідомий АКБ».");
        return false;
    }
    // Відмова тут означає, що гістограму циклів обнулити не вдалось (блок не
    // тієї довжини). Монітор у такому разі теж не чіпаємо — інакше вийде та
    // сама суперечність, лише з іншого боку.
    if (can33 && !impresHistoryZero(batteryDump, chipSN2433)) {
        Serial.println("RESET: історію в DS2433 обнулити не вдалось (блок гістограми "
                       "не тієї довжини). Монітор НЕ чіпаємо — розсинхронізований "
                       "пакет гірший за пакет зі старими лічильниками.");
        return false;
    }
    if (hasDump2438) {
        for (int i = 8; i <= 11; i++) batteryDump2438[i] = 0; // ETM (таймер)
        batteryDump2438[60] = batteryDump2438[61] = 0;         // CCA
        batteryDump2438[62] = batteryDump2438[63] = 0;         // DCA
    }
    return true;
    // ⚠️ Раніше тут «ставили здоров'я 100%»: у першому записі довжини 0x17
    // байту +21 присвоювали 0x64 і перераховували суму. Це було двічі хибно.
    // По-перше, запис шукали як «тег 0x17 + 0x00», хоча перший байт — довжина.
    // По-друге, запис @0x129 — ЗАВОДСЬКА таблиця: у всіх 19 екземплярів
    // PMNN4409A і всіх 8 екземплярів PMNN4409B у dumps/ вона побайтово
    // ОДНАКОВА, тобто це не поточне здоров'я АКБ, а константа моделі, і
    // правити її не можна й немає сенсу. Реальне «здоров'я/строк служби»
    // у прошивці не зберігається — його рахує рація (див. impres_format.h).
}

// Ядро скидання: редагує дампи, пише в обидві мікросхеми, зберігає. Без HTTP —
// викликається і з веб-обробника, і з меню на дисплеї (по кнопкам).
bool performReset() {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }

    Serial.println("\n=== Battery reset (recalibration) ===");
    ledSet(LED_WRITE);
    displayShow("СКИДАННЯ...");
    // Відмова тут — НЕ дрібниця, яку можна проковтнути: скидання без ключа
    // лишило б чипи розсинхронізованими, а це рівно те, через що пакет
    // перестає впізнаватись. Краще нічого не записати й сказати чому.
    if (!resetBatteryData()) {
        displayShow("НЕМА ROM 2433");
        ledSet(LED_ERROR);
        Serial.println("=== Reset ABORTED: ROM-ID DS2433 невідомий ===\n");
        return false;
    }

    bool ok = true;
    if (hasDump)     ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
    if (hasDump2438) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);

    if (ok) {
        if (hasDump)     saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        if (hasDump2438) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        displayShow("СКИД. OK");
    } else {
        displayShow("СКИД. ЗБІЙ");
    }
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Reset completed ===\n");
    return ok;
}

// ------------------- Очистка (крім критичних даних) -------------------

// Очистка "до заводського": скидає лічильники використання (CCA/DCA/ETM) і
// здоров'я на 100%, зберігаючи КРИТИЧНЕ (модель, ID, криву калібрування, блок
// автент., дзеркало DS2438↔DS2433, заголовок).
//
// ⚠️ ВАЖЛИВО: записи 0x16 більше НЕ обнуляємо. Звірка з еталоном PMNN4409A
// показала, що перший запис 0x16 (тег "16 00") містить КАЛІБРУВАЛЬНУ таблицю
// (86 00 6C 00 56 00 …), а не «статистику». Її обнуління ламало АКБ (рація
// «невідома»). Тепер очистка = безпечне скидання лічильників, без правки
// калібрувальних записів.
bool factoryCleanData() {
    // Лічильники в ОБОХ чипах або в жодному (див. resetBatteryData).
    bool ok = resetBatteryData();
    if (hasDump) fixHeaderChecksum(batteryDump);
    return ok;
}

// Стерти НАВЧЕНИЙ калібрувальний хвіст DS2433 (0x18A..0x1FF) — донорські/старі
// дані про РЕАЛЬНІ банки, які після заміни елементів суперечать новому пакету.
//
// ⚠️ ЩО БУЛО НЕ ТАК РАНІШЕ. Тут шукали «записи 0x0A»: байт 0x0A, після якого
// 10 байт дають суму 0x5A. Це помилка на двох рівнях:
//   • перший байт запису — це ДОВЖИНА, а не тег, тож «0x0A» = будь-який запис
//     довжини 10, а не якийсь особливий «learned»-запис;
//   • на 500 зсувах випадкове вікно дає суму 0x5A приблизно двічі на дамп, тож
//     функція стирала по 10 байт у ВИПАДКОВИХ місцях (у т.ч. у журналі та в
//     заводських таблицях), псуючи прошивку замість ремонту.
// Плюс зона стирання (0x180..кінець) не збігалася з реальною межею навчених
// даних, а другий прохід (0x68..0x140) заходив на заводські таблиці.
//
// Тепер стираємо РІВНО регіон 0x18A..0x1FF — межу визначено побайтовим
// порівнянням 49 дампів (нижче 0x18A лежать ідентичність, крива, copyright,
// заводська таблиця й запис моделі; вище — тільки навчене калібрування).
// Повертає к-сть змінених байт.
// Знайти «чистий» хвіст для моделі, зчитаної з АКБ. nullptr — вибірки дампів
// для цієї моделі бракує, робити ремонт наосліп не можна.
static const uint8_t *freshTailForDump(char *modelOut, size_t n) {
    if (modelOut && n) modelOut[0] = '\0';
    if (!hasDump) return nullptr;
    char m[16] = "";
    if (!impresModelName(batteryDump, m, sizeof(m))) return nullptr;
    if (modelOut && n) { strncpy(modelOut, m, n - 1); modelOut[n - 1] = '\0'; }
    int t = findTemplate(m);
    return (t < 0) ? nullptr : BATTERY_TEMPLATES[t].fresh;
}

// Записати ЧИСТИЙ хвіст у буфер дампа. -1 — немає перевіреного шаблону.
static int applyFreshTail(const uint8_t *fresh) {
    if (!fresh) return -1;
    static uint8_t buf[IMPRES_FRESH_TAIL_LEN];
    memcpy_P(buf, fresh, IMPRES_FRESH_TAIL_LEN);
    return impresResetTailFrom(batteryDump, buf);
}

// ПОВНЕ стирання DS2433 (КРАЙНІЙ ВИПАДОК). Заповнює всі 512 байт 0xFF (стан
// стертого EEPROM) — чіп стає "чистим". Стирає ВСЕ, включно з моделлю/ID/
// калібруванням DS2433! Після цього АКБ не працюватиме, доки не запишете
// еталонний дамп тієї ж моделі. DS2438 (його калібрування/дзеркало) НЕ чіпаємо
// — з нього потім можна відновити калібрувальний блок ("Ремонт").
bool performWipe2433() {
    static uint8_t blank[DUMP_SIZE];
    memset(blank, 0xFF, DUMP_SIZE);
    Serial.println("\n=== FULL WIPE DS2433 ===");
    ledSet(LED_WRITE); displayShow("СТИРАННЯ 2433..");
    bool ok = battery.writeBattery(blank, DUMP_SIZE);
    if (ok) {
        memcpy(batteryDump, blank, DUMP_SIZE); hasDump = true;
        saveDump("/dump.bin", blank, DUMP_SIZE);
        displayShow("2433 СТЕРТО");
    } else displayShow("СТИР. ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Wipe completed ===\n");
    return ok;
}

// ПОВНЕ стирання DS2438 (64 Б -> 0xFF). Крайній випадок: чіп-монітор у
// незрозумілому стані. Після цього дзеркало калібрування (DS2438[24:50]) теж
// стерте, тож АКБ треба відновити («Новий АКБ» або запис еталона).
bool performWipe2438() {
    static uint8_t blank[DS2438_MEM_SIZE];
    memset(blank, 0xFF, DS2438_MEM_SIZE);
    Serial.println("\n=== FULL WIPE DS2438 ===");
    ledSet(LED_WRITE); displayShow("СТИРАННЯ 2438..");
    bool ok = battery.writeDS2438(blank, DS2438_MEM_SIZE);
    if (ok) {
        memcpy(batteryDump2438, blank, DS2438_MEM_SIZE); hasDump2438 = true;
        saveDump("/dump2438.bin", blank, DS2438_MEM_SIZE);
        displayShow("2438 СТЕРТО");
    } else displayShow("СТИР. ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Wipe 2438 completed ===\n");
    return ok;
}

bool performFactoryClean() {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }
    Serial.println("\n=== Factory clean (keep identity) ===");
    ledSet(LED_WRITE); displayShow("ОЧИСТКА...");
    // Те саме, що й у скиданні: без ключа DS2433 узгодженої очистки не вийде,
    // а неузгоджена — це і є «пакет перестав впізнаватись».
    if (!factoryCleanData()) {
        displayShow("НЕМА ROM 2433");
        ledSet(LED_ERROR);
        Serial.println("=== Clean ABORTED: ROM-ID DS2433 невідомий ===\n");
        return false;
    }
    bool ok = true;
    if (hasDump)     ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
    if (hasDump2438) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) {
        if (hasDump)     saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        if (hasDump2438) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        displayShow("ОЧИСТКА OK");
    } else displayShow("ОЧИСТКА ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Clean completed ===\n");
    return ok;
}

// Підготовка до РЕКАЛІБРУВАННЯ (після заміни елементів). Не чіпає ідентичність/
// модель/криву — лише прибирає СТАРЕ learned-калібрування і обнуляє лічильники,
// щоб пакет став «валідним, але не відкаліброваним»: рація приймає («потребує
// відновлення»), а IMPRES-ЗП запускає цикл калібрування нових банок.
//   1) DS2433: стерти НАВЧЕНИЙ ХВІСТ 0x18A..0x1FF (→0xFF);
//   2) DS2438: обнулити лічильники (ETM/CCA/DCA), зберігши конфіг, апаратний
//      OFFSET АЦП і дзеркало; паливомір виставити за фактичною напругою.
//
// Межа 0x18A і сам механізм підтверджені дампами з dumps/:
//   • 13-dozaryadka-na-stantsii/01 — хвіст стерто до кінця чипа: рація показала
//     «IMPRES(tm)», рівень заряду вірний, ЗП і заряджає, і калібрує;
//   • 11-chastkove-stirannya, 09-4409a-z-proshyvkoyu-4809a — хвіст побитий:
//     ЗП теж приймає АКБ і йде в калібрування;
//   • 08-nova-batareya — записано ПОВНИЙ еталон разом із ЧУЖИМ хвостом:
//     «невідомий акумулятор», ЗП лише світить зеленим.
// Тобто рація/ЗП відкидають не «зіпсовану», а САМЕ ЧУЖУ навчену калібровку;
// порожній хвіст читається як «пакет фірмовий, але не калібрований».
//
// ⚠️ Фізичну калібровку це НЕ замінює — далі АКБ обов'язково калібрувати на ЗП.
// deep = true — додатково стерти навчені записи ЄМНОСТІ (0x153..0x189) і
// кільцевий журнал використання. Потрібно, коли після звичайної підготовки ЗП
// усе ще тримається за стару ємність. Модель, крива й copyright не чіпаються
// НІКОЛИ (інакше рація каже «нема моделі» — саме це сталося в експерименті
// власника зі стиранням від 0x130).
bool performRecalPrepare(bool deep) {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }
    Serial.println("\n=== Prepare for recalibration ===");
    ledSet(LED_WRITE); displayShow("ПІД КАЛІБР...");
    bool ok = true;

    if (hasDump) {
        // 1) Записати ЧИСТИЙ хвіст: скелет записів і сталі моделі лишаються,
        //    навчені значення обнулені, суми правильні.
        //
        //    ⚠️ Раніше тут хвіст СТИРАВСЯ у 0xFF. Рація такий пакет приймала,
        //    але калібрування щоразу падало в помилку: ЗП пише навчені значення
        //    за фіксованими адресами й НЕ створює структуру записів заново.
        //    У dumps/13-dozaryadka-na-stantsii після повного циклу на стертому
        //    хвості з'явилися рівно два байти (0x1E1, 0x1E2), а байт довжини
        //    0x1E0 лишився 0xFF — навчений блок не міг стати валідним ніколи.
        char mdl[16];
        const uint8_t *fresh = freshTailForDump(mdl, sizeof(mdl));
        int changed = applyFreshTail(fresh);
        if (changed < 0) {
            Serial.printf("no verified fresh tail for model '%s'\n", mdl[0] ? mdl : "?");
            displayShow("НЕМА ШАБЛОНУ");
            ledSet(LED_ERROR);
            return false;                          // краще нічого, ніж наосліп
        }
        int deepCleared = 0;
        if (deep) {
            deepCleared += impresEraseLearnedCapacity(batteryDump);
            deepCleared += impresEraseUsageLog(batteryDump);
        }
        Serial.printf("fresh tail written for %s: %d B changed, deep: %d B\n",
                      mdl, changed, deepCleared);
        impresFixHeader(batteryDump);               // заголовок поза хвостом — узгоджуємо про запас
        ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
        if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    }

    // 2) DS2438: обнулити ЛІЧИЛЬНИКИ, не стираючи монітор.
    //
    // ⚠️ Раніше сюди заливали суцільний 0xFF. Це не «скидання», а псування
    // монітора: байт статусу/конфігу отримував 0xFF замість 0x0F (виставлені
    // зарезервовані біти), поріг — 0xFF замість 0x40, і затиралися апаратний
    // OFFSET АЦП струму та дзеркало ідентичності. Симптом збігається з тим, що
    // описував власник: ЗП бачить АКБ, світить зеленим і не заряджає.
    if (hasDump2438) {
        // Паливомір ставимо за фактичною напругою — щоб ЗП стартувала з
        // осмисленого рівня, а не з нуля на зарядженому пакеті.
        int pct = impresPercentFromMv(impresVoltageMv(batteryDump2438));
        char rcM[16] = ""; decodeModel(rcM, sizeof(rcM));
        impresResetMonitor(batteryDump2438, hasDump ? batteryDump : nullptr,
                           impresIcaFromPercentRs(pct,
                               impresRatedMahFor(hasDump ? batteryDump : nullptr, rcM),
                               impresBmsRsense(batteryDump2438)));
        Serial.printf("monitor reset, ICA from %d%% (U=%u mV)\n",
                      pct, (unsigned)impresVoltageMv(batteryDump2438));
        bool o38 = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
        if (o38) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        ok &= o38;
    }

    displayShow(ok ? "ГОТ. ДО КАЛІБР" : "КАЛІБР ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Recal-prepare completed ===\n");
    return ok;
}
inline bool performRecalPrepare() { return performRecalPrepare(false); }

// ===========================================================================
//  КЕРОВАНИЙ РОЗРЯД — опитування й запобіжники (тут, бо потрібен battery/дампи)
// ===========================================================================

// Витримка на встановлення режиму ключа перед зняттям показань. Замість
// delay(): у циклі крутимо ledTask(), щоб індикація не залежала від того, скільки
// триває цикл вимірювання. Головну роботу тут робить апаратне згасання (див.
// leds.h) — воно взагалі не потребує процесора, — але перевертання півхвилі
// все одно має статися вчасно, інакше на краю хвилі з'явиться зайва пауза.
// Веб-сервер тут НЕ опитуємо свідомо: обробник міг би запустити ще одну
// операцію просто посеред циклу вимірювання.
static void dischargeSettle(unsigned long ms) {
    unsigned long t0 = millis();
    // ⚑ ГОДУЄМО ОБИДВА СТОРОЖІ. Функцію кличе не лише розряд: chargeTask()
    //  бере її ж на витримку перед виміром напруги. dischargeWatchdogFeed()
    //  мовчки нічого не робить, коли розряд не йде, — тобто під час ЗАРЯДУ ця
    //  пауза лишалась негодованою. Зараз вона коротка (10 мс) і до порога
    //  сторожа не дотягує, але це збіг обставин, а не властивість коду:
    //  варто комусь підняти CHARGE_VSENSE_SETTLE_MS — і пристрій почав би
    //  перезавантажуватись саме на вимірі, тобто в найменш очікуваному місці.
    while (millis() - t0 < ms) {
        ledTask();
        dischargeWatchdogFeed();
        chargeWatchdogFeed();
        delay(1);
    }
}

// Зняти показання монітора під навантаженням. true — читання вдалось.
static bool dischargeSample(uint16_t *mv, int16_t *ma, int16_t *tC10) {
    static uint8_t buf[DS2438_MEM_SIZE];
    if (!battery.readDS2438(buf, DS2438_MEM_SIZE)) return false;
    memcpy(batteryDump2438, buf, DS2438_MEM_SIZE);
    hasDump2438 = true;
    *mv = impresVoltageMv(buf);
    // Струм зі знаком; при розряді від'ємний. Формула та сама, що в /api/info2438.
    // ⚑ Шунт — із самого чипа (DS2438[56..57]). Це не косметика: уставка ШІМ
    // тримає САМЕ виміряний струм, і з чужою константою (0.025 Ом на пакеті,
    // де шунт 0.046 Ом) реальний струм був би вдвічі меншим за задану уставку.
    int16_t raw = (int16_t)((buf[6] << 8) | buf[5]);
    float rs = impresBmsRsense(buf);
    if (rs <= 0.0f) rs = DS2438_RSENSE_OHM;
    *ma  = (int16_t)((float)raw / (4096.0f * rs) * 1000.0f);
    *tC10 = (int16_t)((((int16_t)((buf[2] << 8) | buf[1])) >> 3) * 0.3125f);  // 0.03125*10
    return true;
}

// Старт розряду. targetMv — до якої напруги (0 = DISCHARGE_TARGET_MV).
// Повертає nullptr при успіху, інакше — текст причини відмови.
const char *dischargeStart(uint16_t targetMv) {
    if (!dischargeAvailable()) return "Розряд не налаштовано: задайте LOAD_PIN у settings.h";
    if (dischargeRunning())    return "Розряд уже виконується";
    if (chargeRunning())       return "Спочатку зупиніть заряд";
    if (!targetMv) targetMv = DISCHARGE_TARGET_MV;

    // Ціль обирає користувач, тож затискаємо її в межі, де розряд узагалі має
    // сенс: нижче — гарантована аварійна відсічка, вище — пакет і так там.
    if (targetMv < DISCHARGE_TARGET_MIN_MV) return "Цільова напруга нижча за допустиму (6.80 В)";
    if (targetMv > DISCHARGE_TARGET_MAX_MV) return "Цільова напруга вища за допустиму (8.00 В)";
    if (targetMv < DISCHARGE_HARD_MIN_MV + 200) return "Цільова напруга нижча за аварійну межу";

    uint16_t mv; int16_t ma, t;
    if (!dischargeSample(&mv, &ma, &t)) {
        loadOff();
        return "DS2438 не читається — розряд наосліп заборонено";
    }
    if (mv <= targetMv)                     return "Пакет уже розряджений до цілі";
    if (mv <= DISCHARGE_HARD_MIN_MV)        return "Напруга нижче аварійної межі";
    if (t >= DISCHARGE_MAX_TEMP_C * 10)     return "Пакет гарячий — дайте охолонути";

    memset(&g_dis, 0, sizeof(g_dis));
    g_dis.state    = DIS_RUN;
    g_dis.reason   = DISR_NONE;
    g_dis.targetMv = targetMv;
    g_dis.startMv  = g_dis.lastMv = mv;
    g_dis.lastMa   = ma;
    g_dis.lastTempC10 = t;
    g_dis.startMs  = g_dis.lastPollMs = millis();
    g_dis.startDca = g_dis.lastDca = impresDca(batteryDump2438);
    g_dis.startIca = g_dis.lastIca = batteryDump2438[12];   // паливомір на старті

    // ⚑ ENABLE — ПЕРШИМ, ще до навантаження. Пін PULLUP_PIN активує сам АКБ, і в
    // штатному режимі піднімається лише на час транзакції 1-Wire. Без утримання
    // пакет між читаннями неактивний: струм тече тільки в моменти опитування, і
    // розряд фактично не йде (саме це й спостерігалось).
    battery.holdEnable(true);
    // Шунт саме цього пакета — від нього залежить і струм, і облік мА·год.
    g_dis.rsense = impresBmsRsense(batteryDump2438);
    dischargeWatchdog(true);            // сторож — лише на час розряду

    // Початкова шпаруватість — З РОЗРАХУНКУ, не 100 %. Інакше перші 5 секунд
    // (до першого виміру піка) пакет тягнув би повні 1.4..1.7 А, тобто рівно те,
    // від чого ми йдемо. Оцінка піка за законом Ома завищена, тож розрахована
    // шпаруватість свідомо занижена — стартуємо м'якше, ніж треба, а перший
    // вимір це виправить.
    g_dis.setMa   = dischargeSetpointMa(mv, g_dis.targetMv);
    g_dis.peakMa  = (uint16_t)dischargeExpectedMa(mv);
    g_dis.dutyPct = dischargeDutyFor(g_dis.peakMa, g_dis.setMa);
    loadDuty(g_dis.dutyPct);

    ledSet(LED_DISCHARGE);
    // ⚑ Прибрати ПІДСУМОК попереднього заряду, якщо його ще не зняли кнопкою.
    // Дисплей і обробник кнопок покладаються на те, що активний екран рівно
    // один («заряд і розряд не можуть іти одночасно» — так і написано в
    // displayRender()). Але «екран активний» — це не «операція йде»: підсумок
    // висить, доки користувач його не прибере. Незнятий підсумок заряду
    // перекривав щойно запущений розряд: сторінка розряду не з'являлась, а
    // перше натискання з'їдалось на прибирання чужого підсумку.
    chargeDismiss();                       // не діє, якщо заряд справді йде
    dischargeMarkDirty(2);                 // увійшли в режим -> повна перемальовка
    Serial.printf("\n=== Discharge started: %u -> %u mV, setpoint %u mA, "
                  "duty %u%% (peak est. %u mA%s) ===\n",
                  mv, targetMv, g_dis.setMa, g_dis.dutyPct, g_dis.peakMa,
                  dischargePwmOk() ? "" : ", PWM UNAVAILABLE");
    return nullptr;
}

// Викликати часто з loop(). Реальна робота — раз на DISCHARGE_POLL_MS.
inline void dischargeTask() {
    if (g_dis.state != DIS_RUN) return;
    unsigned long now = millis();

    // Стеля тривалості: якщо розряд «не йде» (обрив навантаження, залиплий
    // MOSFET, поганий контакт) — зупиняємось, а не крутимось нескінченно.
    if ((now - g_dis.startMs) / 60000UL >= (unsigned long)DISCHARGE_MAX_MIN) {
        dischargeStop(DISR_TIMEOUT);
        Serial.println("=== Discharge ABORT: timeout ===");
        return;
    }
    dischargeWatchdogFeed();          // цикл живий — сторож спокійний
    if (now - g_dis.lastPollMs < DISCHARGE_POLL_MS) return;

    unsigned long dtMs = now - g_dis.lastPollMs;
    g_dis.lastPollMs = now;

    // ── СТОРОЖ (програмна половина) ────────────────────────────────────────
    //  Ми тут, отже цикл живий. Але якщо від попереднього опитування минуло
    //  надто багато, десь була довга затримка, під час якої ключ лишався
    //  відкритим БЕЗ НАГЛЯДУ. Це стан, коли розряд треба припинити, а не
    //  «надолужити»: скільки пакет віддав за цей час, ми не бачили, і скільки
    //  ще протримається — теж. Ключ і enable знімає dischargeStop().
    if (dtMs > DISCHARGE_STALL_MS) {
        dischargeStop(DISR_STALL);
        Serial.printf("=== Discharge ABORT: main loop stalled for %lu ms ===\n", dtMs);
        return;
    }
    g_dis.elapsedS   = (now - g_dis.startMs) / 1000UL;

    // Інтеграл ємності за інтервал, що ЩОЙНО минув. Рахуємо ДО оновлення стану:
    // весь цей час діяли попередні пік і шпаруватість, тобто тік середній струм
    // pick*duty, а не той, який ми зараз виміряємо.
    g_dis.mahX1000 += ((uint32_t)dischargeAvgMa(g_dis) * dtMs) / 3600UL;

    // --- крок 1: НАПРУГА і температура при ЗНЯТОМУ навантаженні --------------
    //  Міряти напругу під струмом не можна: просадка на внутрішньому опорі тим
    //  більша, чим розрядженіший пакет, і саме вона передчасно «продавлювала» б
    //  напругу до цілі — розряд обривався б, не добравши ємності. Тому на час
    //  виміру ключ закривається. 80 мс, звісно, не дають повної релаксації, але
    //  прибирають миттєву омічну просадку — головну складову — і, головне,
    //  роблять вимір однаковим від початку до кінця розряду.
    uint16_t mv = 0; int16_t ma = 0, t = 0;
    loadOff();
    dischargeSettle(DISCHARGE_PEAK_SETTLE_MS);
    bool okV = dischargeSample(&mv, &ma, &t);

    // --- крок 2: ПІК струму при повністю відкритому ключі --------------------
    //  DS2438 віддає струм останнього перетворення (~27 мс) і нічого не знає про
    //  ШІМ: під шпаруватістю невідомо, у яку фазу те перетворення попало. Тому
    //  міряємо в однозначній точці — на 100 % — і отримуємо ПІК, з якого вже
    //  однозначно рахується все інше.
    uint16_t mvLoaded = 0; int16_t peakRaw = 0, tLoaded = 0;
    loadFull();
    dischargeSettle(DISCHARGE_PEAK_SETTLE_MS);
    bool okI = dischargeSample(&mvLoaded, &peakRaw, &tLoaded);
    // Ключ НЕГАЙНО назад у робочу шпаруватість — повністю відкритим його не
    // лишаємо ні на мить довше, ніж триває вимір.
    loadDuty(g_dis.dutyPct);

    if (!okV || !okI) {
        // Кілька невдач поспіль — зупинка. Продовжувати розряд, не бачачи
        // напруги, не можна: так пакет саджається в нуль.
        if (++g_dis.readFails >= DISCHARGE_MAX_READ_FAILS) {
            dischargeStop(DISR_NOREAD);
            Serial.println("=== Discharge ABORT: DS2438 unreadable ===");
        }
        return;
    }
    g_dis.readFails = 0;
    g_dis.polls++;

    // --- крок 3: перерахунок уставки і шпаруватості --------------------------
    uint16_t peak = (uint16_t)(peakRaw < 0 ? -peakRaw : peakRaw);
    int      est  = dischargeExpectedMa(mv);
    // Захист від «датчик мовчить» (облік струму в DS2438 вимкнено, IAD=0 —
    // тоді регістр струму завжди 0). Повірити нулю означало б виставити 100 %
    // шпаруватості й розряджати повним струмом саме тоді, коли ми його не
    // бачимо. Тому неправдоподібно малий пік замінюємо оцінкою за законом Ома:
    // вона завищена, отже шпаруватість вийде занижена — помиляємось у безпечний бік.
    if (peak < (uint16_t)(est / 4)) peak = (uint16_t)est;

    g_dis.peakMa  = peak;
    g_dis.setMa   = dischargeSetpointMa(mv, g_dis.targetMv);
    g_dis.dutyPct = dischargeDutyFor(peak, g_dis.setMa);
    loadDuty(g_dis.dutyPct);

    g_dis.lastMv = mv;
    // lastMa — СЕРЕДНІЙ струм навантаження (пік * шпаруватість), зі знаком «-»
    // як у розряду. Саме він тече між вимірами, саме він потрібен для потужності
    // й для ємності; сам пік показуємо окремо.
    g_dis.lastMa = (int16_t)(-(int32_t)dischargeAvgMa(g_dis));
    g_dis.lastTempC10 = t;
    // Лічильники ВБУДОВАНОГО датчика струму: DCA інтегрує розряд апаратно,
    // ICA — поточний паливомір. Обидва читаються з того ж кадру DS2438.
    g_dis.lastDca = impresDca(batteryDump2438);
    g_dis.lastIca = batteryDump2438[12];

    // sag — просадка напруги під ПОВНИМ струмом (різниця між кроками 1 і 2).
    // Це фактично внутрішній опір пакета: sag/peak. Різке зростання за час
    // розряду — ознака поганої пайки або слабкої банки, тож у журнал воно варте
    // більше, ніж здається. (Струм і температура кроку 1 нам не потрібні:
    // під знятим навантаженням струм ~0, а температуру беремо з того ж кроку.)
    (void)ma; (void)tLoaded;
    int sag = (int)mv - (int)mvLoaded;
    // Слід у чорний ящик і тренд пам'яті — так само, як у заряду (postmortem.h).
    pmStep(PM_STEP_REPORT);
    pmNote(PM_MODE_DISCHARGE, g_dis.polls, g_dis.dutyPct, g_dis.lastMa, mv);

    Serial.printf("discharge: %u mV (sag %d mV), avg %d mA (peak %u, set %u, duty %u%%), "
                  "%.1f W, %.1f C, %lu mAh (DCA %lu), ICA %u, %lus, "
                  "купа %lu/%lu, стек %lu\n",
                  mv, sag, g_dis.lastMa, g_dis.peakMa, g_dis.setMa, g_dis.dutyPct,
                  dischargeWattsX10(mv, g_dis.lastMa) / 10.0f, t / 10.0f,
                  (unsigned long)dischargeMah(), (unsigned long)dischargeDcaMah(),
                  g_dis.lastIca, (unsigned long)g_dis.elapsedS,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                  (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    dischargeMarkDirty(1);                 // нові показання -> оновити екран

    if (mv <= DISCHARGE_HARD_MIN_MV) {
        dischargeStop(DISR_HARD_MIN);
        Serial.println("=== Discharge ABORT: below hard minimum ===");
    } else if (t >= DISCHARGE_MAX_TEMP_C * 10) {
        dischargeStop(DISR_TEMP);
        Serial.println("=== Discharge ABORT: overheat ===");
    } else if (mv <= g_dis.targetMv) {
        dischargeStop(DISR_TARGET);
        Serial.printf("=== Discharge DONE: %lu mAh in %lus ===\n",
                      (unsigned long)dischargeMah(), (unsigned long)g_dis.elapsedS);
    }
}

// Стан розряду у JSON — для веб-моніторингу й USB-клієнта.
static String dischargeJson() {
    // ⚑ ОДНА ВИДІЛЕНА ДІЛЯНКА ЗАМІСТЬ СОТЕНЬ ПЕРЕВИДІЛЕНЬ. Цей відповідач
    //  кличеться КОЖНУ СЕКУНДУ, поки клієнт дивиться на заряд/розряд, а нижче
    //  йдуть десятки `j += ...`. Рядок Arduino росте блоками по 16 байтів, тож
    //  без reserve() кожна відповідь — це десятки realloc (malloc+memcpy+free)
    //  плюс тимчасовий String на КОЖНЕ число.
    //
    //  Само по собі це не помилка, але за години заряду виходять СОТНІ ТИСЯЧ
    //  розподілів різного розміру — а це фрагментація купи. Вільної пам'яті
    //  начебто вистачає, а суцільного шматка під буфер Wi-Fi/TCP уже немає, і
    //  черговий розподіл падає. Виглядає це як «під час заряду пристрій
    //  періодично перезавантажується» — тобто рівно як скарга.
    //
    //  reserve() робить із цього ОДИН розподіл. Розмір узято з запасом над
    //  реальною довжиною відповіді; якщо не влізе, String просто дорощується
    //  як і раніше, тобто гірше не стане.
    String j; j.reserve(1024);
    j = "{\"available\":" + String(dischargeAvailable() ? "true" : "false");
    j += ",\"state\":\"" + String(g_dis.state == DIS_RUN ? "run"
                               : g_dis.state == DIS_DONE ? "done"
                               : g_dis.state == DIS_ABORT ? "abort" : "idle") + "\"";
    j += ",\"reason\":\""; j += dischargeReasonText(g_dis.reason); j += "\"";
    j += ",\"targetMv\":" + String(g_dis.targetMv);
    j += ",\"startMv\":"  + String(g_dis.startMv);
    j += ",\"mv\":"       + String(g_dis.lastMv);
    j += ",\"ma\":"       + String(g_dis.lastMa);
    j += ",\"tempC\":"    + String(g_dis.lastTempC10 / 10.0f, 1);
    j += ",\"mah\":"      + String((unsigned long)dischargeMah());
    // Дані вбудованого датчика струму DS2438
    j += ",\"watts\":"    + String(dischargeWattsX10(g_dis.lastMv, g_dis.lastMa) / 10.0f, 1);
    j += ",\"dcaMah\":"   + String((unsigned long)dischargeDcaMah());
    j += ",\"dcaDelta\":" + String((int)(uint16_t)(g_dis.lastDca - g_dis.startDca));
    j += ",\"ica\":"      + String(g_dis.lastIca);
    j += ",\"icaStart\":" + String(g_dis.startIca);
    j += ",\"elapsedS\":" + String((unsigned long)g_dis.elapsedS);
    j += ",\"polls\":"    + String(g_dis.polls);
    j += ",\"expectedMa\":" + String(dischargeExpectedMa(g_dis.lastMv));
    j += ",\"loadOhm\":"  + String(LOAD_OHM, 1);
    // Обмеження струму ШІМом: уставка за напругою, виміряний пік, шпаруватість.
    j += ",\"setMa\":"    + String(g_dis.setMa);
    j += ",\"peakMa\":"   + String(g_dis.peakMa);
    j += ",\"duty\":"     + String(g_dis.dutyPct);
    j += ",\"inBand\":"   + String(dischargeInBand(g_dis) ? "true" : "false");
    j += ",\"bandLoMa\":" + String((uint32_t)g_dis.setMa * DISCHARGE_BAND_LO_PCT / 100u);
    j += ",\"bandHiMa\":" + String((uint32_t)g_dis.setMa * DISCHARGE_BAND_HI_PCT / 100u);
    j += ",\"pwm\":"      + String(dischargePwmOk() ? "true" : "false");
    // Межі лінійки струму — щоб інтерфейси описували її, а не зашивали числа.
    j += ",\"rampHiMv\":" + String(DISCHARGE_RAMP_HI_MV);
    j += ",\"maHi\":"     + String(DISCHARGE_MA_HI);
    j += ",\"maLo\":"     + String(DISCHARGE_MA_LO);
    j += ",\"tgtMinMv\":" + String(DISCHARGE_TARGET_MIN_MV);
    j += ",\"tgtMaxMv\":" + String(DISCHARGE_TARGET_MAX_MV);
    j += ",\"tgtDefMv\":" + String(DISCHARGE_TARGET_MV);
    j += "}";
    return j;
}

// ===========================================================================
//  КЕРОВАНИЙ ЗАРЯД — опитування й запобіжники (тут, бо потрібен battery/дампи;
//  сам регулятор і стан машини — у charge.h, детальний опис різниці з
//  розрядом — на початку того файлу).
// ===========================================================================

// Старт заряду до обраного ВІДСОТКА (0 -> типово 100 %, повний заряд).
// Повертає nullptr при успіху, інакше — текст причини відмови.
const char *chargeStart(uint8_t targetPct) {
    if (!chargeAvailable()) return "Заряд не налаштовано: задайте CHARGE_PWM_PIN, CHARGE_ISENSE_PIN і CHARGE_VSENSE_PIN у settings.h";
    if (chargeRunning())    return "Заряд уже виконується";
    if (dischargeRunning()) return "Спочатку зупиніть розряд";
    // Без ШІМ заряд заборонено взагалі. На відміну від розряду (де відмова
    // ШІМ безпечно відкочується на digitalWrite: ключ відкритий повністю,
    // струм лише ЗРОСТАЄ понад задане, але резистор його жорстко обмежує),
    // тут відкочуватись нема куди: обмежувача струму в схемі немає, і
    // «постійно відкритий» ключ означав би (Uживл − Uпакета)/R_шунт —
    // десяток ампер через шунт і банки.
    if (!chargePwmOk())     return "Керування недоступне: ШІМ не прикріпився. "
                                   "Найімовірніше CHARGE_LEDC_CH ділить ТАЙМЕР з "
                                   "іншим каналом (вони йдуть попарно: 0-1, 2-3, "
                                   "4-5, 6-7), а частоти різні. Дивіться рядок "
                                   "CHARGE: у журналі — там номер таймера";

    if (!targetPct) targetPct = 100;
    if (targetPct < CHARGE_TARGET_PCT_MIN) targetPct = CHARGE_TARGET_PCT_MIN;
    if (targetPct > 100) targetPct = 100;
    // Ціль завжди в межах [BATTERY_EMPTY_MV..CHARGE_TARGET_MV] — вище
    // CHARGE_TARGET_MV (=100 % на шкалі) заряджати не можна ХАЙ ЯКИЙ
    // targetPct прийшов ззовні (клієнт міг надіслати щось дивне).
    uint16_t targetMv = (uint16_t)impresMvFromPercent(targetPct);
    if (targetMv > CHARGE_TARGET_MV) targetMv = CHARGE_TARGET_MV;
    uint16_t hardMaxMv = (uint16_t)(targetMv + CHARGE_HARD_MAX_HEADROOM_MV);

    // ⚑ ENABLE — НАЙПЕРШИМ, І ЛИШЕ ПОТІМ УСЕ ІНШЕ.
    //  Пакет IMPRES під'єднує свої клеми до зовнішнього кола не сам по собі, а
    //  за сигналом enable. Поки він не піднятий, на клемі немає напруги
    //  ПАКЕТА — там або нічого, або напруга нашого ж живлення через ключ. Отже
    //  будь-який вимір, зроблений до enable, стосується не пакета, а
    //  розімкненого кола: саме так і виходить те «9.9 В» (стеля подільника),
    //  через яке заряд оголошував хибну перенапругу.
    //
    //  Раніше enable піднімався в самому КІНЦІ chargeStart() — уже після того,
    //  як зміряно напругу, струм і температуру, і навіть після chargeSetDuty(),
    //  тобто струм подавався РАНІШЕ за під'єднання пакета. Коментар поруч при
    //  цьому стверджував «ще ДО подачі струму»; насправді порядок був
    //  зворотний.
    //
    //  CHARGE_ENABLE_LEAD_MS — пауза на те, щоб пакет справді замкнув клеми й
    //  напруга встоялась. Без неї перший же вимір потрапляв би у перехідний
    //  процес.
    battery.holdEnable(true);
    delay(CHARGE_ENABLE_LEAD_MS);

    //  Далі — усе, що може відмовити. Тіло винесене в лямбду НЕ для краси: у
    //  нього сім різних виходів «не можна почати», і при кожному enable треба
    //  зняти назад. Сім однакових рядків перед сімома return — це шість шансів
    //  забути один і лишити пакет під'єднаним після невдалого старту.
    const char *startErr = [&]() -> const char * {

    // ⚑ ДВА РІЗНІ ДЖЕРЕЛА, КОЖНЕ ЗА СВОЇМ ПРИЗНАЧЕННЯМ.
    //  • НАПРУГА — з нашого подільника (chargePackMv). Вона й керує зупинкою за
    //    ціллю, тож мусить бути тим самим числом, яке далі щосекунди міряє
    //    chargeTask(): міряти старт монітором, а хід — подільником означало б
    //    порівнювати різні шкали й ловити стрибок на першому ж опитуванні.
    //  • ТЕМПЕРАТУРА — з DS2438: свого датчика в пристрою немає, а без контролю
    //    нагріву заряд заборонено. Ключ зараз закритий, тож 1-Wire читається
    //    без зсуву землі (див. коментар про шунт у мінусовому проводі).
    chargeOff();                        // гарантовано закритий ключ на час вимірів

    // ⚑ ЖИВЛЕННЯ — ПЕРШИМ, ще до всіх інших перевірок. Без нього решта вимірів
    //  не має сенсу: заряд «іде» зі шпаруватістю в стелю й нульовим струмом, і
    //  виглядає це як несправність пакета, а не як забутий блок живлення.
    //  Міряємо саме зараз, на закритому ключі, — під струмом на шині живлення
    //  є просадка, і поріг довелося б розмазувати.
    uint8_t psu = chargePsuPoll();
    if (psu != PSU_OK && psu != PSU_UNKNOWN) {
        ledSet(LED_FAULT);
        // ⚑ ПОВІДОМЛЕННЯ МУСИТЬ ВЕСТИ ДО ВИХОДУ, а не лише називати біду.
        //  Контроль живлення — НОВИЙ вузол (подільник 68к/10к на GPIO39), і
        //  поки його фізично не зібрано, пін висить, читається ~0 В, і заряд
        //  відхиляється з «немає живлення» — тобто щойно додана перевірка
        //  блокує функцію, яка до неї працювала. Розрізнити «давача немає» і
        //  «живлення немає» вимірюванням НЕМОЖЛИВО: GPIO34..39 не мають
        //  внутрішніх підтяжок, тож розгойдати вільний пін і побачити різницю
        //  ніяк. Тому виходом мусить бути текст: називаємо виміряне число й
        //  прямо кажемо, що робити, якщо подільника ще немає.
        static char msg[190];
        snprintf(msg, sizeof(msg),
                 "%s. Виміряно %u.%02u В на GPIO%d (треба %d.%d…%d.%d). "
                 "Якщо подільник живлення ще НЕ ЗІБРАНО — закоментуйте "
                 "CHARGE_PSU_PIN у settings.h, і заряд працюватиме як раніше.",
                 chargePsuText(psu), chargePsuMv() / 1000, (chargePsuMv() % 1000) / 10,
                 (int)CHARGE_PSU_PIN,
                 CHARGE_PSU_MIN_MV / 1000, (CHARGE_PSU_MIN_MV % 1000) / 100,
                 CHARGE_PSU_MAX_MV / 1000, (CHARGE_PSU_MAX_MV % 1000) / 100);
        Serial.printf("=== Charge NOSTART: %s ===\n", msg);
        return msg;
    }

    uint16_t mv = chargePackMv();
    uint16_t mvPeak; uint16_t maIdle = chargeMeasureMa(&mvPeak);

    uint16_t dummyMv; int16_t ma, t;
    if (!dischargeSample(&dummyMv, &ma, &t)) {   // те саме читання DS2438: потрібна ТЕМПЕРАТУРА
        chargeOff();
        return "DS2438 не читається — заряд наосліп заборонено (без температури "
               "пакет можна перегріти). Спершу домогтися читання: сторінка "
               "стану скаже, чого бракує — підтяжки 4.7к, лінії даних чи enable";
    }
    // Правдоподібність власного вимірювального кола. Нуль на подільнику при
    // під'єднаному пакеті означає обрив вимірювальної лінії або переплутані
    // клеми — заряджати, не бачачи напруги, не можна.
    if (mv < BATTERY_EMPTY_MV / 2)
        return "Подільник напруги показує майже нуль — перевірте CHARGE_VSENSE_PIN і клеми";
    if (maIdle > CHARGE_DEADBAND_MA)
        return "На закритому ключі шунт бачить струм — перевірте ключ (можливо, пробитий MOSFET)";
    if (mv >= targetMv)                 return "Пакет уже заряджений до обраної цілі";
    if (mv >= hardMaxMv)                return "Напруга вже вище аварійної межі — заряд не почато";
    if (t >= CHARGE_MAX_TEMP_C * 10)    return "Пакет гарячий — дайте охолонути";

    memset(&g_chg, 0, sizeof(g_chg));
    g_chg.state    = CHG_RUN;
    g_chg.reason   = CHGR_NONE;
    g_chg.startMv  = g_chg.lastMv = mv;
    g_chg.targetMv  = targetMv;
    g_chg.targetPct = targetPct;
    g_chg.lastMa   = 0;              // ключ ще закритий — струму немає
    g_chg.lastTempC10 = t;
    g_chg.startMs  = g_chg.lastPollMs = millis();
    g_chg.startCca = g_chg.lastCca = impresCca(batteryDump2438);
    g_chg.startIca = g_chg.lastIca = batteryDump2438[12];
    g_chg.lastPct  = (uint8_t)impresPercentFromMv(mv);
    g_chg.rsense   = impresBmsRsense(batteryDump2438);   // шунт ПАКЕТА — лише для CCA
    (void)ma;                        // струм DS2438 тут не потрібен: міряємо своїм шунтом

    // ⚑ СТАРТ ІЗ РОЗРАХОВАНОЇ ТОЧКИ НУЛЬОВОГО СТРУМУ, а не з нуля.
    //  Шлях від нуля до робочої точки — сотні відліків ШІМ, тобто хвилини
    //  повзання. chargeStartDuty() цілиться одразу в ПОТРІБНИЙ струм (уставку
    //  профілю), рахуючи по тій гілці, у якій перетворювач зараз працює:
    //  переривчастій (струм ~D²) чи неперервній (струм ~D).
    //  Оцінка спирається на приблизні L і R — і це нормально: далі все веде
    //  замкнутий контур за реальним струмом, а крок регулятора пропорційний
    //  похибці, тож промах моделі закривається за кілька опитувань.
    g_chg.setMa = chargeSetpointMaForPct(g_chg.lastPct, targetPct);
    g_chg.duty  = chargeStartDuty(mv, g_chg.setMa);
    chargeSetDuty(g_chg.duty);

    // enable вже піднято на самому початку chargeStart() — до всіх вимірів.
    chargeWatchdog(true);

    ledSet(g_chg.lastPct >= CHARGE_LED_TAPER_PCT ? LED_CHARGE_TAPER : LED_CHARGE);
    // Дзеркально до dischargeStart(): зняти незакритий підсумок розряду, інакше
    // він перекриє сторінку заряду (докладніше — коментар там).
    dischargeDismiss();                    // не діє, якщо розряд справді йде
    chargeMarkDirty(2);
    Serial.printf("\n=== Charge started: %u mV (%u%%) -> ціль %u mV (%u%%), setpoint %u mA, "
                  "старт duty %u з %u (стеля %u = %d%%)%s ===\n",
                  mv, g_chg.lastPct, targetMv, targetPct, g_chg.setMa,
                  g_chg.duty, (unsigned)CHARGE_DUTY_FULL,
                  (unsigned)CHARGE_DUTY_MAX,
                  (int)CHARGE_DUTY_MAX_PCT, chargePwmOk() ? "" : ", PWM UNAVAILABLE");
    return nullptr;
    }();

    //  Єдине місце, де знімається enable після невдалого старту. Ключ гасимо
    //  теж: частина виходів трапляється вже після chargeOff(), частина — ні.
    if (startErr) {
        chargeOff();
        battery.holdEnable(false);
    }
    return startErr;
}

// Викликати часто з loop(). Реальна робота — раз на CHARGE_POLL_MS.
inline void chargeTask() {
    if (g_chg.state != CHG_RUN) return;
    unsigned long now = millis();

    if ((now - g_chg.startMs) / 60000UL >= (unsigned long)CHARGE_MAX_MIN) {
        chargeStop(CHGR_TIMEOUT);
        Serial.println("=== Charge ABORT: timeout ===");
        return;
    }
    chargeWatchdogFeed();
    if (now - g_chg.lastPollMs < CHARGE_POLL_MS) return;

    unsigned long dtMs = now - g_chg.lastPollMs;
    g_chg.lastPollMs = now;

    // ── СТОРОЖ (програмна половина) — той самий принцип, що й розряд:
    // довга затримка між опитуваннями = ключ побув відкритим без нагляду.
    if (dtMs > CHARGE_STALL_MS) {
        chargeStop(CHGR_STALL);
        Serial.printf("=== Charge ABORT: main loop stalled for %lu ms ===\n", dtMs);
        return;
    }
    g_chg.elapsedS = (now - g_chg.startMs) / 1000UL;

    // Інтеграл ємності за інтервал, що ЩОЙНО минув — на чинному (до цього
    // опитування) струмі, а не на щойно виміряному.
    g_chg.mahX1000 += ((uint32_t)(g_chg.lastMa < 0 ? -g_chg.lastMa : g_chg.lastMa) * dtMs) / 3600UL;

    // ── крок 1: СТРУМ під ЧИННОЮ шпаруватістю ────────────────────────────
    //  Серія відліків через кілька повних періодів ШІМ: середнє — це реальний
    //  середній струм у пакет, найбільший відлік — пік. Пік потрібен окремо:
    //  дроселя в схемі немає, тож у момент відкриття ключа струм обмежують лише
    //  різниця напруг і опір кола, і саме він вирішує долю шунта.
    pmStep(PM_STEP_ISENSE);
    uint16_t peakMa = 0;
    uint16_t avgMa  = chargeMeasureMa(&peakMa);

    // ⚑ ПІКОВА ВІДСІЧКА — ПЕРЕД усім іншим. Це єдиний запобіжник, який мусить
    // спрацювати В ТОМУ Ж проході, де помічений: середній струм може лишатись
    // у нормі, поки пік уже палить шунт.
    if (peakMa > CHARGE_PEAK_MA_MAX) {
        g_chg.peakMa = peakMa;
        chargeStop(CHGR_PEAK);
        Serial.printf("=== Charge ABORT: peak %u mA > %u mA (шунт %d мОм) ===\n",
                      peakMa, (unsigned)CHARGE_PEAK_MA_MAX, (int)CHARGE_SHUNT_MOHM);
        return;
    }

    // ── крок 2: НАПРУГА на коротко закритому ключі ────────────────────────
    //  Напруга на клемі й так згладжена дроселем, але ПІД СТРУМОМ до неї
    //  додається падіння на дротах і внутрішньому опорі пакета. Закриваємо
    //  ключ на CHARGE_VSENSE_SETTLE_MS: струм дроселя стікає через діод за
    //  мікросекунди, омічна просадка зникає, і вимір лишається ОДНАКОВИМ від
    //  початку до кінця заряду.
    pmStep(PM_STEP_VSENSE);
    uint16_t saveDuty = g_chg.duty;
    chargeSetDuty(0);
    dischargeSettle(CHARGE_VSENSE_SETTLE_MS);   // той самий неблокуючий сон, що й у розряду
    uint16_t mv = chargePackMv();

    // ⚑ КЛЕМА В СТЕЛІ — КЛЮЧ БІЛЬШЕ НЕ ВІДКРИВАЄМО ЦЬОГО ПРОХОДУ.
    //  Стеля означає розімкнене коло: пакет або від'єднався, або закрив себе
    //  сам. Гнати струм у невідомість, поки з'ясовується причина, немає жодної
    //  потреби — навантаження там усе одно немає. Важливіше інше: регулятор,
    //  побачивши нульовий струм, почав би ПІДНІМАТИ шпаруватість, і поки
    //  триває витримка на з'ясування, ключ виходив би на стелю. Тому вимір
    //  робимо, а привід до ключа лишаємо знятим — рішення ухвалить кінець
    //  цього ж проходу.
    bool satMv = chargeSenseSaturated(mv);
    if (satMv) saveDuty = 0;

    // ── крок 2а: ЖИВЛЕННЯ — тут же, на закритому ключі ────────────────────
    //  Разом із напругою пакета й з тієї ж причини: під струмом на шині
    //  живлення є просадка від власного опору блока й проводів, і порівнювати
    //  її з порогом означало б ловити не блок живлення, а кидки струму.
    //  Одиничний вихід за межі не зупиняє заряд — потрібно CHARGE_PSU_BAD_POLLS
    //  поспіль. Пропадання ж живлення видно одразу й безпомилково.
    pmStep(PM_STEP_PSU);
    uint8_t psu = chargePsuPoll();
    if (chargePsuTrip(psu, &g_chg.badPsuPolls)) {
        chargeStop(CHGR_PSU);
        ledSet(LED_FAULT);
        Serial.printf("=== Charge ABORT: %s (%u мВ, допуск %d..%d мВ) ===\n",
                      chargePsuText(psu), chargePsuMv(),
                      (int)CHARGE_PSU_MIN_MV, (int)CHARGE_PSU_MAX_MV);
        return;                      // ключ уже закритий — chargeStop() гасить його першим
    }

    // ── крок 2б: ТЕМПЕРАТУРА з DS2438 — РІДКО (CHARGE_TEMP_EVERY_N) ───────
    //  Транзакція 1-Wire коштує сотні мілісекунд, і весь цей час ключ мусить
    //  лишатись закритим (шунт у мінусовому проводі зсуває опорну землю
    //  чипа). Читати щосекунди означало б віддавати п'яту частину часу заряду
    //  й щоразу перезапускати струм дроселя з нуля. Температура міняється
    //  хвилинами, тож раз на CHARGE_TEMP_EVERY_N опитувань вистачає навіть
    //  для аварійної відсічки.
    // ⚑ НАЙПІДОЗРІЛІШИЙ ЕТАП. Саме тут іде повний пошук шини й читання 64
    //  байтів по 1-Wire — найдовша й найскладніша операція опитування, до того
    //  ж на шині, яка на цій платі читається нестабільно. Позначка стоїть ДО
    //  умови, щоб було видно навіть той прохід, де читання не виконувалось.
    pmStep(PM_STEP_DS2438);
    // ⚑ У КІНЦІ ЗАРЯДУ — КОЖНЕ ОПИТУВАННЯ, А НЕ РАЗ НА ДЕСЯТЬ.
    //  Рідке читання коштувало нам саме того випадку, задля якого монітор і
    //  потрібен. Пакет розмикає власний захист за секунди; при читанні раз на
    //  CHARGE_TEMP_EVERY_N опитувань свідчення зсередини на цей момент могло
    //  бути дев'ятисекундної давнини — тобто взяте ще до події. Щойно клема
    //  підійшла на CHARGE_CHIP_WATCH_MV до цілі (або вже стоїть у стелі),
    //  переходимо на щосекундне читання: тут воно і найточніше, і вирішальне.
    //  Ціна — сотні мілісекунд на транзакцію — платиться лише в останні
    //  хвилини сеансу, а не всі шість годин.
    bool nearEnd = satMv ||
                   ((uint32_t)mv + CHARGE_CHIP_WATCH_MV >= (uint32_t)g_chg.targetMv);
    bool tempRead = ((g_chg.polls % CHARGE_TEMP_EVERY_N) == 0) || nearEnd;
    bool chipFresh = false;      // монітор відповів САМЕ цього проходу
    int16_t t = g_chg.lastTempC10;
    if (tempRead) {
        // ⚑ Напругу з монітора БІЛЬШЕ НЕ ВИКИДАЄМО. Раніше вона йшла в змінну
        //  на ім'я dummy — а це був єдиний у прошивці незалежний вимір напруги
        //  пакета, зроблений ІЗСЕРЕДИНИ пакета. Саме його бракувало, щоб
        //  розрізнити «банки перезаряджені» й «на клемі живлення, бо пакета
        //  немає»: зовнішній подільник у другому випадку бреше, а чип — ні.
        uint16_t chipMv = 0; int16_t maChip = 0;
        if (dischargeSample(&chipMv, &maChip, &t)) {
            g_chg.readFails = 0;
            chipFresh     = true;
            g_chg.chipMv  = chipMv;
            g_chg.lastCca = impresCca(batteryDump2438);
            g_chg.lastIca = batteryDump2438[12];
        } else if (satMv) {
            // ⚑ ПОКИ КЛЕМА В СТЕЛІ, МОВЧАННЯ МОНІТОРА — ЦЕ СВІДЧЕННЯ, А НЕ ЗБІЙ.
            //  Вихід звідси по CHGR_NOREAD («монітор не читається») був би
            //  правильною відповіддю на неправильне питання. Коли клема стоїть
            //  у стелі, ми з'ясовуємо не справність шини, а долю пакета — і
            //  мовчання монітора тут не перешкода діагнозу, а сам діагноз:
            //  пакета в колі немає. Тому не зупиняємось і не виходимо, а
            //  доносимо цей факт до класифікації наприкінці проходу, де він
            //  разом із витримкою CHARGE_NOPACK_POLLS дасть CHGR_NOPACK.
            //  Лічильник невдач не чіпаємо: він міряє здоров'я шини, а шина
            //  при вийнятому пакеті мовчить законно.
        } else {
            chargeSetDuty(saveDuty);            // ключ назад, перш ніж вирішувати долю
            if (++g_chg.readFails >= CHARGE_MAX_READ_FAILS) {
                chargeStop(CHGR_NOREAD);
                Serial.println("=== Charge ABORT: DS2438 unreadable ===");
            }
            return;
        }
    }

    // Ключ НЕГАЙНО назад у робочу шпаруватість — закритим його не лишаємо ні на
    // мить довше, ніж триває вимір (інакше заряд просто не йшов би).
    chargeSetDuty(saveDuty);
    g_chg.polls++;

    // ── крок 3: перерахунок уставки і шпаруватості ────────────────────────
    pmStep(PM_STEP_REGULATOR);
    int pct = impresPercentFromMv(mv);
    if (satMv) {
        // ⚑ РЕГУЛЯТОР ПРИ РОЗІМКНЕНОМУ КОЛІ МОВЧИТЬ. Він працює за зворотним
        //  зв'язком по струму, а струму в розімкненому колі немає й бути не
        //  може: підняття шпаруватості нічого не виправить, зате виведе ключ
        //  на стелю за лічені проходи. Так само не чіпаємо відсоток і уставку —
        //  вони рахуються з напруги на клемі, а клема зараз не показує напругу
        //  пакета, і записати 100 % за відліком 4095 означало б збрехати ще й
        //  на екрані. Останні достовірні значення лишаються як були; долю
        //  сеансу вирішить класифікація наприкінці цього ж проходу.
        g_chg.duty = 0;
        chargeSetDuty(0);
        pct = g_chg.lastPct;
    } else {
        g_chg.lastPct = (uint8_t)pct;
        // Гістерезис: інакше уставка брязкає 1000 <-> 100 мА на межі (див. charge.h).
        g_chg.setMa   = chargeSetpointMaForPctH(pct, g_chg.targetPct, &g_chg.inTaper);
        g_chg.duty    = chargeNextDuty(g_chg.duty, (int32_t)avgMa, g_chg.setMa);
        chargeSetDuty(g_chg.duty);
    }

    // ── «КЛЮЧ НЕ ТЯГНЕ»: стеля шпаруватості без струму ────────────────────
    //  Дзеркало пікової відсічки й з тією ж метою — врятувати силовий ключ.
    //  Регулятор на брак струму відповідає підняттям шпаруватості, тобто
    //  ЗБІЛЬШУЄ час, який транзистор проводить під навантаженням; якщо причина
    //  браку — недостатній струм бази (ключ поза насиченням), це рівно те, що
    //  його й палить. Тому вихід на стелю разом із недобором струму — привід
    //  зупинитись, а не чекати шість годин до відсічки за часом.
    //  Лічильник скидається на першому ж нормальному опитуванні: рахуємо
    //  СТАЛИЙ стан, а не окремий провал під час стрибка уставки.
    if (chargeNoDriveTrip(g_chg.duty, (int32_t)avgMa, g_chg.setMa, &g_chg.lowDrivePolls)) {
        chargeStop(CHGR_NODRIVE);
        ledSet(LED_FAULT);
        // Підказка залежить від типу ключа: у біполярного винен струм бази,
        // у польового — напруга на затворі. Однакова порада для двох різних
        // приладів відправила б шукати не туди.
#if CHARGE_SW_IS_MOS
        Serial.printf("=== Charge ABORT: стеля duty %u, а струм лише %u мА з %u мА. "
                      "Ключ %s: перевірте напругу на затворі (дільник %d/%d Ом дає "
                      "|VGS| %d мВ при порозі %d), базу керуючого NPN (%d Ом), "
                      "дросель і контакти ===\n",
                      (unsigned)CHARGE_DUTY_MAX, avgMa, g_chg.setMa, CHARGE_SW_NAME,
                      (int)CHARGE_BASE_DRIVE_ASBUILT_OHM, (int)CHARGE_BASE_PULLUP_OHM,
                      (int)CHARGE_MOS_VGS_ON_ASBUILT_MV, (int)CHARGE_MOS_VGSTH_MAX_MV,
                      (int)CHARGE_NPN_BASE_ASBUILT_OHM);
#else
        Serial.printf("=== Charge ABORT: стеля duty %u, а струм лише %u мА з %u мА. "
                      "Перевірте струм бази ключа (база PNP %d Ом, база NPN %d Ом), "
                      "дросель і контакти ===\n",
                      (unsigned)CHARGE_DUTY_MAX, avgMa, g_chg.setMa,
                      (int)CHARGE_BASE_DRIVE_ASBUILT_OHM, (int)CHARGE_NPN_BASE_ASBUILT_OHM);
#endif
        return;
    }

    g_chg.lastMv = mv;
    g_chg.lastMa = (int16_t)avgMa;   // СЕРЕДНІЙ струм із НАШОГО шунта, додатний = заряджаємо
    g_chg.peakMa = peakMa;
    g_chg.lastTempC10 = t;           // з останнього читання монітора (див. крок 2б)

    // «чип N mV» — напруга з DS2438 усередині пакета. Друкуємо поруч із
    // виміром на клемі САМЕ для звірки: поки числа поряд, коло ціле; коли
    // клема пішла в стелю, а чип лишився на 8 В — пакета в колі немає.
    // ⚑ СЛІД У ЧОРНИЙ ЯЩИК — раз на опитування. Переживає скидання, і саме він
    //  перетворює «періодично перезавантажується» на конкретні обставини
    //  (postmortem.h).
    pmStep(PM_STEP_REPORT);
    pmNote(PM_MODE_CHARGE, g_chg.polls, g_chg.duty, g_chg.lastMa, mv);

    // Купа й запас стека — В КОЖНОМУ рядку журналу, а не лише при старті.
    //  Разові числа нічого не кажуть; потрібен ТРЕНД. Фрагментація й витік
    //  видно саме як повільне сповзання за годину роботи, а переповнення
    //  стека — як запас, що тане до сотень байтів.
    Serial.printf("charge: %u mV (чип %u mV) (%d%%), %d mA (пік %u, set %u, duty %u/%u = %u%%), "
                  "%.1f W, %.1f C%s, %lu mAh (CCA %lu), ICA %u, %lus, "
                  "купа %lu/%lu, стек %lu\n",
                  mv, g_chg.chipMv, pct, g_chg.lastMa, g_chg.peakMa, g_chg.setMa,
                  g_chg.duty, (unsigned)CHARGE_DUTY_FULL, chargeDutyPct(),
                  chargeWattsX10(mv, g_chg.lastMa) / 10.0f, t / 10.0f,
                  tempRead ? "" : "~",
                  (unsigned long)chargeMah(), (unsigned long)chargeCcaMah(),
                  g_chg.lastIca, (unsigned long)g_chg.elapsedS,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                  (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    chargeMarkDirty(1);
    // Індикатор — за ТИМ САМИМ станом, що й струм. Власне порівняння з порогом
    // брязкало б разом із відсотком, і світлодіод стрибав би між двома режимами
    // щосекунди — а кожен такий перехід ще й відчіплює/чіпляє ШІМ підсвітки.
    ledSet(g_chg.inTaper ? LED_CHARGE_TAPER : LED_CHARGE);

    // ⚑ «ПАКЕТ ВІД'ЄДНАНО» ПЕРЕВІРЯЄМО ПЕРШИМ — ДО ПЕРЕНАПРУГИ.
    //  Порядок тут вирішує, що побачить користувач. Показання, що вперлось у
    //  стелю АЦП, формально «вище аварійної межі», тож стара послідовність
    //  віддавала цей випадок перенапрузі — і на екрані з'являлось «напруга
    //  9.9 В, аварійна зупинка, перенапруга». Обидві половини повідомлення
    //  були неправдою: 9.9 В — це підпис відліку 4095 (див. chargeSenseSaturated()
    //  у charge.h), а перенапруги не було взагалі. Правильний діагноз —
    //  на клемі стоїть живлення, тобто пакета в колі немає.
    //
    //  ⚑ І ВІДПОВІДЕЙ НА «КЛЕМА В СТЕЛІ» ЧОТИРИ, А НЕ ОДНА.
    //  Спостереження власника: дійшовши до 8.2 В, пакет розмикає власний
    //  захист — і клема йде в стелю точно так само, як від загубленого
    //  контакту. Єдиний діагноз «пакета немає» на всі випадки перетворював
    //  штатне завершення заряду на аварію. Розрізняє їх монітор усередині
    //  пакета: він від силового ключа пакета не залежить (див. chargeSatVerdict()
    //  у charge.h). Свідчення накопичуються за весь епізод, а не за останній
    //  прохід: одна невдала транзакція 1-Wire не сміє перетворити «повний» на
    //  «немає».
    chargeSatWitness(mv, chipFresh, g_chg.chipMv,
                     &g_chg.noPackPolls, &g_chg.satChipOk, &g_chg.satChipMv);
    if (chargeSatTripped(g_chg.noPackPolls)) {
        uint8_t v = chargeSatVerdict(g_chg.satChipOk, g_chg.satChipMv, g_chg.targetMv);
        switch (v) {
            case SATV_FULL:
                chargeStop(CHGR_PACKFULL);
                Serial.printf("=== Charge DONE: пакет закрився власним захистом на %u мВ "
                              "(ціль %u мВ, допуск %d мВ) — заряд завершено. "
                              "%lu mAh за %lus ===\n",
                              g_chg.satChipMv, g_chg.targetMv, (int)CHARGE_PACKFULL_TOL_MV,
                              (unsigned long)chargeMah(), (unsigned long)g_chg.elapsedS);
                break;
            case SATV_OVER:
                chargeStop(CHGR_HARD_MAX);
                ledSet(LED_FAULT);
                Serial.printf("=== Charge ABORT: монітор пакета показує %u мВ — вище "
                              "аварійної межі %u мВ. Це вимір ЗСЕРЕДИНИ пакета, "
                              "справжня перенапруга ===\n",
                              g_chg.satChipMv,
                              (unsigned)(g_chg.targetMv + CHARGE_HARD_MAX_HEADROOM_MV));
                break;
            case SATV_OPEN:
                chargeStop(CHGR_PACKOPEN);
                ledSet(LED_FAULT);
                Serial.printf("=== Charge ABORT: пакет розімкнув власний захист на %u мВ, "
                              "а це на %u мВ нижче цілі %u мВ. За напругою він не повний, "
                              "тож причина інша: розбіг банок, температура або струм. "
                              "Пакет потребує перевірки ===\n",
                              g_chg.satChipMv,
                              (unsigned)(g_chg.targetMv - g_chg.satChipMv), g_chg.targetMv);
                break;
            default:
                chargeStop(CHGR_NOPACK);
                ledSet(LED_FAULT);
                Serial.printf("=== Charge ABORT: на клемі %u мВ — це стеля подільника (%d мВ), "
                              "а не напруга пакета, і монітор пакета мовчить. Пакета в колі "
                              "немає: перевірте контакт клем і резистор 4.7 кОм між enable "
                              "(GPIO%d) і +3.3 В ===\n",
                              mv, (int)CHARGE_VSENSE_SAT_MV, (int)PULLUP_PIN);
                break;
        }
        return;
    }

    // ⚑ ВІДЛІК У СТЕЛІ НЕ ГОДИТЬСЯ ДЛЯ ЖОДНОГО ПОРІВНЯННЯ З НАПРУГОЮ.
    //  Саме тут ховалась причина «спрацювала перенапруга»: перевірка «пакета
    //  немає» стоїть першою, але вона з ВИТРИМКОЮ у CHARGE_NOPACK_POLLS, а
    //  аварійна межа спрацьовувала з ПЕРШОГО ж відліку. Тож на першому
    //  насиченому проході керування спокійно проходило повз неї просто вниз —
    //  і 9900 мВ, «формально вище межі», давало CHGR_HARD_MAX ще до того, як
    //  витримка встигала набратись. Порядок перевірок цього не рятує; рятує
    //  лише те, що насичений відлік узагалі не подається на порівняння з
    //  напругою — він не є виміром напруги.
    if (satMv) return;

    if (mv >= (uint32_t)g_chg.targetMv + CHARGE_HARD_MAX_HEADROOM_MV) {
        chargeStop(CHGR_HARD_MAX);
        Serial.printf("=== Charge ABORT: above hard maximum (%u мВ >= %u мВ; "
                      "монітор DS2438 %u мВ) ===\n",
                      mv, (unsigned)(g_chg.targetMv + CHARGE_HARD_MAX_HEADROOM_MV),
                      g_chg.chipMv);
    } else if (t >= CHARGE_MAX_TEMP_C * 10) {
        chargeStop(CHGR_TEMP);
        Serial.println("=== Charge ABORT: overheat ===");
    } else if (mv >= g_chg.targetMv) {
        chargeStop(CHGR_TARGET);
        Serial.printf("=== Charge DONE: %lu mAh in %lus ===\n",
                      (unsigned long)chargeMah(), (unsigned long)g_chg.elapsedS);
    } else if (chipFresh && g_chg.chipMv >= g_chg.targetMv) {
        // ⚑ ДРУГИЙ, НЕЗАЛЕЖНИЙ КРИТЕРІЙ ЗАВЕРШЕННЯ — ЗА МОНІТОРОМ ПАКЕТА.
        //  Перегони, які ми програвали. Пакет вимикає себе за власним виміром,
        //  а ми зупинялись за своїм — через подільник із його похибкою і через
        //  дроти з їхнім падінням. Досить було нам занизити на пів відсотка,
        //  щоб пакет щоразу встигав першим, і штатне завершення заряду
        //  приходило до нас у вигляді розімкненого кола.
        //  Монітор міряє ті самі банки, що й захист пакета, тож ця перевірка
        //  зупиняє заряд ДО того, як захисту буде що робити. Помилитись вона
        //  може лише в безпечний бік: зупинити зарано, а не запізно.
        //  chipFresh обов'язковий — g_chg.chipMv поза цією умовою може бути
        //  показом кількох опитувань давнини (див. CHARGE_TEMP_EVERY_N).
        chargeStop(CHGR_TARGET);
        Serial.printf("=== Charge DONE: монітор пакета %u мВ >= ціль %u мВ "
                      "(на клемі %u мВ). %lu mAh за %lus ===\n",
                      g_chg.chipMv, g_chg.targetMv, mv,
                      (unsigned long)chargeMah(), (unsigned long)g_chg.elapsedS);
    }
}

// ── КОНТРОЛЬ ЖИВЛЕННЯ У СПОКОЇ ─────────────────────────────────────────────
//  Під час заряду живлення міряє chargeTask() — на закритому ключі, разом із
//  напругою пакета. Але саме в спокої несправність і треба помітити: інакше
//  користувач дізнається про забутий блок живлення лише з відмови старту.
//
//  ⚑ Світлодіод чіпаємо ОБЕРЕЖНО. LED_FAULT — постійний сигнал, і виставляти
//  його поверх заряду/розряду/читання не можна: це стерло б індикацію процесу,
//  який саме йде. Тому вмикаємо його тільки із «спокійних» режимів, а знімаємо
//  тільки якщо він і зараз наш.
inline void chargePsuIdleTask() {
    if (!chargePsuSensed()) return;
    if (chargeRunning()) return;              // під час заряду цим займається chargeTask()

    static unsigned long lastMs = 0;
    unsigned long now = millis();
    if (lastMs && (now - lastMs) < CHARGE_PSU_IDLE_MS) return;
    lastMs = now;

    uint8_t before = chargePsuState();
    uint8_t after  = chargePsuPoll();
    if (after != before) chargeMarkDirty(2);  // стан живлення показує й екран

    bool fault = chargePsuFault();
    LedMode m = ledMode();
    if (fault) {
        // Не перебиваємо індикацію процесу, що саме йде.
        if (m == LED_IDLE || m == LED_BOOT) ledSet(LED_FAULT);
    } else if (m == LED_FAULT) {
        ledSet(LED_IDLE);                     // несправність зникла — повертаємось у спокій
    }
}

// Стан заряду у JSON — для веб-моніторингу й USB-клієнта.
static String chargeJson() {
    // ⚑ ОДНА ВИДІЛЕНА ДІЛЯНКА ЗАМІСТЬ СОТЕНЬ ПЕРЕВИДІЛЕНЬ. Цей відповідач
    //  кличеться КОЖНУ СЕКУНДУ, поки клієнт дивиться на заряд/розряд, а нижче
    //  йдуть десятки `j += ...`. Рядок Arduino росте блоками по 16 байтів, тож
    //  без reserve() кожна відповідь — це десятки realloc (malloc+memcpy+free)
    //  плюс тимчасовий String на КОЖНЕ число.
    //
    //  Само по собі це не помилка, але за години заряду виходять СОТНІ ТИСЯЧ
    //  розподілів різного розміру — а це фрагментація купи. Вільної пам'яті
    //  начебто вистачає, а суцільного шматка під буфер Wi-Fi/TCP уже немає, і
    //  черговий розподіл падає. Виглядає це як «під час заряду пристрій
    //  періодично перезавантажується» — тобто рівно як скарга.
    //
    //  reserve() робить із цього ОДИН розподіл. Розмір узято з запасом над
    //  реальною довжиною відповіді; якщо не влізе, String просто дорощується
    //  як і раніше, тобто гірше не стане.
    String j; j.reserve(1536);
    j = "{\"available\":" + String(chargeAvailable() ? "true" : "false");
    j += ",\"state\":\"" + String(g_chg.state == CHG_RUN ? "run"
                               : g_chg.state == CHG_DONE ? "done"
                               : g_chg.state == CHG_ABORT ? "abort" : "idle") + "\"";
    j += ",\"reason\":\""; j += chargeReasonText(g_chg.reason); j += "\"";
    // targetMv/targetPct — ЦЕЙ сеанс (0, доки не стартував); targetPctMin —
    // нижня межа для повзунка/полів клієнта (сама шкала пресетів 100/95/90/
    // 85/80 % захардкожена в кожному клієнті, як і voltage-пресети розряду).
    j += ",\"targetMv\":"  + String(g_chg.targetMv);
    j += ",\"targetPct\":" + String(g_chg.targetPct);
    j += ",\"targetPctMin\":" + String(CHARGE_TARGET_PCT_MIN);
    j += ",\"startMv\":"  + String(g_chg.startMv);
    j += ",\"mv\":"       + String(g_chg.lastMv);
    j += ",\"ma\":"       + String(g_chg.lastMa);
    j += ",\"pct\":"      + String(g_chg.lastPct);
    j += ",\"tempC\":"    + String(g_chg.lastTempC10 / 10.0f, 1);
    j += ",\"mah\":"      + String((unsigned long)chargeMah());
    j += ",\"watts\":"    + String(chargeWattsX10(g_chg.lastMv, g_chg.lastMa) / 10.0f, 1);
    j += ",\"ccaMah\":"   + String((unsigned long)chargeCcaMah());
    j += ",\"ccaDelta\":" + String((int)(uint16_t)(g_chg.lastCca - g_chg.startCca));
    j += ",\"ica\":"      + String(g_chg.lastIca);
    j += ",\"icaStart\":" + String(g_chg.startIca);
    j += ",\"elapsedS\":" + String((unsigned long)g_chg.elapsedS);
    j += ",\"polls\":"    + String(g_chg.polls);
    j += ",\"setMa\":"    + String(g_chg.setMa);
    // Керування тепер — шпаруватість ключа, а не «цільова напруга DC/DC».
    // Клієнти показують duty/dutyMax у відліках і dutyPct у відсотках.
    j += ",\"duty\":"     + String(g_chg.duty);
    j += ",\"dutyMax\":"  + String((unsigned)CHARGE_DUTY_MAX);
    j += ",\"dutyFull\":" + String((unsigned)CHARGE_DUTY_FULL);
    j += ",\"dutyPct\":"  + String(chargeDutyPct());
    // Вершина пульсацій і межа — запобіжник від «дросель випав із кола».
    j += ",\"peakMa\":"   + String(g_chg.peakMa);
    j += ",\"peakMaxMa\":"+ String((unsigned)CHARGE_PEAK_MA_MAX);
    j += ",\"shuntMohm\":"+ String((unsigned)CHARGE_SHUNT_MOHM);
    j += ",\"pwm\":"      + String(chargePwmOk() ? "true" : "false");
    j += ",\"hardMaxMv\":"+ String((unsigned)g_chg.targetMv + CHARGE_HARD_MAX_HEADROOM_MV);
    // ── ЖИВЛЕННЯ +14 В ─────────────────────────────────────────────────────
    //  Йде в тому ж об'єкті, що й заряд, і оновлюється НЕЗАЛЕЖНО від того, чи
    //  заряд узагалі запускали: несправний блок треба показати на екрані до
    //  того, як користувач натисне кнопку, а не після невдалого старту.
    // ── ОСТАННЄ СКИДАННЯ ───────────────────────────────────────────────────
    //  Причина й обставини — прямо в статусі заряду, бо саме тут користувач і
    //  дивиться, коли пристрій «періодично перезавантажується». Читати консоль
    //  він не зобов'язаний, а без цих чисел скарга лишається здогадкою.
    j += ",\"rstReason\":" + String(g_pmResetReason);
    j += ",\"rstText\":\"" + String(resetReasonText((esp_reset_reason_t)g_pmResetReason)) + "\"";
    j += ",\"rstAbnormal\":" + String(pmIsAbnormal(g_pmResetReason) ? "true" : "false");
    j += ",\"pmValid\":" + String(g_pmPrevOk ? "true" : "false");
    if (g_pmPrevOk) {
        j += ",\"pmMode\":\"" + String(pmModeName(g_pmPrev.mode)) + "\"";
        j += ",\"pmStep\":\"" + String(pmStepName(g_pmPrev.step)) + "\"";
        j += ",\"pmSec\":"   + String((unsigned long)(g_pmPrev.ms / 1000));
        j += ",\"pmPolls\":" + String(g_pmPrev.polls);
        j += ",\"pmDuty\":"  + String(g_pmPrev.duty);
        j += ",\"pmMa\":"    + String(g_pmPrev.ma);
        j += ",\"pmMv\":"    + String(g_pmPrev.mv);
        j += ",\"pmHeap\":"  + String((unsigned long)g_pmPrev.freeHeap);
        j += ",\"pmBlock\":" + String((unsigned long)g_pmPrev.maxBlock);
        j += ",\"pmStack\":" + String((unsigned long)g_pmPrev.stackLeft);
    }
    // Поточні числа — щоб тренд було видно НЕ ЛИШЕ після падіння.
    j += ",\"heap\":"      + String((unsigned long)ESP.getFreeHeap());
    j += ",\"heapBlock\":" + String((unsigned long)ESP.getMaxAllocHeap());
    j += ",\"stack\":"     + String((unsigned long)uxTaskGetStackHighWaterMark(NULL));

    j += ",\"psuSensed\":" + String(chargePsuSensed() ? "true" : "false");
    j += ",\"psuMv\":"    + String(chargePsuMv());
    // НОМІНАЛ блока живлення — головне число в повідомленні про помилку:
    // саме його користувач має під'єднати. Допуск нижче лише пояснює, чому
    // нинішній блок відхилено.
    j += ",\"psuNomMv\":" + String((unsigned)CHARGE_SUPPLY_MV);
    j += ",\"psuMinMv\":" + String((unsigned)CHARGE_PSU_MIN_MV);
    j += ",\"psuMaxMv\":" + String((unsigned)CHARGE_PSU_MAX_MV);
    j += ",\"psuOk\":"    + String(!chargePsuFault() ? "true" : "false");
    j += ",\"psuHead\":\""; j += chargePsuHead(chargePsuState()); j += "\"";
    j += ",\"psuText\":\""; j += chargePsuText(chargePsuState()); j += "\"";
    j += "}";
    return j;
}

// ------------------- Ремонт / правка / зміна ємності -------------------

// Перерахунок "відновних" полів поточних дампів: контрольна сума заголовка
// DS2433, дзеркало калібрування (з уцілілого DS2438 в DS2433), контрольні суми
// відомих записів (0x0D CCA/DCA і 0x17 історія ємності). НЕ чіпає дані —
// лише виправляє цілісність, щоб рація знову прийняла підправлену прошивку.
void repairDumps() {
    if (hasDump && hasDump2438 && mirrorSourceValid(batteryDump2438) &&
        !mirrorOk(batteryDump, batteryDump2438)) {
        // DS2438 зазвичай зберігається при пошкодженні DS2433 — беремо калібрування з нього.
        // ЛИШЕ якщо дзеркало DS2438 реальне (R7 його не має → не чіпаємо заголовок).
        syncMirrorFrom2438(batteryDump, batteryDump2438);
        Serial.println("repair: mirror DS2438->DS2433 restored");
    }
    if (hasDump) {
        fixHeaderChecksum(batteryDump);

        // Запис COPYRIGHT (0x0E0, довжина 0x20) має звичайну суму Σ≡0x5A, але
        // ремонт його НЕ бачив: ланцюг нижче йде лише від 0x120, а цей запис
        // лежить окремо, поза ланцюгом. Правимо явно, за фіксованою адресою.
        // Моделі без нього (4409A, APLI4810C) пропускаються — його відсутність
        // не є пошкодженням, рація такі пакети приймає.
        if (impresFixCopyright(batteryDump))
            Serial.println("repair: COPYRIGHT record @0x0E0 checksum fixed");

        // Блок профілю моделі 0x021..0x040 (Σ≡0x00) свідомо НЕ правимо: він
        // побайтово однаковий у всіх екземплярів моделі, тож зіпсована сума
        // означає пошкоджені ДАНІ, і лікується це записом модельної частини з
        // еталона, а не підгонкою байта. Тільки повідомляємо.
        if (!impresProfileOk(batteryDump))
            Serial.println("repair: WARNING profile block 0x021..0x040 checksum is wrong "
                           "-> потрібен запис модельної частини еталона");

        // Перерахунок контрольних сум записів. Раніше тут «шукали запис 0x17»
        // як «байт 0x17, за яким 0x00», тобто трактували довжину як тег і
        // правили суму випадковому запису. Тепер ідемо ЛАНЦЮГОМ від 0x120
        // (заводська таблиця + модель + навчені записи) і перераховуємо суму
        // лише тим записам, які її мають і в яких вона зараз хибна.
        // ⚑ Блоки BMS (дати, цикли, калібрування, гістограми) лежать за
        // адресами з ТАБЛИЦІ ВЕКТОРІВ, і більшість із них — НИЖЧЕ 0x120, куди
        // ланцюг нижче не заходить узагалі. Правимо їх спільним кодом
        // (impres_audit.h), яким і Майстер їх виявляє.
        int nb = impresAuditFixSums(batteryDump);
        if (nb) Serial.printf("repair: BMS block checksums fixed: %d\n", nb);

        int i = 0x120;
        while (i < (int)DUMP_SIZE) {
            if (batteryDump[i] == 0xFF) { i++; continue; }
            int len = batteryDump[i];
            if (len < 2 || i + len > (int)DUMP_SIZE) break;
            // 3-байтовий запис прапорців ЗП контрольної суми не має — не чіпаємо.
            if (len > 3 && !impresRecordOk(batteryDump, i)) {
                impresFixRecord(batteryDump, i, len);
                Serial.printf("repair: record @0x%03X len=%d checksum fixed\n", i, len);
            }
            i += len;
        }
    }
}

// Веб-ремонт: виправляє цілісність і пише обидві мікросхеми. Це "відновлення
// пошкодженої прошивки" для випадку пошкодженого заголовка/калібрування. Повне
// відновлення стертого DS2433 робиться завантаженням еталонного дампа той же
// моделі (вкладка «Прошивка» → запис).
// Ядро ремонту без HTTP — викликається і з веб-обробника, і з меню дисплея.
// ── ДОБУДОВА ПІСЛЯ ЗАРЯДНОЇ СТАНЦІЇ ──────────────────────────────────────
//  Одномісний IMPRES-зарядний WPLN4226A, отримавши пакет зі стертим DS2433,
//  сам записує в нього дзеркало заголовка з DS2438 (26 байт: DS2433[0x01..0x1A]
//  = DS2438[0x18..0x31]) — але НЕ виправляє контрольну суму, і на цьому
//  зупиняється: профіль, модель і блоки лишаються 0xFF. Заголовок після цього
//  структурно НЕВАЛІДНИЙ, хоча дані в ньому вже правильні.
//
//  Ця функція добудовує РІВНО те, що почала станція: копіює дзеркало (якщо
//  ще не скопійоване — safe навіть коли вже скопійоване) і виправляє суму.
//  Профіль, модель і блоки цим НЕ відновлюються — для них потрібен «Відновити
//  еталон» (модель відома) або режим копії (модель невідома, але DS2438 несе
//  достатньо даних — див. impres_clone.h).
bool performHeaderComplete(String *note) {
    if (!hasDump) { if (note) *note = "Спочатку зчитайте АКБ"; return false; }
    if (!hasDump2438 || !mirrorSourceValid(batteryDump2438)) {
        if (note) *note = "DS2438 не читається або дзеркала в ньому немає — добудовувати нічим";
        return false;
    }
    bool already = mirrorOk(batteryDump, batteryDump2438);
    syncMirrorFrom2438(batteryDump, batteryDump2438);
    ledSet(LED_WRITE); displayShow("ДОБУДОВА...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    displayShow(ok ? "ДОБУДОВА OK" : "ДОБУДОВА ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    if (note) {
        String n = already
            ? "Заголовок добудовано: дзеркало вже було на місці, виправлено лише суму."
            : "Заголовок добудовано з дзеркала DS2438.";
        // Чесно кажемо, чого цей крок НЕ вирішує — інакше виглядало б, що
        // пакет уже готовий, хоча профілю й моделі в ньому й досі немає.
        char md[16] = "";
        if (!decodeModel(md, sizeof(md)) || !md[0])
            n += " Модель і профіль ще відсутні: далі — «Відновити еталон» (якщо модель "
                 "відома) або режим копії в «Небезпечній зоні» (якщо ні).";
        *note = n;
    }
    Serial.printf("HDRFIX: mirror %s, write %s\n", already ? "already ok" : "restored",
                  ok ? "OK" : "FAIL");
    return ok;
}

// POST /api/hdrfix
void handleHeaderComplete() {
    if (!requireAdmin()) return;
    String note;
    bool ok = performHeaderComplete(&note);
    server.send(ok ? 200 : 400, "application/json",
                String("{\"status\":\"") + (ok ? "success" : "error") + "\",\"message\":\"" + note + "\"}");
}

bool performRepair() {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }
    ledSet(LED_WRITE); displayShow("РЕМОНТ...");
    repairDumps();
    bool ok = true;
    if (hasDump)     ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
    if (hasDump2438) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) {
        if (hasDump)     saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        if (hasDump2438) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        displayShow("РЕМОНТ OK");
    } else displayShow("РЕМОНТ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    return ok;
}

void handleRepair() {
    if (!requireAdmin()) return;
    if (!hasDump && !hasDump2438) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return;
    }
    bool ok = performRepair();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Firmware integrity repaired\"}"
           : "{\"status\":\"error\",\"message\":\"Repair write failed\"}");
}

// Веб-стирання DS2433 (крайній випадок), під паролем.
void handleWipe2433() {
    if (!requireAdmin()) return;
    bool ok = performWipe2433();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"DS2433 fully erased\"}"
           : "{\"status\":\"error\",\"message\":\"Wipe write failed\"}");
}

// Веб-стирання DS2438 (крайній випадок), під паролем.
void handleWipe2438() {
    if (!requireAdmin()) return;
    bool ok = performWipe2438();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"DS2438 fully erased\"}"
           : "{\"status\":\"error\",\"message\":\"Wipe write failed\"}");
}

// Веб-очистка (крім критичних даних), під паролем.
void handleClean() {
    if (!requireAdmin()) return;
    if (!hasDump && !hasDump2438) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return;
    }
    if (hasDump && !hasSN2433) {
        server.send(400, "application/json",
            "{\"status\":\"error\",\"message\":\"ROM-ID чипа DS2433 невідомий — історію "
            "в DS2433 нічим переписати, а обнуляти лише монітор не можна (пакет перестане "
            "впізнаватись). Перечитайте АКБ на пристрої й повторіть.\"}");
        return;
    }
    bool ok = performFactoryClean();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Історію використання обнулено в ОБОХ "
             "чипах; ідентичність, модель і дата виготовлення збережені\"}"
           : "{\"status\":\"error\",\"message\":\"Clean write failed\"}");
}

// Зсуви/довжини ділянок, які востаннє змінив applyModel(), щоб writeModelPages()
// записав РІВНО їх, не шукаючи запис заново. Це критично: після запису моделі,
// що починається з цифри (напр. "4409A"), findModelRecord() вже НЕ знайшов би
// щойно змінений запис 0x0B (він шукає 0x0B + літеру A-Z) — і сторінка мовчки
// не записувалась. Тепер пишемо саме те, що змінили.
static int g_mdRecOff = -1;   // зсув запису 0x0B моделі (або -1)
static int g_mdHdrOff = -1;   // зсув ASCII-моделі у заголовку (або -1)
static int g_mdHdrLen = 0;    // довжина ASCII-моделі у заголовку

// Валідність імені моделі: 3..9 символів [A-Z0-9]. Винесено окремо, щоб
// відрізняти «невірне ім'я» від «у дампі немає куди писати» і давати точну
// підказку користувачу.
static bool modelNameValid(const char *name) {
    int n = strlen(name);
    if (n < 3 || n > 9) return false;
    for (int k = 0; k < n; k++) {
        char c = name[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// Знаходить зсув запису моделі або -1.
//
// ⚠️ Раніше шукали просто «байт 0x0B, за яким літера A-Z». На цілому дампі це
// випадково працює, але на частково стертому/сміттєвому чипі дає хибне влучання
// раніше за справжній запис — і модель писалася не туди (власник це й бачив:
// «ошибку не выдало, но и действий записи не было»). Тепер перевіряємо повний
// запис: довжина 0x0B, 9 символів [A-Z0-9 ] і контрольна сума Σ≡0x5A, зі
// штатним місцем 0x148 у пріоритеті.
int findModelRecord() {
    return impresFindModel(batteryDump);
}

// Правит дамп: МОДЕЛЬ (part number) зберігається у ДВОХ місцях —
//   • запис 0x0B: 9-байтове поле, доповнене пробілами, Σ≡0x5A;
//   • ASCII-модель у заголовку 0x23 (лише формат 4488/4493).
// Оновлюємо ОБИДВІ копії, інакше рація читає стару. Довжина 3..9, [A-Z0-9].
// Повертає к-сть оновлених копій (>0 — успіх), або -1 (невірне ім'я / жодної копії).
int applyModel(const char *name) {
    if (!modelNameValid(name)) return -1;
    int n = strlen(name);
    g_mdRecOff = -1; g_mdHdrOff = -1; g_mdHdrLen = 0;
    int updated = 0;
    // (1) Запис 0x0B.
    int rec = findModelRecord();
    if (rec < 0) {
        // Порожній/стертий/невідомий чіп — запису моделі немає. СТВОРЮЄМО його
        // на стандартному місці (0x148, де запис 0x0B у genuine 4409A/4809A),
        // щоб «Запис моделі» працював і коли модель відсутня (саме заради цього
        // випадку функція й потрібна).
        rec = 0x148;
        batteryDump[rec] = 0x0B;
    }
    {
        for (int k = 0; k < 9; k++) batteryDump[rec + 1 + k] = (k < n) ? (uint8_t)name[k] : 0x20;
        fixRecordChecksum(batteryDump, rec, 11);   // [0x0B][9][контр], Σ≡0x5A
        g_mdRecOff = rec;
        updated++;
    }
    // (2) ASCII-модель у заголовку.
    if (batteryDump[0x23] >= 'A' && batteryDump[0x23] <= 'Z') {
        int L = 0, j = 0x23;
        while (j < 0x30 && ((batteryDump[j] >= 'A' && batteryDump[j] <= 'Z') ||
                            (batteryDump[j] >= '0' && batteryDump[j] <= '9'))) { j++; L++; }
        for (int k = 0; k < L; k++) batteryDump[0x23 + k] = (k < n) ? (uint8_t)name[k] : 0x00;
        fixHeaderChecksum(batteryDump);            // заголовок 0x00..0x1F (модель поза ним — no-op, але узгоджено)
        g_mdHdrOff = 0x23; g_mdHdrLen = L;
        updated++;
    }
    return updated > 0 ? updated : -1;
}

// Пише зачеплені моделлю сторінки DS2433 (запис 0x0B і заголовок), кожну окремо —
// точковий запис не залежить від придатності решти чипа до перезапису. Пише
// САМЕ ділянки, що змінив applyModel() (не шукає їх заново — інакше модель, що
// починається з цифри, не знаходилась і мовчки не записувалась). Повертає false,
// якщо не було записано жодної копії (щоб не рапортувати «успіх» без запису).
bool writeModelPages() {
    bool ok = true, wrote = false;
    if (g_mdRecOff >= 0) { ok &= battery.writeBatteryRange(batteryDump, g_mdRecOff, 11); wrote = true; }
    if (g_mdHdrOff >= 0 && g_mdHdrLen > 0) { ok &= battery.writeBatteryRange(batteryDump, g_mdHdrOff, g_mdHdrLen); wrote = true; }
    return wrote && ok;
}

// Ядро запису моделі: правит дамп + пише зачеплені сторінки. Спільне для веб і USB.
bool performSetModel(const char *name) {
    if (!hasDump) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }
    if (applyModel(name) < 0) return false;
    ledSet(LED_WRITE); displayShow("ЗАПИС МОДЕЛІ");
    bool ok = writeModelPages();
    if (ok) { saveDump("/dump.bin", batteryDump, DUMP_SIZE); displayShow("МОДЕЛЬ OK"); }
    else displayShow("МОДЕЛЬ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    return ok;
}

void handleSetModel() {
    if (!requireAdmin()) return;
    if (!hasDump) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Спочатку зчитайте АКБ\"}"); return; }
    String m = server.arg("model"); m.trim(); m.toUpperCase();
    if (!modelNameValid(m.c_str())) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Модель має містити 3–9 символів A–Z / 0–9\"}"); return;
    }
    if (applyModel(m.c_str()) < 0) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Невірне ім'я моделі (3–9 символів A–Z / 0–9)\"}"); return;
    }
    ledSet(LED_WRITE); displayShow("ЗАПИС МОДЕЛІ");
    bool ok = writeModelPages();
    if (ok) { saveDump("/dump.bin", batteryDump, DUMP_SIZE); displayShow("МОДЕЛЬ OK"); }
    else displayShow("МОДЕЛЬ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    server.send(ok ? 200 : 500, "application/json",
        ok ? (String("{\"status\":\"success\",\"model\":\"") + m + "\"}")
           : "{\"status\":\"error\",\"message\":\"Write failed (see serial log)\"}");
}

// РУЧНИЙ РЕЖИМ: правка байта у ЗАВОДСЬКІЙ таблиці моделі (запис @0x129, зсув +21).
//
// ⚠️ Це НЕ «здоров'я АКБ». Порівняння dumps/ показало, що запис @0x129 побайтово
// однаковий у всіх екземплярів моделі (19×PMNN4409A, 8×PMNN4409B), тобто це
// заводська константа, а байт +21 у ній завжди 0x64. Рація свій «строк служби»
// рахує сама й на цей байт не дивиться — правка тут показань станції НЕ змінить.
// Функцію лишено для ручного аналізу (власник просив зберегти ручний режим);
// у відповіді про це попереджаємо явно.
void handleSetCapacity() {
    if (!requireAdmin()) return;
    if (!hasDump) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return; }
    if (!server.hasArg("cap")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No cap\"}"); return; }
    int cap = server.arg("cap").toInt();
    if (cap < 0) cap = 0; if (cap > 100) cap = 100;

    // Запис на ФІКСОВАНОМУ місці 0x129 (раніше шукали «тег 0x17 + 0x00», хоча
    // перший байт запису — довжина; пошук міг влучити в інший запис).
    const int rec = IMPRES_FACTORY_REC;
    if (!impresRecordOk(batteryDump, rec) || batteryDump[rec] != 0x17) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Заводську таблицю @0x129 не знайдено або вона пошкоджена\"}"); return;
    }

    batteryDump[rec + 21] = (uint8_t)cap;
    impresFixRecord(batteryDump, rec, 0x17);   // контрольна сума запису (Σ≡0x5A)

    ledSet(LED_WRITE); displayShow("ЗАПИС ЄМН...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) { saveDump("/dump.bin", batteryDump, DUMP_SIZE); displayShow("ЄМН. OK"); }
    else displayShow("ЄМН. ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Записано в заводську таблицю @0x129. Увага: рація рахує строк служби сама і цей байт не використовує — показання станції не зміняться.\"}"
           : "{\"status\":\"error\",\"message\":\"Write failed\"}");
}

// Змінити залишкову ємність (заряд) в мА·ч і записати в DS2438. Пише регістр
// ICA (байт 12) як паливомір ВІДНОСНО паспортної ємності моделі:
// ICA = 255 * mAh / ratedMah. Раніше ділили на DS2438_MAH_PER_LSB, і повна
// шкала виходила ~4978 мА·год — більше за сам пакет.
void handleSetMah() {
    if (!requireAdmin()) return;
    if (!hasDump2438) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return; }
    if (!server.hasArg("mah")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No mah\"}"); return; }
    long mah = server.arg("mah").toInt();
    char smModel[16] = "";
    if (hasDump) impresModelName(batteryDump, smModel, sizeof(smModel));
    long ica = impresIcaFromMahRs(mah, impresRatedMahFor(hasDump ? batteryDump : nullptr, smModel),
                                  impresBmsRsense(batteryDump2438));
    batteryDump2438[12] = (uint8_t)ica;
    ledSet(LED_WRITE); displayShow("ЗАПИС ЄМН mAh");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    displayShow(ok ? "ЄМН mAh OK" : "ЄМН mAh ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    String m = String("{\"status\":\"") + (ok ? "success" : "error") +
               "\",\"ica\":" + ica + ",\"mah\":" +
               impresIcaToMahRs((uint8_t)ica, impresRatedMahFor(hasDump ? batteryDump : nullptr, smModel),
                                impresBmsRsense(batteryDump2438)) + "}";
    server.send(ok ? 200 : 500, "application/json", m);
}

// Рівень заряду з поточної напруги — за шкалою BATTERY_EMPTY_MV..BATTERY_FULL_MV
// (settings.h), лінійно.
inline int chargePctFromVoltage() {
    // Спільна шкала з batteryPercent() і підготовкою до калібрування
    // (BATTERY_EMPTY_MV..BATTERY_FULL_MV з settings.h).
    return impresPercentFromMv((int)impresVoltageMv(batteryDump2438));
}
// Записати рівень заряду у % в регістр ICA (DS2438[12]).
inline bool performSetChargePct(int pct) {
    if (!hasDump2438) return false;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    // Апаратна шкала паливоміра (див. impres_format.h): 100 % — це не 255, а
    // стільки одиниць, скільки важить повний пакет за його ж шунтом.
    char cm[16] = ""; decodeModel(cm, sizeof(cm));
    uint8_t ica = impresIcaFromPercentRs(pct,
                      impresRatedMahFor(hasDump ? batteryDump : nullptr, cm),
                      impresBmsRsense(batteryDump2438));
    batteryDump2438[12] = ica;
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    return ok;
}
// Виставити рівень заряду (ICA): auto=1 — з напруги (BATTERY_SCALE_TXT),
// або вручну pct=0..100. Зарядка/рація потім самі уточнять це значення.
void handleSetCharge() {
    if (!requireAdmin()) return;
    if (!hasDump2438) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return; }
    int pct;
    if (server.hasArg("auto") && server.arg("auto") == "1") pct = chargePctFromVoltage();
    else if (server.hasArg("pct")) pct = server.arg("pct").toInt();
    else { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No pct/auto\"}"); return; }
    ledSet(LED_WRITE); displayShow("ЗАПИС ЗАРЯДУ");
    bool ok = performSetChargePct(pct);
    displayShow(ok ? "ЗАРЯД OK" : "ЗАРЯД ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    server.send(ok ? 200 : 500, "application/json",
        String("{\"status\":\"") + (ok ? "success" : "error") + "\",\"pct\":" + pct +
        ",\"ica\":" + batteryDump2438[12] + "}");
}

// Змінити ETM (наробіток, DS2438[8..11], сек) — так рація показує «дату першого
// користування» = (її поточний час − ETM). Клієнт рахує sec = (сьогодні − цільова
// дата) і надсилає сюди. Пише лише DS2438.
void handleSetEtm() {
    if (!requireAdmin()) return;
    if (!hasDump2438) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Спочатку зчитайте АКБ\"}"); return; }
    if (!server.hasArg("sec")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No sec\"}"); return; }
    // toInt() -> long може не вмістити >2^31; беремо як unsigned через strtoul.
    uint32_t sec = (uint32_t)strtoul(server.arg("sec").c_str(), nullptr, 10);
    batteryDump2438[8]  = sec & 0xFF;
    batteryDump2438[9]  = (sec >> 8) & 0xFF;
    batteryDump2438[10] = (sec >> 16) & 0xFF;
    batteryDump2438[11] = (sec >> 24) & 0xFF;
    ledSet(LED_WRITE); displayShow("ЗАПИС ДАТИ");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    displayShow(ok ? "ДАТА OK" : "ДАТА ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    server.send(ok ? 200 : 500, "application/json",
        ok ? (String("{\"status\":\"success\",\"etmSec\":") + sec + "}")
           : "{\"status\":\"error\",\"message\":\"Помилка запису\"}");
}

// Універсальна запис сирих байт з браузера. Аргументи: target=2433|2438,
// data=hex-рядок (512 або 64 байта), autofix=1 (для 2433 — перерахунок контр.
// суми заголовка і синхронізація дзеркала). Дозволяє змінювати Будь-які дані і
// писати їх в АКБ прямо з веб-редактора.
static int hexToBytes(const String &s, uint8_t *out, int maxn) {
    int n = 0; int hi = -1;
    for (size_t i = 0; i < s.length() && n < maxn; i++) {
        char c = s[i]; int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;                       // пропускаємо пробіли/переводи рядків
        if (hi < 0) hi = v; else { out[n++] = (hi << 4) | v; hi = -1; }
    }
    return n;
}

void handleWriteHex() {
    if (!requireAdmin()) return;
    String target = server.arg("target");
    String data   = server.arg("data");
    bool autofix  = server.arg("autofix") == "1";
    bool is38 = (target == "2438");
    int need = is38 ? DS2438_MEM_SIZE : DUMP_SIZE;

    static uint8_t buf[DUMP_SIZE];
    int got = hexToBytes(data, buf, need);
    if (got != need) {
        String m = String("{\"status\":\"error\",\"message\":\"Expected ") + need + " bytes, got " + got + "\"}";
        server.send(400, "application/json", m); return;
    }

    ledSet(LED_WRITE); displayShow(is38 ? "ЗАПИС HEX 2438" : "ЗАПИС HEX 2433");
    bool ok;
    if (is38) {
        ok = battery.writeDS2438(buf, DS2438_MEM_SIZE);
        if (ok) { memcpy(batteryDump2438, buf, DS2438_MEM_SIZE); hasDump2438 = true; saveDump("/dump2438.bin", buf, DS2438_MEM_SIZE); }
    } else {
        if (autofix) {
            fixHeaderChecksum(buf);
            if (hasDump2438 && mirrorSourceValid(batteryDump2438)) { /* держим зеркало согласованным (не для R7 без дзеркала) */ for (int i=0;i<26;i++) buf[1+i]=batteryDump2438[24+i]; fixHeaderChecksum(buf); }
        }
        ok = battery.writeBattery(buf, DUMP_SIZE);
        if (ok) { memcpy(batteryDump, buf, DUMP_SIZE); hasDump = true; saveDump("/dump.bin", buf, DUMP_SIZE); }
    }
    displayShow(ok ? "HEX ЗАПИС OK" : "HEX ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Bytes written\"}"
           : "{\"status\":\"error\",\"message\":\"Write failed\"}");
}

// Веб-обробник скидання (под паролем).
void handleResetBattery() {
    if (!requireAdmin()) return;
    if (!hasDump && !hasDump2438) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}");
        return;
    }
    // Без ROM-ID скидання свідомо не робиться (див. resetBatteryData): краще
    // відмова з поясненням, ніж пакет із розсинхронізованими чипами.
    if (hasDump && !hasSN2433) {
        server.send(400, "application/json",
            "{\"status\":\"error\",\"message\":\"ROM-ID чипа DS2433 невідомий. "
            "Історія в DS2433 зашифрована ключем із нього, і без ключа її не обнулити. "
            "Обнулити самі лічильники монітора не можна: пакет із порожнім монітором "
            "проти повного історії DS2433 рація читає як «невідомий акумулятор». "
            "Перечитайте АКБ на пристрої й повторіть.\"}");
        return;
    }
    bool ok = performReset();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Лічильники обнулено в ОБОХ чипах "
             "(дата виготовлення збережена)\"}"
           : "{\"status\":\"error\",\"message\":\"Failed to write reset\"}");
}

// Веб-обробник підготовки до рекалібрування (після заміни елементів), під паролем.
void handleRecalPrepare() {
    if (!requireAdmin()) return;
    if (!hasDump && !hasDump2438) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Спочатку зчитайте АКБ\"}"); return;
    }
    // deep=1 — додатково стерти навчені записи ємності й журнал використання
    // (ручний режим, коли після звичайної підготовки ЗП тримає стару ємність).
    bool deep = server.hasArg("deep") && server.arg("deep") == "1";
    bool ok = performRecalPrepare(deep);
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Готово. Тепер поставте АКБ на IMPRES-ЗП для калібрування.\"}"
           : "{\"status\":\"error\",\"message\":\"Помилка запису\"}");
}

// ── ПЕРЕНЕСЕННЯ АПАРАТНОГО КАЛІБРУВАННЯ МОНІТОРА ───────────────────────────
//  Два поля DS2438 налаштовані на ЗАВОДІ під конкретний екземпляр і в шаблоні
//  належать ІНШОМУ пакету:
//    0x0D..0x0E — OFFSET АЦП струму;
//    0x38..0x39 — вимірювальний резистор (шунт), значення/100000 = Ом.
//  Шунт у різних моделей відрізняється майже вдвічі (0.025 проти 0.046 Ом), тож
//  чужий шунт робить неправильними струм, залишок і знос — рівно та пастка, що
//  зловилась на APLI4811C (dumps/14-r7-4807a-4811c/README.md).
//  Тому: якщо в самому пакеті ці поля правдоподібні — лишаємо ЙОГО значення.
//  dst — те, що збираємось писати; src — те, що зараз у чипі (або nullptr).
static void keepMonitorCalibration(uint8_t *dst, const uint8_t *src) {
    if (!dst || !src) return;
    if (impresBmsRsense(src) > 0.0f) { dst[56] = src[56]; dst[57] = src[57]; }
    uint16_t off = (uint16_t)((src[0x0E] << 8) | src[0x0D]);
    if (off != 0x0000 && off != 0xFFFF) { dst[0x0D] = src[0x0D]; dst[0x0E] = src[0x0E]; }
}

// ── ЧИ ПРИДАТНИЙ ВЛАСНИЙ МОНІТОР ПАКЕТА ────────────────────────────────────
//  Еталон DS2438 у templates.h — теж побайтова копія ОДНОГО пакета. Замінюючи
//  ним монітор робочого акумулятора, ми віддаємо йому чужі заводські поля:
//  крім шунта й OFFSET АЦП (їх повертає keepMonitorCalibration) у DS2438 є
//  байти 0x10..0x17, 0x32..0x35 і 0x3A..0x3B, значення яких ми не розбирали, —
//  і вони теж ставали донорськими. На реальній парі це 20 чужих байтів із 64.
//
//  Саме так виглядає скарга «нова рація не бачить АКБ, стара бачить, але заряд
//  неадекватний»: ідентичність (DS2433) правильна, а монітор — від іншого
//  екземпляра.
//
//  Тому шаблон монітора потрібен ЛИШЕ тоді, коли свого немає ЗОВСІМ: чіп не
//  читається або порожній (усе 0x00/0xFF). У всіх інших випадках монітор пакета
//  лишається СВОЇМ, а ми тільки обнуляємо лічильники й виставляємо паливомір —
//  тобто робимо рівно те, що обіцяє операція.
//
//  ⚠️ Відсутній ШУНТ (0x38..0x39 = 0) сюда НЕ входить. Спершу він теж вважався
//  причиною замінити монітор цілком — і на пакеті власника саме так і сталося:
//  шунт у чипі був нульовий, тож у монітор поїхала копія донора разом із
//  0x32..0x35 і 0x3A..0x3B, і рація далі не приймала пакет. Порожнє поле треба
//  ЗАПОВНИТИ, а не міняти через нього всі 64 байти.
static bool monitorIsOwn(const uint8_t *d38) {
    if (!d38) return false;
    bool allZero = true, allFF = true;
    for (int i = 0; i < DS2438_MEM_SIZE; i++) {
        if (d38[i] != 0x00) allZero = false;
        if (d38[i] != 0xFF) allFF = false;
    }
    return !allZero && !allFF;
}

// Шунт — заводське значення КОНКРЕТНОГО екземпляра, відновити його нізвідки.
// Якщо в пакеті його немає, беремо з еталона моделі: він хоч і чужий, зате того
// самого порядку (у родині 4409 це 0.045..0.046 Ом проти 0.025 в інших), а без
// шунта струм, залишок і знос не рахуються взагалі. Це свідомий компроміс, і він
// має бути ГУЧНИМ у лозі, а не тихою підміною.
static bool fillMissingRsense(uint8_t *d38, const uint8_t *tpl38) {
    if (!d38 || !tpl38) return false;
    if (impresBmsRsense(d38) > 0.0f) return false;         // свій є — не чіпаємо
    if (impresBmsRsense(tpl38) <= 0.0f) return false;      // і в еталона немає
    d38[56] = tpl38[56]; d38[57] = tpl38[57];
    Serial.printf("Restore: у пакеті НЕМАЄ заводського шунта — узято з еталона "
                  "моделі (%.5f Ом). Струм і залишок будуть приблизними.\n",
                  impresBmsRsense(d38));
    return true;
}

// ------------------- Ініціалізація нового акумулятора -------------------
// Порожній/стертий/невідомий чіп -> робочий АКБ обраної моделі. Вантажимо
// вшитий genuine-еталон (DS2433 + DS2438), зануляємо всю історію/лічильники
// (як заводська очистка), ставимо здоров'я 100%, а введену вручну ємність у
// мА·год пишемо в ICA (поточний заряд). Дзеркало калібрування DS2438<->DS2433
// у шаблоні вже узгоджене. Пишемо ОБИДВІ мікросхеми.
// Чи був чип ПОРОЖНІЙ до відновлення: усе 0xFF (стертий) або все 0x00 (новий /
// не читався). Тільки в цьому випадку ідентичність можна генерувати наново — на
// пакеті з даними це знищило б його справжню історію.
static bool dumpIsBlank(const uint8_t *d33) {
    if (!d33) return true;
    bool allFF = true, all00 = true;
    for (int i = 0; i < DUMP_SIZE; i++) {
        if (d33[i] != 0xFF) allFF = false;
        if (d33[i] != 0x00) all00 = false;
        if (!allFF && !all00) return false;
    }
    return true;
}

// Чи згенерувалась ідентичність під ROM цього чипа в останньому «новому АКБ» —
// щоб клієнт міг це показати, а не лише Serial-лог.
static bool g_initIdentityGen = false;
static int  g_initIdentityDate = 0;      // РРРРММДД, 0 — годинник не заведено

bool performInitBattery(const char *model, long mah) {
    int t = findTemplate(model);
    if (t < 0) { displayShow("НЕМА ШАБЛОНУ"); return false; }

    Serial.printf("\n=== Init new battery: %s, %ld mAh ===\n", model, mah);
    // Шунт і OFFSET АЦП цього пакета — до того, як затремо буфер шаблоном.
    uint8_t was38[DS2438_MEM_SIZE];
    bool had38 = hasDump2438;
    if (had38) memcpy(was38, batteryDump2438, DS2438_MEM_SIZE);

    memcpy_P(batteryDump, BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
    // Для частини моделей еталона монітора немає (d38 = nullptr): тоді монітор
    // пакета не підмінюємо, а лише зануляємо лічильники нижче.
    if (BATTERY_TEMPLATES[t].d38) {
        memcpy_P(batteryDump2438, BATTERY_TEMPLATES[t].d38, DS2438_MEM_SIZE);
        if (had38) keepMonitorCalibration(batteryDump2438, was38);
    } else if (!had38) {
        memset(batteryDump2438, 0, DS2438_MEM_SIZE);   // чистий монітор із нуля
    }
    hasDump = true; hasDump2438 = true;

    // ⚑ КЛЮЧОВЕ. Шаблон у templates.h — це побайтова копія ОДНОГО реального
    // АКБ, разом із його навченим калібруванням. Записати його цілком = віддати
    // новому пакету ЧУЖІ виміряні дані про банки; саме так і виходив «невідомий
    // акумулятор» (dumps/08-nova-batareya: шаблон 4409A побайтово дорівнює
    // робочому 02-katalog-osnovnyi/10_PMNN4409A, і рація його не прийняла).
    // Тому з шаблону лишаємо ЛИШЕ модельну частину, а навчений хвіст стираємо.
    int cleared = applyFreshTail(BATTERY_TEMPLATES[t].fresh);
    if (cleared < 0) {                              // немає перевіреного шаблону
        cleared = impresEraseTail(batteryDump);     // хоча б не чужі дані
        Serial.printf("INIT: no fresh tail for %s, tail erased: %d B "
                      "(калібрування на ЗП може не завершитись)\n", model, cleared);
    } else {
        Serial.printf("INIT: fresh (unlearned) tail written: %d B\n", cleared);
    }
    // Свіжий стан: зануляємо лічильники/історію/статистику, лишаємо
    // ідентичність/криву/дзеркало.
    //
    // ⚑ Результат тут свідомо не перевіряємо, і ось чому. factoryCleanData()
    //  відмовляється працювати без ROM-ID, щоб не лишити чипи
    //  розсинхронізованими. Але «новий АКБ» — не той випадок: нижче йде
    //  impresIdentityWrite(), яка переписує ВСЮ ідентичність під ключ цього
    //  чипа, а слідом impresCyclesWrite(0)/impresNonImpresWrite(0) обнуляють
    //  нешифровані гістограми. Тобто узгодженість тут забезпечує не очистка, а
    //  повний перезапис. Якщо ж ROM невідомий, то й ідентичність не запишеться
    //  — про це користувачу кажуть прямим текстом у відповіді обробника.
    (void)factoryCleanData();
    impresFixHeader(batteryDump);

    // ⚑ ІДЕНТИЧНІСТЬ ГЕНЕРУЄМО, А НЕ КОПІЮЄМО. Зашифровані блоки еталона
    // (дати, знос, калібрування) зашифровані ROM-ом ДОНОРА: рація розшифрує їх
    // СВОЇМ ключем і побачить сміття — це і є «невідомий акумулятор»
    // (dumps/16-verbatim-4409a-chuzhyi-kliuch). Тому беремо ROM-ID цього чипа
    // (він і є серійним номером пакета), із нього — ключі, і під ними пишемо
    // свіжі числа: дата виготовлення = сьогодні, пакет ще не вмикали,
    // лічильники нульові, потенційна ємність = паспортна.
    int gy = 0, gm = 0, gd = 0;
    deviceClockToday(&gy, &gm, &gd);          // 0, якщо годинник не заведено
    int ratedNew = impresRatedMahFor(batteryDump, model);
    uint8_t ctsNew = restoreCtsFromHealth(100, ratedNew, impresBmsRsense(batteryDump2438));
    g_initIdentityGen = false;
    g_initIdentityDate = 0;
    if (hasSN2433 && impresIdentityWrite(batteryDump, chipSN2433, gy, gm, gd, ctsNew)) {
        g_initIdentityGen = true;
        g_initIdentityDate = (int)restoreDateNum(gy, gm, gd);
        Serial.printf("INIT: ідентичність згенеровано під ROM %02X%02X…: ключі %02X/%02X, "
                      "дата %04d-%02d-%02d, CTS %u\n",
                      chipSN2433[0], chipSN2433[1], chipSN2433[1], chipSN2433[6],
                      gy, gm, gd, (unsigned)ctsNew);
        if (!gy) Serial.println("INIT: годинник не заведено — блок DATE лишено як є");
    } else {
        Serial.println("INIT: ROM DS2433 невідомий — ідентичність НЕ згенеровано, "
                       "у чипі лишились зашифровані поля донора (рація побачить сміття)");
    }
    // Лічильники циклів ключа не потребують — обнуляємо їх окремо, інакше
    // новий пакет успадкував би гістограму донора.
    impresCyclesWrite(batteryDump, 0);
    impresNonImpresWrite(batteryDump, 0);
    impresFixHeader(batteryDump);

    // Монітор — у стан «новий пакет» (конфіг/поріг/дзеркало зберігаються).
    // Введена ємність (поточний заряд) у мА·год -> регістр ICA DS2438.
    long ica = impresIcaFromMahRs(mah, ratedNew, impresBmsRsense(batteryDump2438));
    impresResetMonitor(batteryDump2438, batteryDump, (uint8_t)ica);

    ledSet(LED_WRITE); displayShow("НОВИЙ АКБ...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) {
        saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        displayShow("НОВИЙ АКБ OK");
    } else displayShow("НОВИЙ АКБ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Init completed ===\n");
    return ok;
}

// ------------------- Відновлення еталона моделі -------------------
// Пише вшитий еталон DS2433+DS2438 на порожній/битий чип.
//
// ⚠️ ЗА ЗАМОВЧУВАННЯМ хвіст навченого калібрування (0x18A..0x1FF) НЕ пишеться,
// а лишається стертим. Шаблони в templates.h — побайтові копії конкретних
// робочих АКБ, і їхній хвіст описує ЧУЖІ банки. Побайтове відновлення давало
// саме те, на що скаржився власник: рація «невідомий акумулятор», ЗП світить
// зеленим і не заряджає (dumps/08-nova-batareya). Модельна частина (0x000..0x189
// — ідентичність, крива, copyright, заводська таблиця, запис моделі) у всіх
// екземплярів моделі однакова, тож саме її й переносимо.
//
// verbatim = true — записати шаблон байт-у-байт, разом із чужим хвостом. Це
// РУЧНИЙ режим для аналізу/експериментів; для ремонту не використовувати.
//
// Працює на ПОРОЖНЬОМУ (усе 0xFF) чи БИТОМУ чіпі: перезапис повністю замінює
// вміст, а ROM шукається по шині (Search/Match ROM), попереднє читання не треба.
// Кожен чіп пишемо НЕЗАЛЕЖНО й повертаємо true, якщо записався хоча б DS2433
// (ідентичність); фактичний стан кожного — в ok33/ok38 і Serial-лог.
//
// plan — правки під конкретний пакет (restore_plan.h). nullptr = типовий набір,
// порахований тут же з того, що зараз у буферах.
// Режим навченого хвоста 0x18A..0x1FF.
//
// ⚑ ЧОМУ ЦЕ ВИБІР, А НЕ КОНСТАНТА. Обидва варіанти перевірені на дампах
// власника, і кожен має свою ціну:
//
//   RTAIL_FRESH — «свіжий» хвіст: скелет записів на місці, навчені значення
//     обнулені, суми правильні. Калібрування на ЗП може завершитись, бо ЗП
//     пише навчені значення за фіксованими адресами й структуру НЕ створює.
//     Але ЗП бачить валідний навчений блок і на всіх ремонтованих пакетах
//     (dumps/06,07,08,15) світила зеленим і не заряджала.
//
//   RTAIL_ERASE — хвіст стертий у 0xFF. Рівно в цьому стані ЗП починала
//     заряджати й переходити в калібрування — тричі незалежно
//     (dumps/11,12,13). Ціна: у dumps/13 після повного циклу з'явилися лише
//     два байти (0x1E1,0x1E2), а байт довжини 0x1E0 лишився 0xFF, тобто
//     навчений блок так і не став валідним — калібрування не завершується.
enum { RTAIL_FRESH = 0, RTAIL_ERASE = 1 };

static int tailModeFromArg(const String &v) {
    String t = v; t.trim(); t.toLowerCase();
    return (t == "erase" || t == "1") ? RTAIL_ERASE : RTAIL_FRESH;
}

bool performRestoreTemplate(const char *model, bool *ok33 = nullptr, bool *ok38 = nullptr,
                            bool verbatim = false, const RestorePlan *plan = nullptr,
                            int tailMode = RTAIL_FRESH) {
    if (ok33) *ok33 = false;
    if (ok38) *ok38 = false;
    int t = findTemplate(model);
    if (t < 0) { displayShow("НЕМА ШАБЛОНУ"); return false; }

    Serial.printf("\n=== Restore %s: %s ===\n", verbatim ? "VERBATIM" : "model-part", model);
    uint8_t was38[DS2438_MEM_SIZE];
    bool had38 = hasDump2438;
    if (had38) memcpy(was38, batteryDump2438, DS2438_MEM_SIZE);
    // Чи був чип порожній — питаємо ДО того, як шаблон затре буфер: після
    // memcpy_P там уже лежить еталон, і відповідь була б завжди «ні».
    bool wasBlank33 = !hasDump || dumpIsBlank(batteryDump);

    // ⚑ План складаємо ДО того, як шаблон затре буфери: після memcpy_P у
    // batteryDump2438 лежить монітор ДОНОРА, і все, пораховане з нього, буде
    // числами донора. Саме на цьому раніше й горіло: рівень заряду брався з
    // напруги, зашитої в шаблоні, тож у розряджений пакет писалось «повний».
    RestorePlan localPlan;
    if (!plan && !verbatim) {
        restorePlanBuild(localPlan, model,
                         BATTERY_TEMPLATES[t].d33, BATTERY_TEMPLATES[t].d38,
                         hasDump ? batteryDump : nullptr,
                         had38 ? was38 : nullptr,
                         hasSN2433 ? chipSN2433 : nullptr);
        // Дата — із системного годинника пристрою: цей шлях і є «відновлення
        // з меню приладу», де клієнта немає й передати її нікому.
        int cy, cm, cd;
        if (deviceClockToday(&cy, &cm, &cd)) restorePlanSetToday(localPlan, cy, cm, cd);
        plan = &localPlan;
    }

    memcpy_P(batteryDump, BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
    // ⚑ Монітор пакета НЕ підмінюємо, поки він свій і живий (див. monitorIsOwn):
    // еталон DS2438 приносить із собою 20 чужих байтів, і для рації це вже
    // інший акумулятор. Шаблон монітора беремо лише на порожньому/битому чипі.
    bool ownMon  = !verbatim && had38 && monitorIsOwn(was38);
    bool write38 = (BATTERY_TEMPLATES[t].d38 != nullptr) && !ownMon;
    if (write38) {
        memcpy_P(batteryDump2438, BATTERY_TEMPLATES[t].d38, DS2438_MEM_SIZE);
        // verbatim — це ручний режим «байт-у-байт», там не втручаємось
        if (!verbatim && had38) keepMonitorCalibration(batteryDump2438, was38);
    }
    if (ownMon) {
        Serial.println("Restore: монітор пакета власний — лишаємо його, "
                       "еталон DS2438 не пишемо");
        // Єдине, чого може не бути у своєму моніторі, — шунт. Доливаємо ЛИШЕ його,
        // і лише коли власник не задав опір сам: явний вибір людини (вручну або
        // з бібліотеки еталонів) головніший за наш запасний варіант.
        bool rsChosen = plan && plan->fx[RPF_RSENSE].on && plan->fx[RPF_RSENSE].useVal > 0;
        if (BATTERY_TEMPLATES[t].d38 && !rsChosen) {
            uint8_t tpl38[DS2438_MEM_SIZE];
            memcpy_P(tpl38, BATTERY_TEMPLATES[t].d38, DS2438_MEM_SIZE);
            fillMissingRsense(batteryDump2438, tpl38);
        }
    }
    if (!verbatim) {
        int cleared;
        if (tailMode == RTAIL_ERASE) {
            cleared = impresEraseTail(batteryDump);
            Serial.println("Restore: навчений хвіст СТЕРТО (0xFF) — ЗП почне заряджати, "
                           "але калібрування може не завершитись");
        } else {
            cleared = applyFreshTail(BATTERY_TEMPLATES[t].fresh);
            if (cleared < 0) cleared = impresEraseTail(batteryDump);
        }
        impresFixHeader(batteryDump);
        bool touch38 = write38 || had38;
        if (touch38) impresResetMonitor(batteryDump2438, batteryDump, plan->icaUse);
        // Правки — ПІСЛЯ скидання монітора: воно обнуляє наробіток і паливомір.
        restorePlanApply(*plan, batteryDump, touch38 ? batteryDump2438 : nullptr);
        // ⚑ Порожній чип: ідентичність ГЕНЕРУЄМО з його ROM, а не лишаємо
        // донорську. Інакше зашифровані блоки еталона лишились би під ключем
        // донора, і рація, розшифрувавши їх своїм, побачила б сміття —
        // «невідомий акумулятор» (dumps/16). На пакеті З ДАНИМИ так робити не
        // можна: це знищило б його справжню історію, тож умова строга —
        // порожньо було до відновлення.
        if (wasBlank33 && hasSN2433) {
            int gy = 0, gm = 0, gd = 0;
            deviceClockToday(&gy, &gm, &gd);
            uint8_t cts = restoreCtsFromHealth(100, impresRatedMahFor(batteryDump, model),
                                               impresBmsRsense(batteryDump2438));
            impresIdentityWrite(batteryDump, chipSN2433, gy, gm, gd, cts);
            impresCyclesWrite(batteryDump, 0);
            impresNonImpresWrite(batteryDump, 0);
            impresFixHeader(batteryDump);
            Serial.printf("Restore: чип був порожній -> ідентичність згенеровано під ROM "
                          "(ключі %02X/%02X, дата %04d-%02d-%02d, CTS %u)\n",
                          chipSN2433[1], chipSN2433[6], gy, gm, gd, (unsigned)cts);
        }
        char pl[96] = "";
        for (int i = 0, k = 0; i < RPF_COUNT; i++)
            if (plan->fx[i].on) k += snprintf(pl + k, sizeof(pl) - k, "%s%s",
                                              k ? "," : "", RESTORE_FIX_DOC[i].key);
        Serial.printf("Restore: donor learned tail cleared: %d B; fixes: %s; ICA=%u\n",
                      cleared, pl[0] ? pl : "(немає)", plan->icaUse);
    }

    ledSet(LED_WRITE); displayShow("ВІДНОВЛ. ЕТАЛОН");
    bool w33 = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (w33) { hasDump = true; saveDump("/dump.bin", batteryDump, DUMP_SIZE); }
    // Монітор пишемо лише якщо є що писати: коли еталона DS2438 для моделі
    // немає, чіпати монітор пакета не можна — у ньому його власний шунт.
    bool w38 = (write38 || had38) ? battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE) : true;
    if (w38 && (write38 || had38)) { hasDump2438 = true; saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE); }

    if (ok33) *ok33 = w33;
    if (ok38) *ok38 = w38;
    bool ok = w33;                       // ідентичність критична; DS2438 може бути відсутнім
    displayShow(w33 && w38 ? "ЕТАЛОН OK" : (w33 ? "2438 ЗБІЙ" : "ЕТАЛОН ЗБІЙ"));
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.printf("Restore: DS2433=%s DS2438=%s\n", w33 ? "OK" : "FAIL", w38 ? "OK" : "FAIL");
    Serial.println("=== Restore completed ===\n");
    return ok;
}

// ---- План правок еталона під конкретний пакет ------------------------------
// Скласти план для моделі з того, що зараз у буферах. refresh=true — спершу
// перечитати чипи: напруга (а з нею й рівень заряду) міняється, і показувати
// користувачеві заряд півгодинної давнини означало б давати йому підтвердити
// не те число, яке запишеться.
static bool buildRestorePlanFor(const char *model, RestorePlan &p, bool refresh) {
    int t = findTemplate(model);
    if (t < 0) return false;
    if (refresh) { bool a, b; readAllChips(a, b); }
    // ROM DS2433 потрібен для правки шифрування: ключ береться з нього. Для
    // дампа, відкритого з файлу, ROM невідомий — і правку ми не пропонуємо.
    restorePlanBuild(p, model, BATTERY_TEMPLATES[t].d33, BATTERY_TEMPLATES[t].d38,
                     hasDump ? batteryDump : nullptr,
                     hasDump2438 ? batteryDump2438 : nullptr,
                     hasSN2433 ? chipSN2433 : nullptr);
    // «Сьогодні» беремо з СИСТЕМНОГО годинника пристрою — тоді наробіток
    // рахується і там, де клієнта немає взагалі: у меню самого приладу й у
    // Майстрі, запущеному з екрана. Клієнт, якщо прийде, перекриє це своєю
    // датою (і заодно заведе годинник) — див. restorePlanOverride().
    int cy, cm, cd;
    if (deviceClockToday(&cy, &cm, &cd)) restorePlanSetToday(p, cy, cm, cd);
    return true;
}

static String restorePlanJson(const RestorePlan &p) {
    char a[24], b[24], c[24];
    String j = "{\"model\":\""; j += p.model;
    j += "\",\"tpl38\":";   j += p.haveTpl38 ? "true" : "false";
    j += ",\"pack33\":";    j += p.havePack33 ? "true" : "false";
    j += ",\"pack38\":";    j += p.havePack38 ? "true" : "false";
    j += ",\"packMv\":";    j += (int)p.packMv;
    j += ",\"tplMv\":";     j += (int)p.tplMv;
    j += ",\"packPct\":";   j += p.packPct;
    j += ",\"ratedMah\":";  j += p.ratedMah;      // ефективна — за нею паливомір
    j += ",\"ratedTpl\":";  j += p.ratedTpl;
    j += ",\"ratedPack\":"; j += p.ratedPack;
    j += ",\"ratedUser\":"; j += p.ratedUser;
    j += ",\"ratedStep\":"; j += IMPRES_RATED_STEP;
    j += ",\"ratedMin\":";  j += IMPRES_RATED_MIN_MAH;
    j += ",\"ratedMax\":";  j += IMPRES_RATED_MAX_MAH;
    // Шунт — у «сирих» одиницях чипа (Ом×100000 == мОм×100), щоб клієнт не
    // ганяв дроби туди-сюди і не втрачав сотих на округленні.
    j += ",\"rsPack\":";    j += (int)p.rsRawPack;
    j += ",\"rsTpl\":";     j += (int)p.rsRawTpl;
    j += ",\"rsUser\":";    j += (int)p.rsUser;
    j += ",\"rsSrc\":\"";   j += p.rsSrc;
    j += "\",\"rsMin\":";   j += (int)RP_RS_MIN_RAW;
    j += ",\"rsMax\":";     j += (int)RP_RS_MAX_RAW;
    // Бібліотека: у кожної вшитої моделі свій шунт — його можна взяти звідси,
    // коли в самому пакеті шунта немає.
    // Шифрування: чи відомий ROM, чи чужий ключ, які дати бачить рація й які
    // насправді. Дати — числом YYYYMMDD, щоб клієнти не парсили рядки.
    j += ",\"haveRom\":";   j += p.haveRom ? "true" : "false";
    j += ",\"cryptWrong\":";j += p.cryptWrong ? "true" : "false";
    j += ",\"cryptSrcOk\":";j += p.cryptSrcOk ? "true" : "false";
    j += ",\"cryptUnknown\":"; j += p.cryptUnknown ? "true" : "false";
    j += ",\"mfgSeen\":";   j += restoreDateNum(p.seenY, p.seenM, p.seenD);
    j += ",\"mfgReal\":";   j += restoreDateNum(p.mfgY, p.mfgM, p.mfgD);
    j += ",\"mfgUser\":";   j += restoreDateNum(p.mfgUserY, p.mfgUserM, p.mfgUserD);
    // Знос: що бачить рація зараз, що там насправді, що вписали руками, і
    // байт CTS, який реально піде в чип.
    j += ",\"hpSeen\":";    j += p.healthSeen;
    j += ",\"hpReal\":";    j += p.healthReal;
    j += ",\"hpUser\":";    j += p.healthUser;
    j += ",\"ctsUse\":";    j += (int)p.ctsUse;
    // Дата першого запуску, калібрування й лічильники циклів. Дати — числом
    // YYYYMMDD; -1 у лічильниках означає «не вписували» (0 — повноцінне число).
    j += ",\"useSeen\":";   j += p.useSeen;
    j += ",\"useReal\":";   j += p.useReal;
    j += ",\"useUser\":";   j += restoreDateNum(p.useUserY, p.useUserM, p.useUserD);
    j += ",\"calSeen\":";   j += p.calSeen;
    j += ",\"calReal\":";   j += p.calReal;
    j += ",\"calUser\":";   j += p.calUser;
    j += ",\"cycNow\":";    j += p.cycNow;
    j += ",\"cycUser\":";   j += p.cycUser;
    j += ",\"nonNow\":";    j += p.nonNow;
    j += ",\"nonUser\":";   j += p.nonUser;
    // Наробіток: свій (у моніторі), порахований із дати першого запуску, від
    // якої дати рахували і яку «сьогодні» нам передав клієнт.
    j += ",\"etmPack\":";   j += p.etmPack;
    j += ",\"etmCalc\":";   j += p.etmCalc;
    j += ",\"etmUseDate\":";j += p.etmUseDate;
    j += ",\"etmFromUse\":";j += p.etmFromUse ? "true" : "false";
    j += ",\"today\":";     j += restoreDateNum(p.todayY, p.todayM, p.todayD);
    // Звідки взялась дата пристрою: none — годинник не заведено, saved —
    // відновлена після перезавантаження (відстає), client — щойно від клієнта.
    j += ",\"todaySrc\":\""; j += deviceClockSrcName(); j += "\"";
    j += ",\"rsLib\":[";
    bool first = true;
    for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++) {
        uint16_t raw = templateRsenseRaw(i);
        if (!raw) continue;                     // без монітора нема чого пропонувати
        if (!first) j += ",";
        first = false;
        j += "{\"model\":\""; j += BATTERY_TEMPLATES[i].name;
        j += "\",\"raw\":";   j += (int)raw;
        j += "}";
    }
    j += "]";
    j += ",\"icaTpl\":";    j += (int)p.icaTpl;
    j += ",\"icaPack\":";   j += (int)p.icaPack;
    j += ",\"icaUse\":";    j += (int)p.icaUse;
    j += ",\"fixes\":[";
    for (int i = 0; i < RPF_COUNT; i++) {
        if (i) j += ",";
        restoreFixText(p, i, p.fx[i].tplVal,  a, sizeof(a));
        restoreFixText(p, i, p.fx[i].packVal, b, sizeof(b));
        restoreFixText(p, i, p.fx[i].useVal,  c, sizeof(c));
        j += "{\"key\":\"";    j += RESTORE_FIX_DOC[i].key;
        j += "\",\"title\":\""; j += RESTORE_FIX_DOC[i].title;
        j += "\",\"detail\":\"";j += RESTORE_FIX_DOC[i].detail;
        j += "\",\"chip\":";    j += (int)RESTORE_FIX_DOC[i].chip;
        j += ",\"chipsText\":\""; j += opChipsText(RESTORE_FIX_DOC[i].chip);
        j += "\",\"avail\":";   j += p.fx[i].avail ? "true" : "false";
        j += ",\"on\":";        j += p.fx[i].on ? "true" : "false";
        j += ",\"tpl\":\"";     j += a;
        j += "\",\"pack\":\"";  j += b;
        j += "\",\"use\":\"";   j += c;
        j += "\"}";
    }
    j += "]}";
    return j;
}

// ── Накласти на план те, що прийшло ззовні ─────────────────────────────────
// Один код на всі три входи (веб, USB-команди, Майстер): інакше кожен із них
// по-своєму трактує «маска не має стирати введене вручну», і галочка в клієнті
// починає означати різне залежно від того, звідки її натиснули.
//   fixes   — список ключів через кому (nullptr/"" — не чіпати маску)
//   rated   — ємність нових банок, <0 — не чіпати
//   rsRaw   — шунт числом (мОм×100), <0 — не чіпати
//   rsModel — шунт із бібліотеки еталонів за назвою моделі; головніший за rsRaw
static void restorePlanOverride(RestorePlan &p, const char *fixes, long rated,
                                long rsRaw, const char *rsModel, long mfg = -1,
                                int health = -1, long useDate = -1, int cal = -1,
                                int cyc = -1, int nonImp = -1, long today = -1,
                                int etmSrc = -1) {
    if (rated >= 0) restorePlanSetRated(p, rated);
    // Сьогоднішню дату ставимо ПЕРШОЮ: від неї залежить наробіток, а той
    // перераховується при кожній наступній правці.
    if (today > 0) {
        // Годинника реального часу в ESP32 немає, а NTP недосяжний: пристрій
        // сам є точкою доступу. Тому дата, яку приніс клієнт, стає СИСТЕМНОЮ —
        // далі нею користуються й ті шляхи, де клієнта немає.
        deviceClockSetNum(today);
        restorePlanSetToday(p, (int)(today / 10000),
                            (int)((today / 100) % 100), (int)(today % 100));
    }
    // Дата виготовлення — одним числом YYYYMMDD (0 прибирає ручне значення).
    if (mfg >= 0) restorePlanSetMfg(p, (int)(mfg / 10000), (int)((mfg / 100) % 100),
                                    (int)(mfg % 100));
    if (health >= 0) restorePlanSetHealth(p, health);
    if (useDate >= 0) restorePlanSetUse(p, (int)(useDate / 10000),
                                        (int)((useDate / 100) % 100), (int)(useDate % 100));
    if (cal >= -1 && cal != -1) restorePlanSetCal(p, cal);
    if (cyc != -1 || nonImp != -1) restorePlanSetCycles(p, cyc, nonImp);
    if (rsModel && *rsModel)
        restorePlanSetRsense(p, templateRsenseRawByName(rsModel), rsModel);
    else if (rsRaw >= 0) restorePlanSetRsense(p, rsRaw);
    if (fixes && *fixes) {
        uint32_t m = restoreMaskFromKeys(fixes, p);
        int  user   = p.ratedUser;              // маска не має стирати введене
        long rsUser = p.rsUser;
        char rsSrc[16]; snprintf(rsSrc, sizeof(rsSrc), "%s", p.rsSrc);
        int  my = p.mfgUserY, mm = p.mfgUserM, md = p.mfgUserD;
        int  hp = p.healthUser;
        int  uy = p.useUserY, um = p.useUserM, ud = p.useUserD;
        int  ca = p.calUser, cy = p.cycUser, ni = p.nonUser;
        restorePlanSetMask(p, m);
        if (user > 0 && (m & (1UL << RPF_RATED))) restorePlanSetRated(p, user);
        if (rsUser > 0 && (m & (1UL << RPF_RSENSE))) restorePlanSetRsense(p, rsUser, rsSrc);
        if (my > 0 && (m & (1UL << RPF_CRYPT))) restorePlanSetMfg(p, my, mm, md);
        if (hp > 0 && (m & (1UL << RPF_CRYPT))) restorePlanSetHealth(p, hp);
        if (uy > 0 && (m & (1UL << RPF_CRYPT))) restorePlanSetUse(p, uy, um, ud);
        if (ca >= 0 && (m & (1UL << RPF_CRYPT))) restorePlanSetCal(p, ca);
        if ((cy >= 0 || ni >= 0) && (m & (1UL << RPF_HIST))) restorePlanSetCycles(p, cy, ni);
        // Вписана дата запуску сама вмикає правку наробітку — але останнє слово
        // за клієнтом: галочка в його таблиці має означати те, що показує.
        p.fx[RPF_ETM].on = p.fx[RPF_ETM].avail && (m & (1UL << RPF_ETM));
        restorePlanRecalc(p);          // «буде записано» рахується з on
    }
    // Джерело наробітку — останнім: воно не має скидатись ані маскою, ані
    // датами, які ми щойно повернули на місце.
    if (etmSrc >= 0) restorePlanSetEtmSource(p, etmSrc != 0);
}

// GET /api/restore/plan?model=XXX[&fixes=a,b][&read=0]
// Що саме буде виправлено в еталоні перед записом у ЦЕЙ пакет.
// rated — ємність нових банок, вписана вручну. Ставимо ДО маски: увімкнення
// правки ємності міняє й паливомір, і робити це двома незалежними кроками
// означало б показати проміжне (неправильне) число.
static void applyPlanArgs(RestorePlan &p) {
    String rsm = server.arg("rsmodel"); rsm.trim(); rsm.toUpperCase();
    restorePlanOverride(p,
        server.hasArg("fixes")  ? server.arg("fixes").c_str() : nullptr,
        server.hasArg("rated")  ? server.arg("rated").toInt() : -1,
        server.hasArg("rsense") ? server.arg("rsense").toInt() : -1,
        rsm.c_str(),
        server.hasArg("mfg") ? server.arg("mfg").toInt() : -1,
        server.hasArg("health") ? server.arg("health").toInt() : -1,
        server.hasArg("use")    ? server.arg("use").toInt()    : -1,
        server.hasArg("cal")    ? server.arg("cal").toInt()    : -1,
        server.hasArg("cyc")    ? server.arg("cyc").toInt()    : -1,
        server.hasArg("nonimp") ? server.arg("nonimp").toInt() : -1,
        server.hasArg("today")  ? server.arg("today").toInt()  : -1,
        server.hasArg("etmsrc") ? server.arg("etmsrc").toInt() : -1);
}

void handleRestorePlan() {
    String model = server.arg("model"); model.trim(); model.toUpperCase();
    RestorePlan p;
    bool refresh = !server.hasArg("read") || server.arg("read") != "0";
    if (!buildRestorePlanFor(model.c_str(), p, refresh)) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Немає вшитого шаблону для цієї моделі\"}");
        return;
    }
    applyPlanArgs(p);
    server.send(200, "application/json",
                String("{\"status\":\"success\",\"plan\":") + restorePlanJson(p) + "}");
}

// ── ЗАСТОСУВАТИ ЛИШЕ ПРАВКИ, без перезапису еталона ────────────────────────
//  Коли ідентичність пакета ціла, а збилися тільки паливомір / шунт / OFFSET /
//  наробіток / паспортна ємність, переписувати весь еталон немає за що: це
//  зайвий ризик і втрата навченої калібровки. Тут ми правимо РІВНО обрані поля
//  в тому, що вже лежить у чипах, і нічого більше не чіпаємо — зокрема НЕ
//  скидаємо лічильники CCA/DCA (їх обнуляє лише повне відновлення).
bool performApplyFixes(const RestorePlan &p, bool *ok33 = nullptr, bool *ok38 = nullptr) {
    if (ok33) *ok33 = false;
    if (ok38) *ok38 = false;
    if (!hasDump2438 && !hasDump) return false;

    bool need38 = hasDump2438 && (p.fx[RPF_CHARGE].on || p.fx[RPF_RSENSE].on ||
                                  p.fx[RPF_ADCOFF].on || p.fx[RPF_ETM].on);
    bool need33 = hasDump && (p.fx[RPF_RATED].on || p.fx[RPF_CRYPT].on || p.fx[RPF_HIST].on);
    if (!need33 && !need38) return false;          // нічого не обрано

    restorePlanApply(p, need33 ? batteryDump : nullptr,
                        need38 ? batteryDump2438 : nullptr, /*onlyEnabled=*/true);

    ledSet(LED_WRITE); displayShow("ЗАПИС ПРАВОК");
    bool w33 = true, w38 = true;
    if (need33) {
        w33 = battery.writeBattery(batteryDump, DUMP_SIZE);
        if (w33) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    }
    if (need38) {
        w38 = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
        if (w38) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    }
    bool ok = w33 && w38;
    if (ok33) *ok33 = w33;
    if (ok38) *ok38 = w38;
    displayShow(ok ? "ПРАВКИ OK" : "ПРАВКИ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.printf("Fixes: DS2433=%s DS2438=%s\n",
                  need33 ? (w33 ? "OK" : "FAIL") : "-", need38 ? (w38 ? "OK" : "FAIL") : "-");
    return ok;
}

// POST /api/restore/fixes?model=…&fixes=…[&rated=…]
void handleApplyFixes() {
    if (!requireAdmin()) return;
    String model = server.arg("model"); model.trim(); model.toUpperCase();
    RestorePlan p;
    if (!buildRestorePlanFor(model.c_str(), p, true)) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Немає вшитого шаблону для цієї моделі\"}");
        return;
    }
    applyPlanArgs(p);
    bool o33 = false, o38 = false;
    bool ok = performApplyFixes(p, &o33, &o38);
    String j = String("{\"status\":\"") + (ok ? "success" : "error") +
               "\",\"ds2433\":" + (o33 ? "true" : "false") +
               ",\"ds2438\":" + (o38 ? "true" : "false") +
               ",\"plan\":" + restorePlanJson(p) + ",\"message\":\"";
    if (ok) j += "Правки записано (еталон не чіпали)";
    else    j += "Не обрано жодної правки або збій запису";
    j += "\"}";
    server.send(ok ? 200 : 500, "application/json", j);
}

// Веб-відновлення еталона (під паролем): model[, fixes][, verbatim].
void handleRestore() {
    if (!requireAdmin()) return;
    String model = server.arg("model"); model.trim(); model.toUpperCase();
    if (findTemplate(model.c_str()) < 0) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Немає вшитого шаблону для цієї моделі\"}"); return;
    }
    bool ok33 = false, ok38 = false;
    // verbatim=1 — ручний режим: записати шаблон байт-у-байт разом із навченим
    // хвостом донора. Для аналізу; для ремонту лишайте вимкненим.
    bool verbatim = server.hasArg("verbatim") && server.arg("verbatim") == "1";

    // Правки під цей пакет. Клієнт надсилає рівно той набір, який показав
    // користувачеві; без параметра діє типовий набір. Перечитуємо чипи, щоб
    // заряд узявся з напруги ЗАРАЗ, а не з давнього читання.
    RestorePlan p;
    const RestorePlan *pp = nullptr;
    if (!verbatim && buildRestorePlanFor(model.c_str(), p, true)) {
        applyPlanArgs(p);
        pp = &p;
    }

    // tail=erase — лишити навчений хвіст стертим (див. RTAIL_* вище).
    int tailMode = tailModeFromArg(server.arg("tail"));
    bool ok = performRestoreTemplate(model.c_str(), &ok33, &ok38, verbatim, pp, tailMode);
    String j = String("{\"status\":\"") + (ok ? "success" : "error") + "\",\"ds2433\":" +
               (ok33 ? "true" : "false") + ",\"ds2438\":" + (ok38 ? "true" : "false");
    // Звітуємо ФАКТИЧНО застосовані правки: користувач має бачити, що саме
    // потрапило в чип, а не лише те, що він попросив.
    if (pp) j += ",\"plan\":" + restorePlanJson(*pp);
    j += ",\"message\":\"";
    if (ok && ok38)      j += String("Відновлено еталон ") + model + " (обидві мікросхеми)";
    else if (ok)         j += "DS2433 відновлено; DS2438 не записано (відсутній/битий)";
    else                 j += "Збій запису (див. Serial-лог)";
    j += "\"}";
    server.send(ok ? 200 : 500, "application/json", j);
}

// Список доступних вшитих моделей-шаблонів (для випадаючого списку у вебі/USB).
// Каталог операцій (operations.h) у JSON — щоб веб і десктопний клієнт малювали
// ТОЙ САМИЙ список у тому самому порядку, що й екранне меню, і не тримали
// власних (розбіжних) копій назв/описів.
// Розряд: старт/зупинка/стан. Небезпечна операція, тому старт вимагає явного
// підтвердження з клієнта, а будь-яка відмова повертається текстом причини.
void handleDischargeStart() {
    if (!requireAdmin()) return;
    uint16_t target = server.hasArg("target") ? (uint16_t)server.arg("target").toInt() : 0;
    const char *err = dischargeStart(target);
    if (err) {
        String j = "{\"status\":\"error\",\"message\":\""; j += err; j += "\"}";
        server.send(400, "application/json", j);
        return;
    }
    server.send(200, "application/json",
        String("{\"status\":\"success\",\"message\":\"Розряд почато\",\"discharge\":") + dischargeJson() + "}");
}
void handleDischargeStop() {
    if (!requireAdmin()) return;
    dischargeStop(DISR_USER);
    server.send(200, "application/json",
        String("{\"status\":\"success\",\"message\":\"Розряд зупинено\",\"discharge\":") + dischargeJson() + "}");
}
void handleDischargeStatus() {
    server.send(200, "application/json", dischargeJson());
}

// Заряд: старт/зупинка/стан. Так само, як розряд — небезпечна операція,
// тому старт вимагає явного підтвердження з клієнта.
void handleChargeStart() {
    if (!requireAdmin()) return;
    uint8_t target = server.hasArg("target") ? (uint8_t)server.arg("target").toInt() : 0;
    const char *err = chargeStart(target);
    if (err) {
        String j = "{\"status\":\"error\",\"message\":\""; j += err; j += "\"}";
        server.send(400, "application/json", j);
        return;
    }
    server.send(200, "application/json",
        String("{\"status\":\"success\",\"message\":\"Заряд почато\",\"charge\":") + chargeJson() + "}");
}
void handleChargeStop() {
    if (!requireAdmin()) return;
    chargeStop(CHGR_USER);
    server.send(200, "application/json",
        String("{\"status\":\"success\",\"message\":\"Заряд зупинено\",\"charge\":") + chargeJson() + "}");
}
void handleChargeStatus() {
    server.send(200, "application/json", chargeJson());
}

// ===========================================================================
//  НАЛАШТУВАННЯ ЗВУКУ
//
//  Заводські значення в buzzer.h підібрані «в середньому», але реальний п'єзо
//  має власний резонанс і власну гучність: те, що на одному екземплярі звучить
//  м'яко, на іншому ледь чутно або, навпаки, різко. Тому все, що формує
//  характер сигналу, править користувач і воно переживає перезавантаження.
//
//  Файл — один рядок «ключ=значення»: його можна прочитати очима й полагодити
//  руками, на відміну від дампа структури.
// ===========================================================================
#define SOUND_CFG_PATH "/sound.cfg"

static bool soundCfgSave() {
    const BuzzCfg &c = buzzGetCfg();
    File f = SPIFFS.open(SOUND_CFG_PATH, "w");
    if (!f) { Serial.println("SOUND: cannot write " SOUND_CFG_PATH); return false; }
    f.printf("v1 en=%d clk=%d vol=%u tempo=%u glide=%u atk=%u rel=%u st=%d\n",
             c.enabled ? 1 : 0, c.clickOn ? 1 : 0, (unsigned)c.volume,
             (unsigned)c.tempoPct, (unsigned)c.glidePct,
             (unsigned)c.attackMs, (unsigned)c.releaseMs, (int)c.semitones);
    f.close();
    return true;
}

// Прочитати збережені налаштування. Відсутній або побитий файл — не помилка:
// лишаються заводські значення, і пристрій просто звучить «як з коробки».
static void soundCfgLoad() {
    if (!SPIFFS.exists(SOUND_CFG_PATH)) { Serial.println("SOUND: defaults (no file)"); return; }
    File f = SPIFFS.open(SOUND_CFG_PATH, "r");
    if (!f) return;
    String line = f.readStringUntil('\n');
    f.close();
    int en = 1, clk = 1, vol = BUZZER_VOLUME, tempo = 100, glide = 100;
    int atk = BUZZ_ATTACK_MS, rel = BUZZ_RELEASE_MS, st = 0;
    int n = sscanf(line.c_str(),
                   "v1 en=%d clk=%d vol=%d tempo=%d glide=%d atk=%d rel=%d st=%d",
                   &en, &clk, &vol, &tempo, &glide, &atk, &rel, &st);
    if (n != 8) { Serial.printf("SOUND: bad cfg (%d fields), using defaults\n", n); return; }
    BuzzCfg c;
    c.enabled   = en != 0;
    c.clickOn   = clk != 0;
    c.volume    = (uint8_t)constrain(vol, 0, 255);
    c.tempoPct  = (uint16_t)constrain(tempo, 0, 1000);
    c.glidePct  = (uint16_t)constrain(glide, 0, 1000);
    c.attackMs  = (uint16_t)constrain(atk, 0, 1000);
    c.releaseMs = (uint16_t)constrain(rel, 0, 1000);
    c.semitones = (int8_t)constrain(st, -12, 12);
    buzzSetCfg(c);                       // затисне решту меж сам
    const BuzzCfg &g = buzzGetCfg();
    Serial.printf("SOUND: loaded en=%d vol=%u tempo=%u%% glide=%u%% st=%d\n",
                  g.enabled ? 1 : 0, (unsigned)g.volume, (unsigned)g.tempoPct,
                  (unsigned)g.glidePct, (int)g.semitones);
}

static String soundJson() {
    const BuzzCfg &c = buzzGetCfg();
    String j = "{\"enabled\":"; j += c.enabled ? "true" : "false";
    j += ",\"click\":";        j += c.clickOn ? "true" : "false";
    j += ",\"volume\":";       j += (int)c.volume;
    j += ",\"tempo\":";        j += (int)c.tempoPct;
    j += ",\"glide\":";        j += (int)c.glidePct;
    j += ",\"attack\":";       j += (int)c.attackMs;
    j += ",\"release\":";      j += (int)c.releaseMs;
    j += ",\"semitones\":";    j += (int)c.semitones;
    j += "}";
    return j;
}

// Повна відповідь: значення + межі повзунків + перелік сигналів. Межі віддає
// пристрій, щоб клієнт не тримав власну копію діапазонів і не розійшовся з
// buzzCfgClamp() після наступної правки.
static String soundFullJson() {
    String j = "{\"status\":\"success\",\"sound\":" + soundJson();
    j += ",\"limits\":{\"volume\":[0,255],\"tempo\":[25,400],\"glide\":[0,300],"
         "\"attack\":[0,200],\"release\":[0,400],\"semitones\":[-12,12]}";
    j += ",\"defaults\":{\"enabled\":true,\"click\":true,\"volume\":" + String((int)BUZZER_VOLUME) +
         ",\"tempo\":100,\"glide\":100,\"attack\":" + String((int)BUZZ_ATTACK_MS) +
         ",\"release\":" + String((int)BUZZ_RELEASE_MS) + ",\"semitones\":0}";
#ifdef BUZZER_PIN
    j += ",\"hasBuzzer\":true,\"pin\":" + String((int)BUZZER_PIN);
#else
    j += ",\"hasBuzzer\":false,\"pin\":-1";
#endif
    j += ",\"signals\":[";
    for (int i = 0; i < BZ_SIGNAL_COUNT; i++) {
        if (i) j += ",";
        j += "{\"key\":\""; j += BZ_SIGNALS[i].key;
        j += "\",\"title\":\""; j += BZ_SIGNALS[i].title;
        j += "\",\"ms\":"; j += (int)buzzPhraseMs(BZ_SIGNALS[i].seq, BZ_SIGNALS[i].len);
        j += "}";
    }
    j += "]}";
    return j;
}

// ── ЗНОС / ЗДОРОВ'Я ОКРЕМОЮ ДІЄЮ ───────────────────────────────────────────
//  POST /api/sethealth?pct=80
//  Те саме, що правка «знос» у «Ремонті», але одним рухом: після заміни банок
//  міняти доводиться саме це число, і заради нього щоразу відкривати план
//  зайве. Рахунок і запис — тим самим кодом (restore_plan.h), щоб два входи не
//  розійшлися: знос залежить і від шунта, і від паспортної ємності, і власна
//  копія формули одного дня дала б інший CTS.
void handleSetHealth() {
    if (!requireAdmin()) return;
    if (!hasDump) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Спочатку зчитайте АКБ\"}");
        return;
    }
    // Знос лежить у зашифрованому блоці RECOND, а ключ береться з ROM-ID чипа
    // DS2433. Для дампа з файлу ROM узяти нізвідки — писати наосліп не будемо.
    if (!hasSN2433) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"ROM чипа DS2433 невідомий — знос шифрується ключем із нього. Перечитайте АКБ на пристрої.\"}");
        return;
    }
    int pct = server.hasArg("pct") ? server.arg("pct").toInt() : 0;
    if (pct < 1 || pct > 100) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Знос має бути 1..100 %\"}");
        return;
    }
    char model[16] = "";
    decodeModel(model, sizeof(model));
    RestorePlan p;
    if (!model[0] || !buildRestorePlanFor(model, p, /*refresh=*/false)) {
        String m = "{\"status\":\"error\",\"message\":\"Немає вшитого еталона для моделі '";
        m += model[0] ? model : "?";
        m += "' — без нього не порахувати знос. Скористайтесь планом у «Ремонті».\"}";
        server.send(400, "application/json", m);
        return;
    }
    restorePlanSetHealth(p, pct);
    if (!p.fx[RPF_CRYPT].on) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Блок RECOND не читається — знос писати нікуди\"}");
        return;
    }
    // d38 = nullptr: знос живе тільки в DS2433, монітор чіпати немає за що.
    restorePlanApply(p, batteryDump, nullptr, /*onlyEnabled=*/true);

    ledSet(LED_WRITE); displayShow("ЗАПИС ЗНОСУ");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    displayShow(ok ? "ЗНОС OK" : "ЗНОС ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    if (!ok) {
        server.send(500, "application/json",
                    "{\"status\":\"error\",\"message\":\"Помилка запису DS2433\"}");
        return;
    }
    String j = "{\"status\":\"success\",\"health\":"; j += pct;
    j += ",\"cts\":";    j += (int)p.ctsUse;
    j += ",\"ratedMah\":"; j += p.ratedMah;
    j += ",\"mah\":";    j += (long)(p.ratedMah * (long)pct / 100);
    j += "}";
    server.send(200, "application/json", j);
}

// ── СИСТЕМНА ДАТА ПРИСТРОЮ ──────────────────────────────────────────────────
//  GET  /api/clock              — яку дату пристрій вважає сьогоднішньою
//  POST /api/clock?today=…      — завести годинник (те саме роблять і всі
//                                 запити плану, які несуть today=)
//  Пароля не питаємо: дата нічого не псує, а без неї наробіток не рахується.
static String deviceClockJson() {
    String j = "{\"status\":\"success\",\"today\":";
    j += deviceClockNum();
    j += ",\"src\":\"";
    j += deviceClockSrcName();
    j += "\"}";
    return j;
}
void handleClockGet() { server.send(200, "application/json", deviceClockJson()); }
void handleClockSet() {
    long t = server.hasArg("today") ? server.arg("today").toInt() : 0;
    if (!deviceClockSetNum(t)) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Потрібна дата РРРРММДД\"}");
        return;
    }
    server.send(200, "application/json", deviceClockJson());
}

void handleSoundGet() { server.send(200, "application/json", soundFullJson()); }

// POST /api/sound — приймає будь-яку підмножину полів: чого немає, те не
// чіпаємо. Так повзунок гучності не скидає темп, і навпаки.
//   test=<ключ>  — одразу прослухати сигнал уже з НОВИМИ значеннями;
//   reset=1      — повернути заводські;
//   save=0       — приміряти без запису в SPIFFS (для «живого» повзунка).
void handleSoundSet() {
    if (!requireAdmin()) return;
    BuzzCfg c = buzzGetCfg();
    if (server.hasArg("reset") && server.arg("reset") != "0") {
        BuzzCfg d = { true, true, BUZZER_VOLUME, 100, 100,
                      BUZZ_ATTACK_MS, BUZZ_RELEASE_MS, 0 };
        c = d;
    }
    auto flag = [&](const char *k, bool cur) {
        if (!server.hasArg(k)) return cur;
        String v = server.arg(k);
        return !(v == "0" || v == "false" || v == "off");
    };
    c.enabled = flag("enabled", c.enabled);
    c.clickOn = flag("click",   c.clickOn);
    if (server.hasArg("volume"))    c.volume    = (uint8_t)constrain(server.arg("volume").toInt(), 0, 255);
    if (server.hasArg("tempo"))     c.tempoPct  = (uint16_t)constrain(server.arg("tempo").toInt(), 0, 1000);
    if (server.hasArg("glide"))     c.glidePct  = (uint16_t)constrain(server.arg("glide").toInt(), 0, 1000);
    if (server.hasArg("attack"))    c.attackMs  = (uint16_t)constrain(server.arg("attack").toInt(), 0, 1000);
    if (server.hasArg("release"))   c.releaseMs = (uint16_t)constrain(server.arg("release").toInt(), 0, 1000);
    if (server.hasArg("semitones")) c.semitones = (int8_t)constrain(server.arg("semitones").toInt(), -12, 12);
    buzzSetCfg(c);                       // затиск меж — усередині

    bool saved = true;
    if (!server.hasArg("save") || server.arg("save") != "0") saved = soundCfgSave();

    uint32_t testMs = 0;
    if (server.hasArg("test")) testMs = buzzPlayNamed(server.arg("test").c_str());

    String j = soundFullJson();
    j.remove(j.length() - 1);            // прибрати '}' і дописати службові поля
    j += ",\"saved\":"; j += saved ? "true" : "false";
    j += ",\"testMs\":"; j += (int)testMs; j += "}";
    server.send(200, "application/json", j);
}

// Окремий маршрут «просто прослухати»: нічого не міняє й не пише в SPIFFS.
// Невідомий ключ і вимкнений звук — різні речі: у першому випадку клієнт помилився,
// у другому все правильно, просто чути нема чого, і мовчання треба пояснити.
void handleSoundTest() {
    if (!requireAdmin()) return;
    String name = server.hasArg("name") ? server.arg("name") : String("ok");
    if (!buzzFindSignal(name.c_str())) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Невідомий сигнал\"}");
        return;
    }
    uint32_t ms = buzzPlayNamed(name.c_str());
    String j = String("{\"status\":\"success\",\"name\":\"") + name +
               "\",\"ms\":" + (int)ms + ",\"played\":" + (ms ? "true" : "false");
    if (!ms) j += ",\"message\":\"Звук вимкнено в налаштуваннях\"";
    server.send(200, "application/json", j + "}");
}

void handleOps() {
    String j = "{\"status\":\"success\",\"ops\":[";
    bool first = true;
    // chips — у яку мікросхему піде запис. Поверхні показують це поруч із
    // назвою операції: переплутати DS2433 (ідентичність) із DS2438 (монітор)
    // коштує або моделі, або заводського калібрування вимірювача струму.
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
            "Ідентичність, розрядна крива, COPYRIGHT, заводська таблиця й запис моделі. Навчений калібрувальний хвіст НЕ переноситься — інакше пакет отримав би чужу калібровку.",
            OPD_WRITE, BATTERY_TEMPLATES[t].name,
            BATTERY_TEMPLATES[t].d38 ? OPC_BOTH : OPC_33);
    for (int t = 0; t < BATTERY_TEMPLATE_COUNT; t++)
        add("new", "Новий АКБ з порожнього чипа",
            "Записує модельну частину еталона й приводить монітор у стан нового пакета. Навчена калібровка лишається порожньою — її запише зарядна станція під час калібрування.",
            OPD_WIPE, BATTERY_TEMPLATES[t].name, OPC_BOTH);
    for (int e = 0; e < OP_EXPERT_COUNT; e++)
        add(OP_DOC_EXPERT[e].key, OP_DOC_EXPERT[e].title, OP_DOC_EXPERT[e].detail,
            OP_TEXT_EXPERT[e].danger, nullptr, OP_DOC_EXPERT[e].chips);
    j += "]}";
    server.send(200, "application/json", j);
}

void handleTemplates() {
    String j = "{\"status\":\"success\",\"models\":[";
    for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++) {
        if (i) j += ",";
        j += "\""; j += BATTERY_TEMPLATES[i].name; j += "\"";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

// ------------------- Майстер відновлення (Recovery Wizard) -------------------
// Двигун і база правил — у recovery.h. Підключаємо ТУТ, коли всі perform*-функції
// та аналітичні хелпери вже визначені. Клієнти отримують єдиний JSON-стан.
#include "recovery.h"

// GET /api/wizard — зчитати чіпи, повернути аналіз + проблеми + план + прогрес.
void handleWizard() {
    server.send(200, "application/json", wizStart());
}
// POST /api/wizard/step — виконати крок плану (під паролем). Аргументи: idx[, model].
void handleWizardStep() {
    if (!requireAdmin()) return;
    int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
    String model = server.arg("model");
    // Майстер бере ТІ САМІ правки під пакет, що й «Відновити еталон»: інакше
    // галочки в картці правок для нього нічого не значили б.
    String wfx = server.hasArg("fixes") ? server.arg("fixes") : String();
    long wrated = server.hasArg("rated") ? server.arg("rated").toInt() : -1;
    long wrs    = server.hasArg("rsense") ? server.arg("rsense").toInt() : -1;
    long wmfg   = server.hasArg("mfg") ? server.arg("mfg").toInt() : -1;
    String wrsm = server.arg("rsmodel"); wrsm.trim(); wrsm.toUpperCase();
    server.send(200, "application/json",
                wizExecStep(idx, model, wfx, wrated, wrs, wrsm, wmfg,
                            tailModeFromArg(server.arg("tail")),
                            server.hasArg("health") ? server.arg("health").toInt() : -1,
                            server.hasArg("use")    ? server.arg("use").toInt()    : -1,
                            server.hasArg("cal")    ? server.arg("cal").toInt()    : -1,
                            server.hasArg("cyc")    ? server.arg("cyc").toInt()    : -1,
                            server.hasArg("nonimp") ? server.arg("nonimp").toInt() : -1,
                            server.hasArg("today")  ? server.arg("today").toInt()  : -1,
                            server.hasArg("etmsrc") ? server.arg("etmsrc").toInt() : -1));
}
// POST /api/wizard/reset — скинути журнал продовження ПОТОЧНОГО АКБ (під паролем).
void handleWizardReset() {
    if (!requireAdmin()) return;
    wizJournalClear();
    server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/wizard/journals — усі збережені журнали (серійник + заплановані дії).
void handleWizardJournals() {
    server.send(200, "application/json", wizJournalListJson());
}
// POST /api/wizard/journals/delete — видалити журнал за серійником (під паролем).
void handleWizardJournalDelete() {
    if (!requireAdmin()) return;
    String serial = server.arg("serial"); serial.trim(); serial.toUpperCase();
    if (!serial.length()) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"no serial\"}"); return; }
    wizJournalDelete(serial.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
}

// Веб-ініціалізація нового АКБ (під паролем): model + mah.
void handleInitBattery() {
    if (!requireAdmin()) return;
    String model = server.arg("model"); model.trim(); model.toUpperCase();
    if (findTemplate(model.c_str()) < 0) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Немає вшитого шаблону для цієї моделі\"}"); return;
    }
    if (!server.hasArg("mah")) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Вкажіть ємність у мА·год\"}"); return;
    }
    long mah = server.arg("mah").toInt();
    if (mah <= 0) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Ємність має бути > 0 мА·год\"}"); return;
    }
    bool ok = performInitBattery(model.c_str(), mah);
    if (!ok) {
        server.send(500, "application/json",
                    "{\"status\":\"error\",\"message\":\"Збій запису (див. Serial-лог)\"}");
        return;
    }
    // Кажемо прямо, чи згенеровано ідентичність: без ROM у чипі лишається
    // шифровка донора, і рація прочитає її як сміття — про це треба знати.
    String m = "Новий АКБ "; m += model; m += " записано. ";
    if (g_initIdentityGen) {
        m += "Ідентичність згенеровано з ROM-ID цього чипа";
        if (g_initIdentityDate) {
            char d[16];
            snprintf(d, sizeof(d), "%04d-%02d-%02d", g_initIdentityDate / 10000,
                     (g_initIdentityDate / 100) % 100, g_initIdentityDate % 100);
            m += ", дата виготовлення "; m += d;
        } else {
            m += " (дату виготовлення не записано: годинник пристрою не заведено)";
        }
        m += ".";
    } else {
        m += "⚠️ ROM-ID чипа невідомий — ідентичність НЕ згенеровано, у чипі лишились "
             "зашифровані поля донора. Перечитайте АКБ і повторіть.";
    }
    String j = "{\"status\":\"success\",\"identity\":";
    j += g_initIdentityGen ? "true" : "false";
    j += ",\"mfgDate\":"; j += g_initIdentityDate;
    j += ",\"message\":\""; j += m; j += "\"}";
    server.send(200, "application/json", j);
}

// ── ВІДНОВЛЕННЯ ЗА ЗРАЗКОМ КИТАЙСЬКОЇ КОПІЇ — КРАЙНІЙ ЗАСІБ ────────────────
//  Коли жодна спроба відновлення не вдалася. Копії влаштовані так, що вся
//  потрібна рації інформація живе в DS2438 (дзеркало заголовка несе паспортну
//  ємність, поруч — шунт і OFFSET), а DS2433 у них порожній і не пишеться.
//  Повторюємо це: пишемо монітор зі зразка, а DS2433 стираємо.
//
//  ⚑ Запис ідентичності в DS2433 (модель, дати, знос) — ОКРЕМИЙ і
//  ЕКСПЕРИМЕНТАЛЬНИЙ крок за бажанням власника: у справжньої копії цього немає
//  взагалі. Каркас беремо з еталона родини 4409 — саме її шунт (45.65 мОм)
//  несуть монітори копій. Шифроване пишеться ключем із ROM ЦЬОГО чипа.
bool performCloneRestore(const uint8_t *src38, int ratedMah, long rsRaw,
                         bool write33, const char *model,
                         int mfgY, int mfgM, int mfgD,
                         int useY, int useM, int useD, int healthPct,
                         String *note, bool zeroCounters = true,
                         bool recheckCharge = true) {
    if (!src38) return false;

    // Паливомір — із РЕАЛЬНОЇ напруги цього пакета, а не з чисел копії.
    int pct = chargePctFromVoltage();
    int rated = ratedMah > 0 ? ratedMah : impresCloneRatedFrom38(src38);
    if (rated <= 0) rated = 2150;                       // родина 4409 за умовчанням
    long rsUse = rsRaw > 0 ? rsRaw : (long)(src38[56] | (src38[57] << 8));
    uint8_t ica = (pct >= 0)
        ? impresIcaFromPercentRs(pct, rated, rsUse > 0 ? rsUse / 100000.0f : DS2438_RSENSE_OHM)
        : src38[12];

    impresCloneBuild38(batteryDump2438, src38, ratedMah, rsRaw, ica, zeroCounters);
    hasDump2438 = true;

    // DS2433 — у 0xFF, як у копії.
    memset(batteryDump, 0xFF, DUMP_SIZE);
    hasDump = true;
    String n = "Монітор записано за зразком копії; DS2433 стерто.";

    if (write33) {
        // ЕКСПЕРИМЕНТ: каркас 4409 + ручна ідентичність під ROM цього чипа.
        int t = findTemplate(model && *model ? model : "PMNN4409A");
        if (t < 0) t = findTemplate("PMNN4409A");
        if (t < 0) {
            n += " Каркаса 4409 немає — ідентичність не записано.";
        } else if (!hasSN2433) {
            n += " ROM чипа невідомий — ідентичність НЕ записано (шифрувати нічим).";
        } else {
            memcpy_P(batteryDump, BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
            int cleared = applyFreshTail(BATTERY_TEMPLATES[t].fresh);
            if (cleared < 0) impresEraseTail(batteryDump);
            if (ratedMah > 0)
                batteryDump[IMPRES_RATED_BYTE] = (uint8_t)(ratedMah / IMPRES_RATED_STEP);
            if (model && *model) applyModel(model);
            uint8_t cts = restoreCtsFromHealth(healthPct > 0 ? healthPct : 100, rated,
                                               rsUse > 0 ? rsUse / 100000.0f : DS2438_RSENSE_OHM);
            // Ключ — ТІЛЬКИ з ROM цього чипа (див. tools/client_audit.py).
            impresIdentityWrite(batteryDump, chipSN2433, mfgY, mfgM, mfgD, cts);
            // Дата першого запуску зберігається як «діб від виготовлення».
            if (useY > 0 && mfgY > 0) {
                ImpresCryptFields f;
                impresCryptRead(batteryDump, chipSN2433[1], chipSN2433[6], &f);
                f.dayInitialUse = f.dayInitialUse2 =
                    restoreDaysBetween(mfgY, mfgM, mfgD, useY, useM, useD);
                impresCryptWrite(batteryDump, chipSN2433[1], chipSN2433[6], &f);
            }
            impresCyclesWrite(batteryDump, 0);
            impresNonImpresWrite(batteryDump, 0);
            impresFixHeader(batteryDump);
            n = "Монітор за зразком копії; у DS2433 записано ЕКСПЕРИМЕНТАЛЬНУ "
                "ідентичність на каркасі 4409 під ROM цього чипа.";
        }
    }

    ledSet(LED_WRITE); displayShow("РЕЖИМ КОПІЇ");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) {
        saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    }
    // ⚑ КОРЕКЦІЯ ЗАРЯДУ ПІСЛЯ ЗАПИСУ. Паливомір ми порахували з напруги, яку
    // виміряли ДО запису — а вимірював її чіп, у якому ще стояли шунт і OFFSET
    // копії. Після запису шунт уже наш, і той самий АЦП дає інше число. Тому
    // перечитуємо монітор і рахуємо заряд ще раз, уже за новими константами;
    // інакше в пакет лишився б відсоток, порахований за чужим шунтом.
    if (ok && recheckCharge) {
        delay(50);                                   // дати вимірюванню осісти
        uint8_t re[DS2438_MEM_SIZE];
        if (battery.readDS2438(re, DS2438_MEM_SIZE)) {
            memcpy(batteryDump2438, re, DS2438_MEM_SIZE);
            int p2 = impresPercentFromMv(impresVoltageMv(batteryDump2438));
            if (p2 >= 0) {
                float rsNow = impresBmsRsense(batteryDump2438);
                uint8_t ica2 = impresIcaFromPercentRs(p2, rated,
                                   rsNow > 0.0f ? rsNow : DS2438_RSENSE_OHM);
                if (ica2 != batteryDump2438[12]) {
                    batteryDump2438[12] = ica2;
                    if (battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE)) {
                        saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
                        Serial.printf("Clone: заряд перераховано після запису -> %d %% (ICA %u)\n",
                                      p2, ica2);
                    }
                }
                n += " Заряд перевірено після запису: ";
                n += p2; n += " %.";
            }
        }
    }
    displayShow(ok ? "КОПІЯ OK" : "КОПІЯ ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    if (note) *note = n;
    Serial.printf("Clone restore: rated=%d rs=%ld ica=%u id33=%d -> %s\n",
                  rated, rsUse, ica, write33 ? 1 : 0, ok ? "OK" : "FAIL");
    return ok;
}

// GET /api/clone/samples — вбудовані зразки моніторів копій.
void handleCloneSamples() {
    String j = "{\"status\":\"success\",\"samples\":[";
    for (int i = 0; i < CLONE_SAMPLE_COUNT; i++) {
        if (i) j += ",";
        j += "{\"name\":\""; j += CLONE_SAMPLES[i].name;
        j += "\",\"note\":\""; j += CLONE_SAMPLES[i].note;
        j += "\",\"rated\":"; j += impresCloneRatedFrom38(CLONE_SAMPLES[i].d38);
        j += ",\"rsense\":"; j += (int)(CLONE_SAMPLES[i].d38[56] | (CLONE_SAMPLES[i].d38[57] << 8));
        j += ",\"hex\":\"";
        char b[3];
        for (int k = 0; k < DS2438_MEM_SIZE; k++) { sprintf(b, "%02X", CLONE_SAMPLES[i].d38[k]); j += b; }
        j += "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

// POST /api/clone — hex38 (64 Б), rated, rsense, id33, model, mfg, use, health
void handleCloneRestore() {
    if (!requireAdmin()) return;
    String hx = server.arg("hex38");
    uint8_t src[DS2438_MEM_SIZE];
    if (hexToBytes(hx, src, DS2438_MEM_SIZE) != DS2438_MEM_SIZE) {
        server.send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"Потрібен дамп DS2438 — рівно 64 байти\"}");
        return;
    }
    long mfg = server.hasArg("mfg") ? server.arg("mfg").toInt() : 0;
    long use = server.hasArg("use") ? server.arg("use").toInt() : 0;
    String md = server.arg("model"); md.trim(); md.toUpperCase();
    String note;
    bool ok = performCloneRestore(src,
        server.hasArg("rated")  ? server.arg("rated").toInt()  : 0,
        server.hasArg("rsense") ? server.arg("rsense").toInt() : 0,
        server.hasArg("id33") && server.arg("id33") == "1",
        md.c_str(),
        (int)(mfg / 10000), (int)((mfg / 100) % 100), (int)(mfg % 100),
        (int)(use / 10000), (int)((use / 100) % 100), (int)(use % 100),
        server.hasArg("health") ? server.arg("health").toInt() : 0, &note,
        !server.hasArg("zero")    || server.arg("zero") != "0",
        !server.hasArg("recheck") || server.arg("recheck") != "0");
    if (!ok) {
        server.send(500, "application/json",
                    "{\"status\":\"error\",\"message\":\"Збій запису (див. Serial-лог)\"}");
        return;
    }
    server.send(200, "application/json",
                String("{\"status\":\"success\",\"message\":\"") + note + "\"}");
}

// Перезавантаження ESP32 (під паролем). Корисно після серії операцій або якщо
// 1-Wire шина «зависла». Відповідь надсилаємо ДО restart, з невеликою затримкою,
// щоб браузер встиг її отримати.
void handleReboot() {
    if (!requireAdmin()) return;
    displayShow("ПЕРЕЗАВАНТАЖЕННЯ");
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Reboot\"}");
    delay(300);
    ESP.restart();
}

// Captive-portal: будь-який невідомий URL (або запит по чужому домену) перенаправляємо
// на головну сторінку. В зв'язці з DNS-сервером (усі домени -> 192.168.4.1) телефон/ПК
// визначає "екран входу в мережа" і сам пропонує відкрити сторінку при підключенні.
//
// ОС-детектори captive-portal (Android /generate_204, Apple /hotspot-detect.html,
// Windows /connecttest.txt|/ncsi.txt) очікують "успіх"; отримавши 302-редирект замість нього,
// система показує сповіщення і відкриває нашу сторінку автоматично.
void handleCaptive() {
    server.sendHeader("Location", String("http://") + ESP_IP + "/", true);
    server.send(302, "text/plain", "");
}

// Налаштування веб-сервера
void setupWebServer() {
    if (!SPIFFS.begin(true)) {
        Serial.println("ERROR: SPIFFS mount failed");
        return;
    }
    
    // Перевіряємо стан SPIFFS при запуску
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    Serial.printf("SPIFFS Status: Total=%d bytes, Used=%d bytes, Free=%d bytes\n", 
                 totalBytes, usedBytes, totalBytes - usedBytes);
    
    server.on("/", handleRoot);
    server.on("/logo.png", HTTP_GET, handleLogo);
    server.on("/api/read", HTTP_GET, handleReadDump);
    server.on("/api/download", HTTP_GET, handleDownloadDump);
    server.on("/api/info", HTTP_GET, handleDumpInfo);
    server.on("/api/write", HTTP_POST, handleWriteDump);
    // Загрузка файлу: 4-аргументна форма — handleUploadDone це обробник
    // запиту (fn, надсилає відповідь), handleUploadDump — upload-колбек (ufn,
    // приймає тело multipart і пише його в SPIFFS).
    server.on("/upload", HTTP_POST, handleUploadDone, handleUploadDump);

    // Мікросхема DS2438 (монітор батареї)
    server.on("/api/download2438", HTTP_GET, handleDownloadDump2438);
    server.on("/api/info2438", HTTP_GET, handleDumpInfo2438);
    server.on("/api/write2438", HTTP_POST, handleWriteDump2438);
    server.on("/upload2438", HTTP_POST, handleUploadDone2438, handleUploadDump2438);
#ifdef DISPLAY_SPLASH_SPIFFS
    server.on("/uploadsplash", HTTP_POST, handleUploadSplashDone, handleUploadSplash);
    server.on("/api/splash", HTTP_GET, handleSplashInfo);        // стан заставки
    server.on("/api/splash/delete", HTTP_POST, handleSplashDelete);
#endif
    server.on("/api/reset", HTTP_POST, handleResetBattery);
    server.on("/api/clean", HTTP_POST, handleClean);            // очистка (крім критичних)
    server.on("/api/wipe2433", HTTP_POST, handleWipe2433);      // ПОВНЕ стирання DS2433
    server.on("/api/wipe2438", HTTP_POST, handleWipe2438);      // ПОВНЕ стирання DS2438
    server.on("/api/repair", HTTP_POST, handleRepair);          // ремонт цілісності
    server.on("/api/setmodel", HTTP_POST, handleSetModel);       // ручний запис моделі
    server.on("/api/setcapacity", HTTP_POST, handleSetCapacity); // змінити ємність %
    server.on("/api/setmah", HTTP_POST, handleSetMah);           // змінити залишок, мА·ч
    server.on("/api/setcharge", HTTP_POST, handleSetCharge);     // рівень заряду з напруги / вручну
    server.on("/api/setetm", HTTP_POST, handleSetEtm);           // змінити ETM (дата першого викор.)
    server.on("/api/writehex", HTTP_POST, handleWriteHex);       // сира запис з редактора
    server.on("/api/reboot", HTTP_POST, handleReboot);           // перезавантаження ESP32
    server.on("/api/templates", HTTP_GET, handleTemplates);      // список вшитих моделей
    server.on("/api/ops", HTTP_GET, handleOps);                  // каталог операцій (operations.h)
    server.on("/api/sethealth", HTTP_POST, handleSetHealth);     // знос/здоров'я одним рухом
    server.on("/api/hdrfix", HTTP_POST, handleHeaderComplete);   // добудова заголовка після станції
    server.on("/api/clock", HTTP_GET, handleClockGet);           // системна дата пристрою
    server.on("/api/clock", HTTP_POST, handleClockSet);          // завести годинник
    server.on("/api/sound", HTTP_GET, handleSoundGet);           // налаштування звуку
    server.on("/api/sound", HTTP_POST, handleSoundSet);          // змінити налаштування звуку
    server.on("/api/sound/test", HTTP_POST, handleSoundTest);    // прослухати сигнал
    server.on("/api/discharge", HTTP_GET, handleDischargeStatus);        // стан розряду
    server.on("/api/discharge/start", HTTP_POST, handleDischargeStart);  // почати розряд
    server.on("/api/discharge/stop", HTTP_POST, handleDischargeStop);    // зупинити розряд
    server.on("/api/charge", HTTP_GET, handleChargeStatus);              // стан заряду
    server.on("/api/charge/start", HTTP_POST, handleChargeStart);        // почати заряд
    server.on("/api/charge/stop", HTTP_POST, handleChargeStop);          // зупинити заряд
    server.on("/api/initbattery", HTTP_POST, handleInitBattery); // ініціалізація нового АКБ
    server.on("/api/clone", HTTP_POST, handleCloneRestore);      // крайній засіб: режим копії
    server.on("/api/clone/samples", HTTP_GET, handleCloneSamples); // вбудовані зразки копій
    server.on("/api/restore", HTTP_POST, handleRestore);         // відновлення еталона verbatim
    server.on("/api/restore/plan", HTTP_GET, handleRestorePlan); // правки еталона під цей пакет
    server.on("/api/restore/fixes", HTTP_POST, handleApplyFixes); // лише правки, без еталона
    server.on("/api/wizard", HTTP_GET, handleWizard);            // Майстер: аналіз+план
    server.on("/api/wizard/step", HTTP_POST, handleWizardStep);  // Майстер: виконати крок
    server.on("/api/wizard/reset", HTTP_POST, handleWizardReset);// Майстер: скинути журнал
    server.on("/api/wizard/journals", HTTP_GET, handleWizardJournals);        // список журналів
    server.on("/api/wizard/journals/delete", HTTP_POST, handleWizardJournalDelete); // видалити журнал
    server.on("/api/recalprep", HTTP_POST, handleRecalPrepare);  // підготовка до рекалібрування

    // Captive-portal: усі інші URL -> редирект на головну (авто-відкриття сторінки).
    server.onNotFound(handleCaptive);

    server.begin();
    Serial.println("Web server started");
}

#endif
