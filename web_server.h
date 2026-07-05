#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "battery_reader.h"
#include "settings.h"
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

// ---------------------------------------------------------------------------
// Перевірка пароля адміністратора. РАНІШЕ: `if (hasArg && arg != PW)` —
// якщо клієнт просто НЕ надсилав аргумент password, перевірка проходила і
// запис виконувався БЕЗ пароля (діра в безпеці). Тепер пароль ОБОВ'ЯЗКОВИЙ:
// має бути присутній І збігатися. Усі веб-обробники запису викликають
// requireAdmin() першим рядком.
static bool adminOk() {
    return server.hasArg("password") && server.arg("password") == ADMIN_PASSWORD;
}
static bool requireAdmin() {
    if (!adminOk()) {
        server.send(403, "application/json",
            "{\"status\":\"error\",\"message\":\"Невірний або відсутній пароль адміністратора\"}");
        return false;
    }
    return true;
}

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

    memset(batteryDump, 0, DUMP_SIZE);
    memset(batteryDump2438, 0, DS2438_MEM_SIZE);

    // DS2433 — основний дамп (512 байт).
    ok2433 = battery.readBattery(batteryDump, DUMP_SIZE);
    if (ok2433) {
        hasDump = true;
        saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    }

    // DS2438 — монітор батареї (64 байта).
    ok2438 = battery.readDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok2438) {
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
    json += ",\"icaMah\":" + String((int)(ica * DS2438_MAH_PER_LSB));
    json += ",\"ccaMah\":" + String((int)(cca * DS2438_MAH_PER_LSB));
    json += ",\"dcaMah\":" + String((int)(dca * DS2438_MAH_PER_LSB));
    // Цикли: сумарний заряд (розряд) / паспортна ємність (BATTERY_RATED_MAH).
    json += ",\"ccaCycles\":" + String((int)(cca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH));
    json += ",\"dcaCycles\":" + String((int)(dca * DS2438_MAH_PER_LSB / BATTERY_RATED_MAH));
    json += ",\"ratedMah\":" + String((int)BATTERY_RATED_MAH);
    json += ",\"charge\":" + String(charge);
    json += ",\"chargeSrc\":\"" + String(csrc) + "\"";
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
    json += ",\"preview\":\"";
    
    json += hexPreview(batteryDump, 16);
    json += "\",\"hex\":\"" + hexPreview(batteryDump, DUMP_SIZE) + "\"}";

    server.send(200, "application/json", json);
}

// Скидання лічильників використання / зносу для рекалібрування на оригінальної ЗУ.
// Обнуляє CCA/DCA/ETM в DS2438 і їх дзеркало в DS2433 (запис 0x0D), скидає
// відображувану ємність на 100% (знос 0). контрольні суми зачеплених записів
// перераховуються (Σ==0x5A). Калібрування (offset-регістр DS2438) зберігається.
void resetBatteryData() {
    if (hasDump2438) {
        for (int i = 8; i <= 11; i++) batteryDump2438[i] = 0; // ETM (таймер)
        batteryDump2438[60] = batteryDump2438[61] = 0;         // CCA
        batteryDump2438[62] = batteryDump2438[63] = 0;         // DCA
    }
    if (hasDump) {
        // Дзеркало CCA/DCA в записи 0x0D одразу після моделі ("0B <модель> ... 0D ...").
        // Модель — 0x0B + літера (PMNN…, PT…, NNTN… — перевірено на дампах), а не
        // лише "PMN": інакше скидання дзеркала мовчки пропускалось для PT4409A тощо.
        for (int i = 0x100; i < 0x1F0 - 13; i++) {
            if (batteryDump[i] == 0x0B && batteryDump[i + 1] >= 'A' && batteryDump[i + 1] <= 'Z') {
                for (int j = i + 10; j < i + 30 && j < 0x1F0 - 13; j++) {
                    if (batteryDump[j] == 0x0D) {
                        // Дзеркало лічильників у записі 0x0D: CCA — 16-біт LE у
                        // байтах [j+4,j+5], DCA — [j+6,j+7] (перевірено на реальних
                        // дампах: значення збігаються з CCA/DCA у DS2438[60..63]).
                        // Обнуляємо ОБИДВА байти кожного, інакше при лічильнику
                        // ≥256 старший байт лишався ненульовим.
                        batteryDump[j + 4] = batteryDump[j + 5] = 0; // CCA
                        batteryDump[j + 6] = batteryDump[j + 7] = 0; // DCA
                        fixRecordChecksum(batteryDump, j, 0x0D);
                        break;
                    }
                }
                break;
            }
        }
        // Ємність -> 100% (знос 0) в записи історії 0x17
        for (int i = 0x100; i < 0x1F0 - 23; i++) {
            if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) {
                batteryDump[i + 21] = 0x64;
                fixRecordChecksum(batteryDump, i, 0x17);
                break;
            }
        }
    }
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

