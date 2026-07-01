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
        SPIFFS.remove("/upload.bin");
        uploadedSize = 0;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File file = SPIFFS.open("/upload.bin", "a");
        if (file) {
            size_t written = file.write(upload.buf, upload.currentSize);
            uploadedSize += written;
            file.close();
            
            if (written != upload.currentSize) {
                Serial.printf("WARNING: Failed to write all upload data (wrote %d of %d bytes)\n", written, upload.currentSize);
                Serial.printf("Total uploaded so far: %d bytes\n", uploadedSize);
            }
        } else {
            Serial.println("ERROR: Failed to open /upload.bin for writing");
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload finished: %s (%d bytes)\n", upload.filename.c_str(), upload.totalSize);
        Serial.printf("Actual size in SPIFFS: %d bytes\n", uploadedSize);
        
        // Проверяем размер файла в SPIFFS
        File file = SPIFFS.open("/upload.bin", "r");
        if (file) {
            size_t fileSize = file.size();
            file.close();
            Serial.printf("File size verification: %d bytes\n", fileSize);
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
    
    Serial.println("Write request received, checking uploaded file...");
    
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
    size_t bytesRead = file.read(buffer, DUMP_SIZE);
    file.close();
    
    Serial.printf("Bytes read: %d\n", bytesRead);
    
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
