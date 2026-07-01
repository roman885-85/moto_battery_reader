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

// Глобальная переменная для отслеживания загруженного размера
size_t uploadedSize = 0;

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
        SPIFFS.remove("/dump.bin");  // Удаляем старый файл
        File file = SPIFFS.open("/dump.bin", "w");
        if (file) {
            size_t written = file.write(batteryDump, DUMP_SIZE);
            file.close();
            if (written != DUMP_SIZE) {
                Serial.printf("ERROR: Failed to write dump to SPIFFS (wrote %d of %d bytes)\n", written, DUMP_SIZE);
            }
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

// Обработчик загрузки файла
void handleUploadDump() {
    HTTPUpload &upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Upload started: %s\n", upload.filename.c_str());
        
        // Проверяем свободное место в SPIFFS
        size_t totalBytes = SPIFFS.totalBytes();
        size_t usedBytes = SPIFFS.usedBytes();
        size_t freeBytes = totalBytes - usedBytes;
        
        Serial.printf("SPIFFS: Total=%d, Used=%d, Free=%d\n", totalBytes, usedBytes, freeBytes);
        
        if (freeBytes < DUMP_SIZE + 512) {
            Serial.println("WARNING: Low free space in SPIFFS");
        }
        
        // Удаляем старый файл и закрываем его перед открытием для записи
        SPIFFS.remove("/upload.bin");
        delay(100);  // Даем время на удаление
        uploadedSize = 0;
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Serial.printf("Upload data chunk: %d bytes\n", upload.currentSize);
        
        // Открываем файл в режиме "w" для первого чанка и "a" для остальных
        File file = SPIFFS.open("/upload.bin", uploadedSize == 0 ? "w" : "a");
        
        if (!file) {
            Serial.printf("ERROR: Failed to open /upload.bin for writing (mode: %s)\n", 
                         uploadedSize == 0 ? "w" : "a");
            return;
        }
        
        size_t written = file.write(upload.buf, upload.currentSize);
        file.close();
        
        Serial.printf("Wrote %d bytes (total so far: %d)\n", written, uploadedSize + written);
        
        if (written != upload.currentSize) {
            Serial.printf("ERROR: Write mismatch! Expected %d, wrote %d\n", upload.currentSize, written);
        }
        
        uploadedSize += written;
        
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload finished: %s (%d bytes total uploaded)\n", 
                     upload.filename.c_str(), upload.totalSize);
        Serial.printf("Actual bytes written to SPIFFS: %d\n", uploadedSize);
        
        // Проверяем размер файла в SPIFFS
        File file = SPIFFS.open("/upload.bin", "r");
        if (file) {
            size_t fileSize = file.size();
            Serial.printf("File size verification in SPIFFS: %d bytes\n", fileSize);
            
            // Читаем первые 16 байтов для проверки
            uint8_t header[16];
            size_t headerRead = file.read(header, 16);
            Serial.printf("Header read: %d bytes: ", headerRead);
            for (int i = 0; i < headerRead; i++) {
                Serial.printf("%02X ", header[i]);
            }
            Serial.println();
            
            file.close();
        } else {
            Serial.println("ERROR: Cannot open /upload.bin for verification");
        }
        
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"File uploaded\"}");
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
    
    // Проверяем размер файла перед открытием
    File file = SPIFFS.open("/upload.bin", "r");
    if (!file) {
        Serial.println("ERROR: /upload.bin not found");
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    size_t fileSize = file.size();
    Serial.printf("File size: %d bytes (expected: %d bytes)\n", fileSize, DUMP_SIZE);
    
    if (fileSize != DUMP_SIZE) {
        file.close();
        Serial.printf("ERROR: Invalid file size: %d (expected %d)\n", fileSize, DUMP_SIZE);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    
    // Читаем файл
    uint8_t buffer[DUMP_SIZE];
    memset(buffer, 0, DUMP_SIZE);
    size_t bytesRead = file.read(buffer, DUMP_SIZE);
    file.close();
    
    Serial.printf("Bytes read: %d\n", bytesRead);
    
    // Выводим первые 16 байтов для проверки
    Serial.printf("Data header: ");
    for (int i = 0; i < 16 && i < bytesRead; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();
    
    if (bytesRead != DUMP_SIZE) {
        Serial.printf("ERROR: Failed to read complete file (read %d of %d bytes)\n", bytesRead, DUMP_SIZE);
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to read uploaded file\"}");
        return;
    }
    
    Serial.println("Writing to battery...");
    if (battery.writeBattery(buffer, DUMP_SIZE)) {
        memcpy(batteryDump, buffer, DUMP_SIZE);
        hasDump = true;
        
        // Сохраняем в SPIFFS
        SPIFFS.remove("/dump.bin");
        File dumpFile = SPIFFS.open("/dump.bin", "w");
        if (dumpFile) {
            size_t written = dumpFile.write(buffer, DUMP_SIZE);
            dumpFile.close();
            if (written != DUMP_SIZE) {
                Serial.printf("ERROR: Failed to write dump to SPIFFS (wrote %d of %d bytes)\n", written, DUMP_SIZE);
            }
        }
        
        Serial.println("Write successful!");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Firmware written successfully\"}");
        
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            delay(200);
            digitalWrite(LED_GREEN_PIN, LOW);
            delay(200);
        }
    } else {
        Serial.println("ERROR: Battery write failed!");
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
        Serial.println("SPIFFS mount failed");
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