// Очистка "до заводського": стирає ВСІ дані використання/історії/статистики,
// зберігаючи КРИТИЧНЕ (модель, серійний/ID, крива калібрування, блок автент.,
// дзеркало DS2438↔DS2433, заголовок). Виконує повне скидання лічильників плюс
// обнуляє записи статистики використання 0x16 (22 байти, Σ==0x5A -> дані 0,
// контрольна сума 0x44 — перевірено: у робочого 4488A такий "порожній" 0x16 є).
void factoryCleanData() {
    resetBatteryData();                 // ETM/CCA/DCA/0x0D/ємність 0x17 -> свіжі
    if (hasDump) {
        // Обидві записи статистики 0x16 -> порожні. Шукаємо валідні (Σ==0x5A).
        for (int i = 0x140; i < 0x1C0 - 22; i++) {
            if (batteryDump[i] == 0x16 && batteryDump[i + 1] == 0x00) {
                int s = 0; for (int k = 0; k < 22; k++) s += batteryDump[i + k];
                if ((s & 0xFF) != 0x5A) continue;      // не запис 0x16 — пропускаємо
                for (int k = 1; k < 21; k++) batteryDump[i + k] = 0;
                fixRecordChecksum(batteryDump, i, 22); // -> 0x44
            }
        }
        fixHeaderChecksum(batteryDump);
    }
}

// Стерти LEARNED-калібрування (TLV-записи 0x0A, Σ≡0x5A) у калібр-зоні DS2433.
// Перевірено діффом однієї й тієї ж АКБ ДО/ПІСЛЯ калібрування на IMPRES-ЗП:
// саме запис 0x0A ЗП створює під час навчання (він кодує виміряну ємність/
// імпеданс РЕАЛЬНИХ банок). Після заміни елементів старий 0x0A суперечить новим
// банкам → рація каже «невідома». Стирання його (→0xFF) повертає пакет у стан
// «валідний, НЕ відкалібрований», який рація приймає, а ЗП бере на калібрування.
// Повертає к-сть стертих записів.
int eraseLearnedCalib() {
    int erased = 0;
    // Скануємо ПІСЛЯ кривої розряду (вона в 0x42..0x67) і до записів моделі —
    // саме тут ЗП тримає learned-калібрування; так не зачепимо криву випадково.
    for (int i = 0x68; i < 0x180 - 10; i++) {
        if (batteryDump[i] == 0x0A) {
            int s = 0; for (int k = 0; k < 10; k++) s += batteryDump[i + k];
            if ((s & 0xFF) == 0x5A) {                  // валідний запис 0x0A
                for (int k = 0; k < 10; k++) batteryDump[i + k] = 0xFF;
                erased++; i += 9;
            }
        }
    }
    return erased;
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
//   1) стерти learned-запис(и) 0x0A (→0xFF);
//   2) обнулити лічильники/історію (0x0D CCA/DCA, 0x16, DS2438 ICA/CCA/DCA/ETM),
//      здоров'я 0x17 → 100%;
//   3) перерахувати суми, синхронізувати дзеркало.
// ⚠️ Фізичну калібровку це НЕ замінює — далі АКБ обов'язково калібрувати на ЗП.
bool performRecalPrepare() {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }
    Serial.println("\n=== Prepare for recalibration ===");
    ledSet(LED_WRITE); displayShow("ПІД КАЛІБР...");
    int er = 0;
    if (hasDump) {
        er = eraseLearnedCalib();          // прибрати старе learned-калібрування
        Serial.printf("erased learned-calib records: %d\n", er);
    }
    factoryCleanData();                     // лічильники/історія/0x17=100% + суми
    if (hasDump && hasDump2438 && !mirrorOk(batteryDump, batteryDump2438))
        syncMirrorFrom2438(batteryDump, batteryDump2438);
    if (hasDump) fixHeaderChecksum(batteryDump);
    bool ok = true;
    if (hasDump)     ok &= battery.writeBattery(batteryDump, DUMP_SIZE);
    if (hasDump2438) ok &= battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) {
        if (hasDump)     saveDump("/dump.bin", batteryDump, DUMP_SIZE);
        if (hasDump2438) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
        displayShow("КАЛІБР: ГОТОВО");
    } else displayShow("КАЛІБР ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    Serial.println("=== Recal-prepare completed ===\n");
    return ok;
}

