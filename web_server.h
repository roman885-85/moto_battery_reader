#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "battery_reader.h"
#include "settings.h"
#include "impres_format.h"     // структура прошивки IMPRES (єдине джерело правди)
#include "operations.h"        // єдиний каталог операцій для всіх поверхонь
#include "discharge.h"         // керований розряд навантаженням (MOSFET)
#include "leds.h"
#include "display.h"
#include "templates.h"

extern WebServer server;
extern BatteryReader battery;
extern uint8_t batteryDump[DUMP_SIZE];
extern bool hasDump;
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern bool hasDump2438;
extern uint8_t chipSN2438[8];
extern bool hasSN2438;

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

    // Серійний номер чипа (лазерний ROM-ID DS2438)
    if (battery.hasRom2438()) {
        memcpy(chipSN2438, battery.rom2438(), 8);
        hasSN2438 = true;
    }

    char st[40];
    if (ok2433 || ok2438) snprintf(st, sizeof(st), "ЧИТ 2433:%s 2438:%s", ok2433 ? "OK" : "-", ok2438 ? "OK" : "-");
    else                  snprintf(st, sizeof(st), "ПОМИЛКА: нема чіпа");
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

    float    i_mA = (float)current / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f;
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
    int ratedMah = impresRatedMah(rmModel);
    json += ",\"icaMah\":" + String(impresIcaToMah((uint8_t)ica, ratedMah));
    json += ",\"ccaMah\":" + String((int)(cca * DS2438_MAH_PER_LSB));
    json += ",\"dcaMah\":" + String((int)(dca * DS2438_MAH_PER_LSB));
    json += ",\"ccaCycles\":" + String((int)(cca * DS2438_MAH_PER_LSB / ratedMah));
    json += ",\"dcaCycles\":" + String((int)(dca * DS2438_MAH_PER_LSB / ratedMah));
    json += ",\"ratedMah\":" + String(ratedMah);
    json += ",\"charge\":" + String(charge);
    json += ",\"chargeSrc\":\"" + String(csrc) + "\"";
    // ETM (DS2438[8..11], сек наробітку). Рація показує «дату першого користування»
    // як (свій поточний час − ETM) — перевірено діффом до/після калібрування.
    uint32_t etm = ((uint32_t)batteryDump2438[11] << 24) | ((uint32_t)batteryDump2438[10] << 16) |
                   ((uint32_t)batteryDump2438[9] << 8) | batteryDump2438[8];
    json += ",\"etmSec\":" + String(etm);
    json += ",\"serial\":\"" + serial + "\"";
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
void resetBatteryData() {
    if (hasDump2438) {
        for (int i = 8; i <= 11; i++) batteryDump2438[i] = 0; // ETM (таймер)
        batteryDump2438[60] = batteryDump2438[61] = 0;         // CCA
        batteryDump2438[62] = batteryDump2438[63] = 0;         // DCA
    }
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
    resetBatteryData();

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
void factoryCleanData() {
    resetBatteryData();                 // CCA/DCA/ETM у DS2438 + ємність 0x17 -> 100%
    if (hasDump) fixHeaderChecksum(batteryDump);
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
    factoryCleanData();
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
        impresResetMonitor(batteryDump2438, hasDump ? batteryDump : nullptr,
                           impresIcaFromPercent(pct));
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

// Зняти показання монітора під навантаженням. true — читання вдалось.
static bool dischargeSample(uint16_t *mv, int16_t *ma, int16_t *tC10) {
    static uint8_t buf[DS2438_MEM_SIZE];
    if (!battery.readDS2438(buf, DS2438_MEM_SIZE)) return false;
    memcpy(batteryDump2438, buf, DS2438_MEM_SIZE);
    hasDump2438 = true;
    *mv = impresVoltageMv(buf);
    // Струм зі знаком; при розряді від'ємний. Формула та сама, що в /api/info2438.
    int16_t raw = (int16_t)((buf[6] << 8) | buf[5]);
    *ma  = (int16_t)((float)raw / (4096.0f * DS2438_RSENSE_OHM) * 1000.0f);
    *tC10 = (int16_t)((((int16_t)((buf[2] << 8) | buf[1])) >> 3) * 0.3125f);  // 0.03125*10
    return true;
}

// Старт розряду. targetMv — до якої напруги (0 = DISCHARGE_TARGET_MV).
// Повертає nullptr при успіху, інакше — текст причини відмови.
const char *dischargeStart(uint16_t targetMv) {
    if (!dischargeAvailable()) return "Розряд не налаштовано: задайте LOAD_PIN у settings.h";
    if (dischargeRunning())    return "Розряд уже виконується";
    if (!targetMv) targetMv = DISCHARGE_TARGET_MV;

    // Ціль не нижче аварійної межі — інакше розряд гарантовано впреться в аварію.
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

    loadOn();
    ledSet(LED_DISCHARGE);
    Serial.printf("\n=== Discharge started: %u -> %u mV, expected %d mA ===\n",
                  mv, targetMv, dischargeExpectedMa(mv));
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
    if (now - g_dis.lastPollMs < DISCHARGE_POLL_MS) return;

    unsigned long dtMs = now - g_dis.lastPollMs;
    g_dis.lastPollMs = now;
    g_dis.elapsedS   = (now - g_dis.startMs) / 1000UL;

    uint16_t mv; int16_t ma, t;
    if (!dischargeSample(&mv, &ma, &t)) {
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

    // Інтеграл РЕАЛЬНОГО струму: мА*год ×1000 = |мА| * мс / 3600.
    // Свій шунт не потрібен — струм іде через вимірювальний резистор DS2438
    // всередині пакета.
    uint32_t absMa = (uint32_t)(ma < 0 ? -ma : ma);
    g_dis.mahX1000 += (absMa * dtMs) / 3600UL;

    g_dis.lastMv = mv; g_dis.lastMa = ma; g_dis.lastTempC10 = t;
    g_dis.lastDca = impresDca(batteryDump2438);

    Serial.printf("discharge: %u mV, %d mA, %.1f C, %lu mAh, %lus\n",
                  mv, ma, t / 10.0f, (unsigned long)dischargeMah(), (unsigned long)g_dis.elapsedS);

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
    String j = "{\"available\":" + String(dischargeAvailable() ? "true" : "false");
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
    j += ",\"dcaDelta\":" + String((int)(g_dis.lastDca - g_dis.startDca));
    j += ",\"elapsedS\":" + String((unsigned long)g_dis.elapsedS);
    j += ",\"polls\":"    + String(g_dis.polls);
    j += ",\"expectedMa\":" + String(dischargeExpectedMa(g_dis.lastMv));
    j += ",\"loadOhm\":"  + String(LOAD_OHM, 1);
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
        // Перерахунок контрольних сум записів. Раніше тут «шукали запис 0x17»
        // як «байт 0x17, за яким 0x00», тобто трактували довжину як тег і
        // правили суму випадковому запису. Тепер ідемо ЛАНЦЮГОМ від 0x120
        // (заводська таблиця + модель + навчені записи) і перераховуємо суму
        // лише тим записам, які її мають і в яких вона зараз хибна.
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
    bool ok = performFactoryClean();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Usage data cleared (identity kept)\"}"
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
    long ica = impresIcaFromMah(mah, impresRatedMah(smModel));
    batteryDump2438[12] = (uint8_t)ica;
    ledSet(LED_WRITE); displayShow("ЗАПИС ЄМН mAh");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    displayShow(ok ? "ЄМН mAh OK" : "ЄМН mAh ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    String m = String("{\"status\":\"") + (ok ? "success" : "error") +
               "\",\"ica\":" + ica + ",\"mah\":" + impresIcaToMah((uint8_t)ica, impresRatedMah(smModel)) + "}";
    server.send(ok ? 200 : 500, "application/json", m);
}

// Рівень заряду з поточної напруги: 7.0 В = 0%, 8.4 В = 100% (лінійно).
inline int chargePctFromVoltage() {
    // Спільна шкала з batteryPercent() і підготовкою до калібрування
    // (BATTERY_EMPTY_MV..BATTERY_FULL_MV з settings.h).
    return impresPercentFromMv((int)impresVoltageMv(batteryDump2438));
}
// Записати рівень заряду у % в регістр ICA (DS2438[12]).
inline bool performSetChargePct(int pct) {
    if (!hasDump2438) return false;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int ica = pct * ICA_FULL_SCALE / 100;
    if (ica > 255) ica = 255;
    batteryDump2438[12] = (uint8_t)ica;
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    return ok;
}
// Виставити рівень заряду (ICA): auto=1 — з напруги (7.0В=0%..8.4В=100%),
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
    bool ok = performReset();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Battery counters reset\"}"
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

// ------------------- Ініціалізація нового акумулятора -------------------
// Порожній/стертий/невідомий чіп -> робочий АКБ обраної моделі. Вантажимо
// вшитий genuine-еталон (DS2433 + DS2438), зануляємо всю історію/лічильники
// (як заводська очистка), ставимо здоров'я 100%, а введену вручну ємність у
// мА·год пишемо в ICA (поточний заряд). Дзеркало калібрування DS2438<->DS2433
// у шаблоні вже узгоджене. Пишемо ОБИДВІ мікросхеми.
bool performInitBattery(const char *model, long mah) {
    int t = findTemplate(model);
    if (t < 0) { displayShow("НЕМА ШАБЛОНУ"); return false; }

    Serial.printf("\n=== Init new battery: %s, %ld mAh ===\n", model, mah);
    memcpy_P(batteryDump,     BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
    memcpy_P(batteryDump2438, BATTERY_TEMPLATES[t].d38, DS2438_MEM_SIZE);
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
    factoryCleanData();
    impresFixHeader(batteryDump);

    // Монітор — у стан «новий пакет» (конфіг/поріг/дзеркало зберігаються).
    // Введена ємність (поточний заряд) у мА·год -> регістр ICA DS2438.
    long ica = impresIcaFromMah(mah, impresRatedMah(model));
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
bool performRestoreTemplate(const char *model, bool *ok33 = nullptr, bool *ok38 = nullptr,
                            bool verbatim = false) {
    if (ok33) *ok33 = false;
    if (ok38) *ok38 = false;
    int t = findTemplate(model);
    if (t < 0) { displayShow("НЕМА ШАБЛОНУ"); return false; }

    Serial.printf("\n=== Restore %s: %s ===\n", verbatim ? "VERBATIM" : "model-part", model);
    memcpy_P(batteryDump,     BATTERY_TEMPLATES[t].d33, DUMP_SIZE);
    memcpy_P(batteryDump2438, BATTERY_TEMPLATES[t].d38, DS2438_MEM_SIZE);
    if (!verbatim) {
        int cleared = applyFreshTail(BATTERY_TEMPLATES[t].fresh);
        if (cleared < 0) cleared = impresEraseTail(batteryDump);
        impresFixHeader(batteryDump);
        impresResetMonitor(batteryDump2438, batteryDump,
                           impresIcaFromPercent(impresPercentFromMv(impresVoltageMv(batteryDump2438))));
        Serial.printf("Restore: donor learned tail cleared: %d B\n", cleared);
    }

    ledSet(LED_WRITE); displayShow("ВІДНОВЛ. ЕТАЛОН");
    bool w33 = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (w33) { hasDump = true; saveDump("/dump.bin", batteryDump, DUMP_SIZE); }
    bool w38 = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (w38) { hasDump2438 = true; saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE); }

    if (ok33) *ok33 = w33;
    if (ok38) *ok38 = w38;
    bool ok = w33;                       // ідентичність критична; DS2438 може бути відсутнім
    displayShow(w33 && w38 ? "ЕТАЛОН OK" : (w33 ? "2438 ЗБІЙ" : "ЕТАЛОН ЗБІЙ"));
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.printf("Restore: DS2433=%s DS2438=%s\n", w33 ? "OK" : "FAIL", w38 ? "OK" : "FAIL");
    Serial.println("=== Restore completed ===\n");
    return ok;
}

// Веб-відновлення еталона (під паролем): model.
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
    bool ok = performRestoreTemplate(model.c_str(), &ok33, &ok38, verbatim);
    String j = String("{\"status\":\"") + (ok ? "success" : "error") + "\",\"ds2433\":" +
               (ok33 ? "true" : "false") + ",\"ds2438\":" + (ok38 ? "true" : "false") + ",\"message\":\"";
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

void handleOps() {
    String j = "{\"status\":\"success\",\"ops\":[";
    bool first = true;
    auto add = [&](const char *key, const char *title, const char *detail,
                   int danger, const char *model) {
        if (!first) j += ",";
        first = false;
        j += "{\"key\":\""; j += key; j += "\",\"title\":\""; j += title;
        j += "\",\"detail\":\""; j += detail;
        j += "\",\"danger\":" + String(danger);
        j += ",\"model\":\""; j += (model ? model : ""); j += "\"}";
    };
    for (int i = 0; i < OP_BASE_COUNT; i++)
        add(OP_DOC[i].key, OP_DOC[i].title, OP_DOC[i].detail, OP_TEXT[i].danger, nullptr);
    for (int t = 0; t < BATTERY_TEMPLATE_COUNT; t++)
        add("model", "Записати модельну частину еталона",
            "Ідентичність, розрядна крива, COPYRIGHT, заводська таблиця й запис моделі. Навчений калібрувальний хвіст НЕ переноситься — інакше пакет отримав би чужу калібровку.",
            OPD_WRITE, BATTERY_TEMPLATES[t].name);
    for (int t = 0; t < BATTERY_TEMPLATE_COUNT; t++)
        add("new", "Новий АКБ з порожнього чипа",
            "Записує модельну частину еталона й приводить монітор у стан нового пакета. Навчена калібровка лишається порожньою — її запише зарядна станція під час калібрування.",
            OPD_WIPE, BATTERY_TEMPLATES[t].name);
    for (int e = 0; e < OP_EXPERT_COUNT; e++)
        add(OP_DOC_EXPERT[e].key, OP_DOC_EXPERT[e].title, OP_DOC_EXPERT[e].detail,
            OP_TEXT_EXPERT[e].danger, nullptr);
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
    server.send(200, "application/json", wizExecStep(idx, model));
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
    server.send(ok ? 200 : 500, "application/json",
        ok ? (String("{\"status\":\"success\",\"message\":\"Новий АКБ ") + model + " записано\"}")
           : "{\"status\":\"error\",\"message\":\"Збій запису (див. Serial-лог)\"}");
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
    server.on("/api/discharge", HTTP_GET, handleDischargeStatus);        // стан розряду
    server.on("/api/discharge/start", HTTP_POST, handleDischargeStart);  // почати розряд
    server.on("/api/discharge/stop", HTTP_POST, handleDischargeStop);    // зупинити розряд
    server.on("/api/initbattery", HTTP_POST, handleInitBattery); // ініціалізація нового АКБ
    server.on("/api/restore", HTTP_POST, handleRestore);         // відновлення еталона verbatim
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
