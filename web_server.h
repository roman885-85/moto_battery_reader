#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "battery_reader.h"
#include "settings.h"
#include "display.h"

extern WebServer server;
extern BatteryReader battery;
extern uint8_t batteryDump[DUMP_SIZE];
extern bool hasDump;
extern uint8_t batteryDump2438[DS2438_MEM_SIZE];
extern bool hasDump2438;
extern uint8_t chipSN2438[8];
extern bool hasSN2438;

// Сохранение дампа в SPIFFS (перезапись файла).
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

// HEX-превью первых n байт в JSON-строку ("AA BB CC ...").
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

// Логотип: отдаём /logo.png из SPIFFS, если он загружен (иначе 404 -> в вебе
// показывается встроенный SVG-тризуб). Позволяет использовать точный логотип НГУ.
void handleLogo() {
    if (SPIFFS.exists("/logo.png")) {
        File f = SPIFFS.open("/logo.png", "r");
        server.streamFile(f, "image/png");
        f.close();
    } else {
        server.send(404, "text/plain", "no logo");
    }
}

// Обработчик главной страницы
void handleRoot() {
    File file = SPIFFS.open("/index.html", "r");
    if (!file) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

// Чтение обеих микросхем (DS2433 + DS2438) с сохранением в SPIFFS и на дисплей.
// Возвращает true, если считана хотя бы одна микросхема.
bool readAllChips(bool &ok2433, bool &ok2438) {
    displayShow("ЗЧИТУВАННЯ...");

    memset(batteryDump, 0, DUMP_SIZE);
    memset(batteryDump2438, 0, DS2438_MEM_SIZE);

    // DS2433 — основной дамп (512 байт).
    ok2433 = battery.readBattery(batteryDump, DUMP_SIZE);
    if (ok2433) {
        hasDump = true;
        saveDump("/dump.bin", batteryDump, DUMP_SIZE);
    }

    // DS2438 — монитор батареи (64 байта).
    ok2438 = battery.readDS2438(batteryDump2438, DS2438_MEM_SIZE);
    if (ok2438) {
        hasDump2438 = true;
        saveDump("/dump2438.bin", batteryDump2438, DS2438_MEM_SIZE);
    }

    // Серийный номер чипа (лазерный ROM-ID DS2438)
    if (battery.hasRom2438()) {
        memcpy(chipSN2438, battery.rom2438(), 8);
        hasSN2438 = true;
    }

    char st[40];
    if (ok2433 || ok2438) snprintf(st, sizeof(st), "ЧИТ 2433:%s 2438:%s", ok2433 ? "OK" : "-", ok2438 ? "OK" : "-");
    else                  snprintf(st, sizeof(st), "ПОМИЛКА: нема чіпа");
    displayShow(st);

    return ok2433 || ok2438;
}

// Обработчик чтения дампа: считываем обе микросхемы (DS2433 + DS2438).
void handleReadDump() {
    Serial.println("Starting battery read...");

    bool ok2433, ok2438;
    readAllChips(ok2433, ok2438);

    if (ok2433 || ok2438) {
        String json = String("{\"status\":\"success\",\"ds2433\":") + (ok2433 ? "true" : "false") +
                      ",\"ds2438\":" + (ok2438 ? "true" : "false") + "}";
        server.send(200, "application/json", json);

        digitalWrite(LED_GREEN_PIN, HIGH);
        delay(200);
        digitalWrite(LED_GREEN_PIN, LOW);
    } else {
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read battery\"}");

        digitalWrite(LED_RED_PIN, HIGH);
        delay(500);
        digitalWrite(LED_RED_PIN, LOW);
    }
}

// Обработчик скачивания дампа
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

// Обработчик скачивания дампа DS2438
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

// Upload-колбэк (ufn) для файла DS2438 -> /upload2438.bin
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

// Обработчик запроса /upload2438 (fn): отправляет ответ после приёма файла.
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

// Обработчик записи дампа в DS2438
void handleWriteDump2438() {
    if (server.hasArg("password") && server.arg("password") != ADMIN_PASSWORD) {
        server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Invalid password\"}");
        return;
    }

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
    displayShow("ЗАПИС 2438...");
    if (battery.writeDS2438(buffer, DS2438_MEM_SIZE)) {
        memcpy(batteryDump2438, buffer, DS2438_MEM_SIZE);
        hasDump2438 = true;
        saveDump("/dump2438.bin", buffer, DS2438_MEM_SIZE);

        Serial.println("✓✓✓ DS2438 WRITE SUCCESSFUL ✓✓✓");
        displayShow("2438 ЗАПИС OK");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"DS2438 written successfully\"}");

        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            delay(200);
            digitalWrite(LED_GREEN_PIN, LOW);
            delay(200);
        }
    } else {
        Serial.println("✗✗✗ DS2438 WRITE FAILED ✗✗✗");
        displayShow("2438 ЗАПИС ЗБІЙ");
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write DS2438\"}");

        digitalWrite(LED_RED_PIN, HIGH);
        delay(500);
        digitalWrite(LED_RED_PIN, LOW);
    }
    Serial.println("=== DS2438 write request completed ===\n");
}

