#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "battery_reader.h"
#include "settings.h"

extern WebServer server;
extern BatteryReader battery;
extern uint8_t batteryDump[DUMP_SIZE];
extern bool hasDump;

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

// Обработчик чтения дампа
void handleReadDump() {
    Serial.println("Starting battery read...");
    
    memset(batteryDump, 0, DUMP_SIZE);
    
    if (battery.readBattery(batteryDump, DUMP_SIZE)) {
        hasDump = true;
        
        // Сохраняем в SPIFFS
        SPIFFS.remove("/dump.bin");
        delay(50);
        File file = SPIFFS.open("/dump.bin", "w");
        if (file) {
            size_t written = file.write(batteryDump, DUMP_SIZE);
            file.flush();
            file.close();
            delay(50);
            Serial.printf("Dump saved to SPIFFS: %d bytes\n", written);
        }
        
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Dump read successfully\"}");
        
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

// Обработчик загрузки файла - использует встроенную поддержку WebServer
void handleUploadDump() {
    HTTPUpload &upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("\n=== Upload started: %s ===\n", upload.filename.c_str());
        
        // Проверяем свободное место в SPIFFS
        size_t totalBytes = SPIFFS.totalBytes();
        size_t usedBytes = SPIFFS.usedBytes();
        size_t freeBytes = totalBytes - usedBytes;
        
        Serial.printf("SPIFFS Status: Total=%d, Used=%d, Free=%d\n", totalBytes, usedBytes, freeBytes);
        
        // Удаляем старый файл
        if (SPIFFS.exists("/upload.bin")) {
            SPIFFS.remove("/upload.bin");
            delay(50);
        }
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Serial.printf("Receiving chunk: %d bytes\n", upload.currentSize);
        
        // Пишем напрямую в SPIFFS используя встроенный буфер WebServer
        // Используем streamFile из WebServer напрямую
        size_t len = upload.currentSize;
        uint8_t *buf = upload.buf;
        
        // Открываем файл в режиме добавления
        File file = SPIFFS.open("/upload.bin", upload.totalSize == upload.currentSize ? "w" : "a");
        if (!file) {
            Serial.println("ERROR: Cannot open /upload.bin!");
            return;
        }
        
        // Пишем данные
        size_t written = 0;
        while (written < len) {
            size_t chunk = (len - written > 512) ? 512 : (len - written);
            size_t w = file.write(buf + written, chunk);
            if (w == 0) {
                Serial.println("ERROR: Write failed!");
                break;
            }
            written += w;
        }
        
        file.close();
        Serial.printf("Written %d bytes\n", written);
        
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload finished: %s (%d bytes total)\n", upload.filename.c_str(), upload.totalSize);
        
        // Даем время на синхронизацию
        delay(100);
        
        // Проверяем размер файла
        if (SPIFFS.exists("/upload.bin")) {
            File file = SPIFFS.open("/upload.bin", "r");
            if (file) {
                size_t fileSize = file.size();
                Serial.printf("File size in SPIFFS: %d bytes\n", fileSize);
                
                // Проверяем первые байты
                uint8_t header[16];
                size_t headerRead = file.read(header, 16);
                Serial.printf("Header: ");
                for (int i = 0; i < headerRead; i++) {
                    Serial.printf("%02X ", header[i]);
                }
                Serial.println();
                
                file.close();
            }
        } else {
            Serial.println("ERROR: /upload.bin not found after upload!");
        }
        
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"File uploaded\"}");
        Serial.println("=== Upload completed ===\n");
    }
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
        Serial.println("ERROR: /upload.bin does not exist");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    // Открываем и проверяем размер
    File file = SPIFFS.open("/upload.bin", "r");
    if (!file) {
        Serial.println("ERROR: Cannot open /upload.bin");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    size_t fileSize = file.size();
    Serial.printf("File size: %d bytes (expected: %d bytes)\n", fileSize, DUMP_SIZE);
    
    if (fileSize != DUMP_SIZE) {
        file.close();
        Serial.printf("ERROR: Invalid file size\n");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    
    // Читаем весь файл в буфер
    uint8_t buffer[DUMP_SIZE];
    memset(buffer, 0, DUMP_SIZE);
    
    size_t bytesRead = file.read(buffer, DUMP_SIZE);
    file.close();
    
    Serial.printf("Bytes read: %d\n", bytesRead);
    
    // Проверяем данные
    Serial.printf("Data header: ");
    for (int i = 0; i < 16 && i < bytesRead; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();
    
    if (bytesRead != DUMP_SIZE) {
        Serial.printf("ERROR: Read mismatch! Expected %d, got %d\n", DUMP_SIZE, bytesRead);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read uploaded file\"}");
        return;
    }
    
    // Пишем в батарею
    Serial.println("Writing to battery chip...");
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
            Serial.printf("Dump saved to SPIFFS: %d bytes\n", written);
        }
        
        Serial.println("✓ Write to battery SUCCESSFUL!");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Firmware written successfully\"}");
        
        // Индикация успеха
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            delay(200);
            digitalWrite(LED_GREEN_PIN, LOW);
            delay(200);
        }
    } else {
        Serial.println("✗ Write to battery FAILED!");
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write battery\"}");
        
        // Индикация ошибки
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
    
    String json = "{\"size\":512,\"hasData\":true,\"preview\":\"";
    
    for (int i = 0; i < 16; i++) {
        char hex[3];
        sprintf(hex, "%02X", batteryDump[i]);
        json += hex;
        if (i < 15) json += " ";
    }
    json += "\"}";
    
    server.send(200, "application/json", json);
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
    server.on("/api/read", HTTP_GET, handleReadDump);
    server.on("/api/download", HTTP_GET, handleDownloadDump);
    server.on("/api/info", HTTP_GET, handleDumpInfo);
    server.on("/api/write", HTTP_POST, handleWriteDump);
    server.on("/upload", HTTP_POST, handleUploadDump);
    
    server.begin();
    Serial.println("Web server started");
}

#endif