// ------------------- Ремонт / правка / зміна ємності -------------------

// Перерахунок "відновних" полів поточних дампів: контрольна сума заголовка
// DS2433, дзеркало калібрування (з уцілілого DS2438 в DS2433), контрольні суми
// відомих записів (0x0D CCA/DCA і 0x17 історія ємності). НЕ чіпає дані —
// лише виправляє цілісність, щоб рація знову прийняла підправлену прошивку.
void repairDumps() {
    if (hasDump && hasDump2438 && !mirrorOk(batteryDump, batteryDump2438)) {
        // DS2438 зазвичай зберігається при пошкодженні DS2433 — беремо калібрування з нього.
        syncMirrorFrom2438(batteryDump, batteryDump2438);
        Serial.println("repair: mirror DS2438->DS2433 restored");
    }
    if (hasDump) {
        fixHeaderChecksum(batteryDump);
        // Перерахунок контрольної суми записи історії ємності 0x17 (якщо знайдена).
        for (int i = 0x100; i < 0x1F0 - 23; i++)
            if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) { fixRecordChecksum(batteryDump, i, 23); break; }
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

// Знаходить зсув запису 0x0B з назвою моделі (0x0B + літера A-Z) або -1.
// Скануємо з 0x30 (одразу після заголовка/дзеркала) до кінця: на всіх реальних
// дампах єдиний збіг «0x0B + літера» — саме запис моделі, тож ширший діапазон
// безпечний і ловить запис навіть якщо він нижче 0x100.
int findModelRecord() {
    for (int i = 0x30; i < (int)DUMP_SIZE - 11; i++)
        if (batteryDump[i] == 0x0B && batteryDump[i + 1] >= 'A' && batteryDump[i + 1] <= 'Z') return i;
    return -1;
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
    if (rec >= 0) {
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
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"У поточному дампі немає запису моделі (0x0B) чи ASCII-заголовка для перезапису — це порожній/стертий/невідомий чіп. Спочатку відновіть еталонний дамп цієї моделі (Прошивка → «Відновлення / запис DS2433»), потім змінюйте модель.\"}"); return;
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

// Змінити відображувану ємність/знос (0..100 %) і записати в АКБ. Редагує
// останню пробу в записи історії ємності 0x17 + її контрольну суму.
void handleSetCapacity() {
    if (!requireAdmin()) return;
    if (!hasDump) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return; }
    if (!server.hasArg("cap")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No cap\"}"); return; }
    int cap = server.arg("cap").toInt();
    if (cap < 0) cap = 0; if (cap > 100) cap = 100;

    int rec = -1;
    for (int i = 0x100; i < 0x1F0 - 23; i++)
        if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) { rec = i; break; }
    if (rec < 0) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Capacity record not found\"}"); return; }

    batteryDump[rec + 21] = (uint8_t)cap;      // остання проба ємності, %
    fixRecordChecksum(batteryDump, rec, 23);   // контрольна сума записи (Σ≡0x5A)

    ledSet(LED_WRITE); displayShow("ЗАПИС ЄМН...");
    bool ok = battery.writeBattery(batteryDump, DUMP_SIZE);
    if (ok) { saveDump("/dump.bin", batteryDump, DUMP_SIZE); displayShow("ЄМН. OK"); }
    else displayShow("ЄМН. ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Capacity written\"}"
           : "{\"status\":\"error\",\"message\":\"Write failed\"}");
}