// Информация о DS2438: превью + расшифрованные напряжение/температура (стр. 0).
void handleDumpInfo2438() {
    if (!hasDump2438) {
        server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"No dump available\"}");
        return;
    }

    // Страница 0: [1]=Temp LSB, [2]=Temp MSB, [3]=V LSB, [4]=V MSB, [5]=I LSB, [6]=I MSB
    uint16_t vraw = ((uint16_t)batteryDump2438[4] << 8) | batteryDump2438[3];
    float voltage = vraw * 0.01f; // 10 мВ/LSb

    int16_t traw = ((int16_t)((batteryDump2438[2] << 8) | batteryDump2438[1])) >> 3; // 13-бит
    float temperature = traw * 0.03125f; // °C/LSb

    int16_t current = (int16_t)((batteryDump2438[6] << 8) | batteryDump2438[5]); // сырое значение

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
    // Циклы: суммарный заряд (разряд) / паспортная ёмкость (BATTERY_RATED_MAH).
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

// Обработчик загрузки файла - переработан для корректной работы с ESP32 WebServer
void handleUploadDump() {
    static File uploadFile;
    static size_t uploadedBytes = 0;
    
    HTTPUpload &upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("\n=== Upload started: %s ===\n", upload.filename.c_str());
        
        // Проверяем свободное место
        size_t totalBytes = SPIFFS.totalBytes();
        size_t usedBytes = SPIFFS.usedBytes();
        size_t freeBytes = totalBytes - usedBytes;
        
        Serial.printf("SPIFFS Status: Total=%d, Used=%d, Free=%d\n", totalBytes, usedBytes, freeBytes);
        
        // Закрываем старый файл если он еще открыт
        if (uploadFile) {
            uploadFile.close();
            delay(50);
        }
        
        // Удаляем старый файл
        if (SPIFFS.exists("/upload.bin")) {
            SPIFFS.remove("/upload.bin");
            delay(100);
        }
        
        // Открываем новый файл
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
        
        // Пишем данные в файл
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
            delay(200);  // Критическая задержка для синхронизации SPIFFS
            
            Serial.printf("Upload finished: %s (%d bytes total)\n", 
                         upload.filename.c_str(), uploadedBytes);
            
            // Проверяем результат
            delay(100);
            if (SPIFFS.exists("/upload.bin")) {
                File file = SPIFFS.open("/upload.bin", "r");
                if (file) {
                    size_t size = file.size();
                    Serial.printf("✓ File created: %d bytes\n", size);
                    
                    // Проверяем первые байты
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
        // Ответ отправляет handleUploadDone() (обработчик запроса), а не upload-колбэк.

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) {
            uploadFile.close();
        }
        Serial.println("Upload aborted!");
    }
}

// Обработчик запроса /upload (fn): вызывается после того, как upload-колбэк
// (ufn) полностью принял тело multipart-формы. Отправляет HTTP-ответ.
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

// Обработчик записи дампа
void handleWriteDump() {
    // Проверяем пароль
    if (server.hasArg("password") && server.arg("password") != ADMIN_PASSWORD) {
        server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Invalid password\"}");
        return;
    }
    
    Serial.println("\n=== Write request received ===");
    
    // Проверяем наличие файла
    if (!SPIFFS.exists("/upload.bin")) {
        Serial.println("✗ /upload.bin does not exist - upload a file first");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    // Открываем и проверяем размер
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
    
    // Читаем весь файл в буфер
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
    
    // Выводим первые байты
    Serial.printf("Data: ");
    for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();
    
    // Пишем в батарею
    Serial.println("Writing to battery chip...");
    displayShow("ЗАПИС 2433...");
    if (battery.writeBattery(buffer, DUMP_SIZE)) {
        memcpy(batteryDump, buffer, DUMP_SIZE);
        hasDump = true;
        
        // Сохраняем как текущий дамп
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
        
        // Индикация успеха
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            delay(200);
            digitalWrite(LED_GREEN_PIN, LOW);
            delay(200);
        }
    } else {
        Serial.println("✗✗✗ WRITE FAILED ✗✗✗");
        displayShow("2433 ЗАПИС ЗБІЙ");
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write battery\"}");
        
        digitalWrite(LED_RED_PIN, HIGH);
        delay(500);
        digitalWrite(LED_RED_PIN, LOW);
    }
    Serial.println("=== Write request completed ===\n");
}

// Обработчик информации о дампе
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

    String json = "{\"size\":512,\"hasData\":true";
    json += ",\"model\":\"" + modelStr + "\"";
    json += ",\"capacity\":" + String(cap);
    json += ",\"wear\":" + String(wear);
    json += ",\"genuine\":" + String(genuine ? "true" : "false");
    json += ",\"authReason\":\"" + String(reason) + "\"";
    json += ",\"preview\":\"";
    
    json += hexPreview(batteryDump, 16);
    json += "\",\"hex\":\"" + hexPreview(batteryDump, DUMP_SIZE) + "\"}";

    server.send(200, "application/json", json);
}

