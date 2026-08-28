#ifndef FAKE_ESP32_STUBS_H
#define FAKE_ESP32_STUBS_H
// ===========================================================================
//  Заглушки ядра ESP32-Arduino для ХОСТОВОЇ ЗБІРКИ драйвера екрана.
// ===========================================================================
//  Тут немає ані краплі поведінки — і не мусить бути. Питання, на яке
//  відповідає ця збірка, рівно одне: «чи компілюється display_color.h?».
//  Доти на нього відповідала лише Arduino IDE — тобто вже після того, як
//  прошивку залили в прилад.
// ===========================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>

using std::min;
using std::max;

#define PROGMEM
#define IRAM_ATTR
#define pgm_read_byte(a)  (*(const uint8_t *)(a))
#define pgm_read_word(a)  (*(const uint16_t *)(a))
#define memcpy_P memcpy
#define strncpy_P strncpy
#define snprintf_P snprintf
#define PSTR(s) (s)
#define F(s) (s)

#define HIGH 1
#define LOW  0
#define INPUT        0
#define OUTPUT       1
#define INPUT_PULLUP 2

static unsigned long g_fakeMillis = 0;
static unsigned long millis() { return g_fakeMillis; }
static unsigned long micros() { return g_fakeMillis * 1000UL; }
static void delay(unsigned long ms) { g_fakeMillis += ms; }
static void delayMicroseconds(unsigned int) {}
static void yield() {}

static void pinMode(int, int) {}
static void digitalWrite(int, int) {}
static int  digitalRead(int) { return HIGH; }
// ⚑ КЕРОВАНИЙ ADC — ЩОБ КНОПКИ МОЖНА БУЛО НАТИСКАТИ З ТЕСТУ. Усі три кнопки
//  приладу висять на одному аналоговому вході (дільник), тож «натиснути
//  кнопку» на хості означає підставити її напругу сюди. Без цього поведінку
//  кнопок можна було перевіряти лише текстом файла — а текст не бачить, ЩО
//  саме станеться, коли на кнопку справді натиснуть.
static int g_fakeAdc = 4095;                 // 4095 = нічого не натиснуто
static int  analogRead(int) { return g_fakeAdc; }
static void analogReadResolution(int) {}
static void analogSetPinAttenuation(int, int) {}
#define ADC_11db 3
static void dacWrite(int, uint8_t) {}
static void ledcAttach(int, uint32_t, uint8_t) {}
static void ledcWrite(int, uint32_t) {}
static void ledcDetach(int) {}
static bool ledcAttachChannel(int, uint32_t, uint8_t, uint8_t) { return true; }
static void ledcFade(int, uint32_t, uint32_t, int) {}
static void analogWrite(int, int) {}
static uint32_t ledcReadFreq(int) { return 0; }

struct hw_timer_t;
static hw_timer_t *timerBegin(uint32_t) { return (hw_timer_t *)1; }
static void timerAttachInterrupt(hw_timer_t *, void (*)()) {}
static void timerAlarm(hw_timer_t *, uint64_t, bool, uint64_t) {}
static void timerEnd(hw_timer_t *) {}

// FreeRTOS — рівно те, чим користується прошивка.
typedef void *TaskHandle_t;
static uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
static TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }
static void vTaskDelay(uint32_t) {}
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(x) (x)

struct FakeEsp {
    uint32_t getFreeHeap()     { return 200000; }
    uint32_t getMaxAllocHeap() { return 100000; }
    uint32_t getMinFreeHeap()  { return 150000; }
    void     restart() {}
};
static FakeEsp ESP;

// String — вузький підмножинний двійник Arduino::String.
class String : public std::string {
public:
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    String(char c) : std::string(1, c) {}
    String(int v)           { char b[24]; snprintf(b, sizeof(b), "%d", v);   assign(b); }
    String(unsigned v)      { char b[24]; snprintf(b, sizeof(b), "%u", v);   assign(b); }
    String(long v)          { char b[24]; snprintf(b, sizeof(b), "%ld", v);  assign(b); }
    String(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v);  assign(b); }
    String(float v, int d = 2)  { char b[32]; snprintf(b, sizeof(b), "%.*f", d, (double)v); assign(b); }
    String(double v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v);         assign(b); }
    const char *c_str() const { return std::string::c_str(); }
    unsigned length() const { return (unsigned)std::string::length(); }
    int indexOf(const char *s) const { auto p = find(s); return p == npos ? -1 : (int)p; }
    int indexOf(char c) const        { auto p = find(c); return p == npos ? -1 : (int)p; }
    String substring(unsigned a) const { return String(std::string::substr(a)); }
    String substring(unsigned a, unsigned b) const { return String(std::string::substr(a, b - a)); }
    bool startsWith(const char *s) const { return rfind(s, 0) == 0; }
    bool endsWith(const char *s) const {
        size_t n = strlen(s); return size() >= n && compare(size() - n, n, s) == 0;
    }
    int   toInt()   const { return atoi(c_str()); }
    float toFloat() const { return (float)atof(c_str()); }
    void  trim() {}
    void  toUpperCase() {}
    void  replace(const char *a, const char *b) {
        size_t p = 0, n = strlen(a);
        while (n && (p = find(a, p)) != npos) { std::string::replace(p, n, b); p += strlen(b); }
    }
    String &operator+=(const char *s)   { append(s ? s : ""); return *this; }
    String &operator+=(const String &s) { append(s);          return *this; }
    String &operator+=(char c)          { push_back(c);       return *this; }
    String &operator+=(int v)           { return *this += String(v); }
    String &operator+=(unsigned v)      { return *this += String(v); }
    String &operator+=(long v)          { return *this += String(v); }
    String &operator+=(unsigned long v) { return *this += String(v); }
};
inline String operator+(const String &a, const String &b) { String r(a); r += b; return r; }
inline String operator+(const String &a, const char *b)   { String r(a); r += b; return r; }
inline String operator+(const char *a, const String &b)   { String r(a); r += b; return r; }
inline String operator+(const String &a, int b)           { String r(a); r += b; return r; }
inline String operator+(const String &a, unsigned long b) { String r(a); r += b; return r; }

struct FakeSerial {
    void begin(unsigned long) {}
    void printf(const char *, ...) {}
    void print(const char *) {}
    void print(int) {}
    void println(const char *) {}
    void println(int) {}
    void println() {}
    int  available() { return 0; }
    int  read() { return -1; }
    void flush() {}
    operator bool() const { return true; }
};
static FakeSerial Serial;

#endif // FAKE_ESP32_STUBS_H
