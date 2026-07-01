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
        File file = SPIFFS.open("/dump.bin", "w");
        if (file) {
            file.write(batteryDump, DUMP_SIZE);
            file.close();
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
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File file = SPIFFS.open("/upload.bin", "a");
        if (file) {
            file.write(upload.buf, upload.currentSize);
            file.close();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload finished: %s (%d bytes)\n", upload.filename.c_str(), upload.totalSize);
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
    
    File file = SPIFFS.open("/upload.bin", "r");
    if (!file) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No file uploaded\"}");
        return;
    }
    
    uint8_t buffer[DUMP_SIZE];
    size_t size = file.read(buffer, DUMP_SIZE);
    file.close();
    
    if (size != DUMP_SIZE) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid file size\"}");
        return;
    }
    
    if (battery.writeBattery(buffer, DUMP_SIZE)) {
        memcpy(batteryDump, buffer, DUMP_SIZE);
        hasDump = true;
        
        File dumpFile = SPIFFS.open("/dump.bin", "w");
        if (dumpFile) {
            dumpFile.write(buffer, DUMP_SIZE);
            dumpFile.close();
        }
        
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Firmware written successfully\"}");
        
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            delay(200);
            digitalWrite(LED_GREEN_PIN, LOW);
            delay(200);
        }
    } else {
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write battery\"}");
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