// Змінити залишкову ємність (заряд) в мА·ч і записати в DS2438. Пише регістр
// ICA (байт 12): ICA = mAh / (0.4882/Rsense), 0..255. Це то, що рація
// показує як рівень заряду — тепер задається в мА·ч, а не в відсотках.
void handleSetMah() {
    if (!requireAdmin()) return;
    if (!hasDump2438) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}"); return; }
    if (!server.hasArg("mah")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No mah\"}"); return; }
    long mah = server.arg("mah").toInt();
    long ica = (long)(mah / DS2438_MAH_PER_LSB + 0.5f);
    if (ica < 0) ica = 0; if (ica > 255) ica = 255;
    batteryDump2438[12] = (uint8_t)ica;
    ledSet(LED_WRITE); displayShow("ЗАПИС ЄМН mAh");
    bool ok = battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok) saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    displayShow(ok ? "ЄМН mAh OK" : "ЄМН mAh ЗБІЙ");
    ledSet(ok ? LED_OK : LED_ERROR);
    String m = String("{\"status\":\"") + (ok ? "success" : "error") +
               "\",\"ica\":" + ica + ",\"mah\":" + (long)(ica * DS2438_MAH_PER_LSB) + "}";
    server.send(ok ? 200 : 500, "application/json", m);
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
            if (hasDump2438) { /* держим зеркало согласованным */ for (int i=0;i<26;i++) buf[1+i]=batteryDump2438[24+i]; fixHeaderChecksum(buf); }
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
    bool ok = performRecalPrepare();
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

    // Свіжий стан: зануляємо лічильники/історію/статистику, лишаємо
    // ідентичність/калібрування/дзеркало. Ставить і здоров'я 0x17 -> 100%.
    factoryCleanData();
    // ВАЖЛИВО: стерти learned-калібрування ДОНОРА (запис 0x0A). Інакше рація
    // бачить чужі виміряні дані й каже «невідома» (саме тому «Новий АКБ» раніше
    // не приймався). Без 0x0A пакет — «валідний, не відкалібрований»: рація
    // приймає, а ЗП калібрує реальні банки.
    eraseLearnedCalib();
    fixHeaderChecksum(batteryDump);

    // Введена ємність (поточний заряд) у мА·год -> регістр ICA DS2438.
    long ica = (long)(mah / DS2438_MAH_PER_LSB + 0.5f);
    if (ica < 0) ica = 0; if (ica > 255) ica = 255;
    batteryDump2438[12] = (uint8_t)ica;

    fixHeaderChecksum(batteryDump);   // узгодити суму заголовка (дзеркало вже збігається)

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

// Список доступних вшитих моделей-шаблонів (для випадаючого списку у вебі/USB).
void handleTemplates() {
    String j = "{\"status\":\"success\",\"models\":[";
    for (int i = 0; i < BATTERY_TEMPLATE_COUNT; i++) {
        if (i) j += ",";
        j += "\""; j += BATTERY_TEMPLATES[i].name; j += "\"";
    }
    j += "]}";
    server.send(200, "application/json", j);
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
    server.on("/api/repair", HTTP_POST, handleRepair);          // ремонт цілісності
    server.on("/api/setmodel", HTTP_POST, handleSetModel);       // ручний запис моделі
    server.on("/api/setcapacity", HTTP_POST, handleSetCapacity); // змінити ємність %
    server.on("/api/setmah", HTTP_POST, handleSetMah);           // змінити залишок, мА·ч
    server.on("/api/writehex", HTTP_POST, handleWriteHex);       // сира запис з редактора
    server.on("/api/reboot", HTTP_POST, handleReboot);           // перезавантаження ESP32
    server.on("/api/templates", HTTP_GET, handleTemplates);      // список вшитих моделей
    server.on("/api/initbattery", HTTP_POST, handleInitBattery); // ініціалізація нового АКБ
    server.on("/api/recalprep", HTTP_POST, handleRecalPrepare);  // підготовка до рекалібрування

    // Captive-portal: усі інші URL -> редирект на головну (авто-відкриття сторінки).
    server.onNotFound(handleCaptive);

    server.begin();
    Serial.println("Web server started");
}

#endif