// Сброс счётчиков использования / износа для рекалибровки на оригинальной ЗУ.
// Обнуляет CCA/DCA/ETM в DS2438 и их зеркало в DS2433 (запись 0x0D), сбрасывает
// отображаемую ёмкость на 100% (износ 0). Контрольные суммы затронутых записей
// пересчитываются (Σ==0x5A). Калибровка (offset-регистр DS2438) сохраняется.
void resetBatteryData() {
    if (hasDump2438) {
        for (int i = 8; i <= 11; i++) batteryDump2438[i] = 0; // ETM (таймер)
        batteryDump2438[60] = batteryDump2438[61] = 0;         // CCA
        batteryDump2438[62] = batteryDump2438[63] = 0;         // DCA
    }
    if (hasDump) {
        // Зеркало CCA/DCA в записи 0x0D сразу после модели ("0B 'PMNN' ... 0D ...")
        for (int i = 0x100; i < 0x1F0 - 13; i++) {
            if (batteryDump[i] == 0x0B && batteryDump[i + 1] == 'P' &&
                batteryDump[i + 2] == 'M' && batteryDump[i + 3] == 'N') {
                for (int j = i + 10; j < i + 30 && j < 0x1F0 - 13; j++) {
                    if (batteryDump[j] == 0x0D) {
                        batteryDump[j + 3] = batteryDump[j + 4] = 0; // CCA
                        batteryDump[j + 5] = batteryDump[j + 6] = 0; // DCA
                        fixRecordChecksum(batteryDump, j, 0x0D);
                        break;
                    }
                }
                break;
            }
        }
        // Ёмкость -> 100% (износ 0) в записи истории 0x17
        for (int i = 0x100; i < 0x1F0 - 23; i++) {
            if (batteryDump[i] == 0x17 && batteryDump[i + 1] == 0x00) {
                batteryDump[i + 21] = 0x64;
                fixRecordChecksum(batteryDump, i, 0x17);
                break;
            }
        }
    }
}

// Ядро сброса: правит дампы, пишет в обе микросхемы, сохраняет. Без HTTP —
// вызывается и из веб-обработчика, и из меню на дисплее (по кнопкам).
bool performReset() {
    if (!hasDump && !hasDump2438) { displayShow("СПОЧАТКУ ЧИТАЙ"); return false; }

    Serial.println("\n=== Battery reset (recalibration) ===");
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
    Serial.println("=== Reset completed ===\n");
    return ok;
}

// Веб-обработчик сброса (под паролем).
void handleResetBattery() {
    if (server.hasArg("password") && server.arg("password") != ADMIN_PASSWORD) {
        server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Invalid password\"}");
        return;
    }
    if (!hasDump && !hasDump2438) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Read battery first\"}");
        return;
    }
    bool ok = performReset();
    server.send(ok ? 200 : 500, "application/json",
        ok ? "{\"status\":\"success\",\"message\":\"Battery counters reset\"}"
           : "{\"status\":\"error\",\"message\":\"Failed to write reset\"}");
}

// Captive-portal: любой неизвестный URL (или запрос по чужому домену) перенаправляем
// на главную страницу. В связке с DNS-сервером (все домены -> 192.168.4.1) телефон/ПК
// определяет "экран входа в сеть" и сам предлагает открыть страницу при подключении.
//
// ОС-детекторы captive-portal (Android /generate_204, Apple /hotspot-detect.html,
// Windows /connecttest.txt|/ncsi.txt) ждут "успех"; получив 302-редирект вместо него,
// система показывает уведомление и открывает нашу страницу автоматически.
void handleCaptive() {
    server.sendHeader("Location", String("http://") + ESP_IP + "/", true);
    server.send(302, "text/plain", "");
}

// Настройка веб-сервера
void setupWebServer() {
    if (!SPIFFS.begin(true)) {
        Serial.println("ERROR: SPIFFS mount failed");
        return;
    }
    
    // Проверяем состояние SPIFFS при запуске
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
    // Загрузка файла: 4-аргументная форма — handleUploadDone это обработчик
    // запроса (fn, отправляет ответ), handleUploadDump — upload-колбэк (ufn,
    // принимает тело multipart и пишет его в SPIFFS).
    server.on("/upload", HTTP_POST, handleUploadDone, handleUploadDump);

    // Микросхема DS2438 (монитор батареи)
    server.on("/api/download2438", HTTP_GET, handleDownloadDump2438);
    server.on("/api/info2438", HTTP_GET, handleDumpInfo2438);
    server.on("/api/write2438", HTTP_POST, handleWriteDump2438);
    server.on("/upload2438", HTTP_POST, handleUploadDone2438, handleUploadDump2438);
    server.on("/api/reset", HTTP_POST, handleResetBattery);

    // Captive-portal: все прочие URL -> редирект на главную (авто-открытие страницы).
    server.onNotFound(handleCaptive);

    server.begin();
    Serial.println("Web server started");
}

#endif
