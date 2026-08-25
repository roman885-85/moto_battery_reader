// Перевірка ЦАП/DDS-синтезу синуса: крутимо СПРАВЖНІЙ buzzTask() (5 мс тік)
// разом зі СПРАВЖНІМ buzzIsr() (той самий код, що піде на плату), вручну
// «дискретизуючи» модельний час на BUZZ_SAMPLE_HZ відліків/с — так само, як
// апаратний таймер робив би це незалежно від buzzTask(). Рендеримо WAV, щоб
// послухати справжній синус, і перевіряємо, що DDS справді видає задану
// частоту (а не просто «щось на екрані компілюється»).
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

using std::min;
using std::max;
static unsigned long g_now = 0;                 // мс модельного часу
static unsigned long millis() { return g_now; }
static void delay(unsigned long ms) { g_now += ms; }

#define ESP_ARDUINO_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(3,3,11)   // як у власника (лог компілятора)
#define IRAM_ATTR

struct hw_timer_t;
static hw_timer_t *timerBegin(uint32_t) { return (hw_timer_t *)1; }
static void timerAttachInterrupt(hw_timer_t *, void (*)()) {}
static void timerAlarm(hw_timer_t *, uint64_t, bool, uint64_t) {}
static uint8_t g_dac = 128;
static void dacWrite(int, uint8_t v) { g_dac = v; }
struct FakeSerial { void printf(const char *, ...) {} void println(const char *) {} } Serial;

// ⚑ Вмикаємо ЦАП-шлях ЯВНО, а не покладаємось на settings.h.
//  Раніше тест мовчки жив із заводським BUZZER_DAC_PIN у налаштуваннях — і
//  щойно звук у settings.h вимкнули (пін віддали керованому заряду), тест
//  перестав збиратись: buzzer.h пішов «тихою» гілкою, де немає ні buzzIsr(),
//  ні DDS. Але перевіряємо ми САМ СИНТЕЗ, а не те, чи ввімкнено звук у
//  поточній збірці прошивки, — тож пін задаємо тут. Значення 25 — той самий
//  ЦАП-пін, що й був заводським.
#define BUZZER_DAC_PIN 25
#include "buzzer.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

// ── «апаратний таймер»: викликаємо СПРАВЖНІЙ buzzIsr() BUZZ_SAMPLE_HZ раз/с,
//    паралельно з buzzTask() (BUZZ_TICK_MS) — точно як на платі: одне не
//    залежить від іншого, лише від спільних volatile-змінних.
static std::vector<uint8_t> g_pcm;               // весь запис, індекс = відлік

static void stepMs(unsigned long ms) {
    static double acc = 0.0;
    for (unsigned long i = 0; i < ms; i++) {
        buzzTask();
        acc += (double)BUZZ_SAMPLE_HZ / 1000.0;
        while (acc >= 1.0) { buzzIsr(); g_pcm.push_back(g_dac); acc -= 1.0; }
        g_now++;
    }
}

static double measureFreqHz(size_t a, size_t b) {
    if (b <= a + 1) return 0.0;
    int crossings = 0;
    bool prevHigh = g_pcm[a] >= 128;
    for (size_t i = a + 1; i < b; i++) {
        bool high = g_pcm[i] >= 128;
        if (high && !prevHigh) crossings++;
        prevHigh = high;
    }
    double seconds = (double)(b - a) / BUZZ_SAMPLE_HZ;
    return crossings / seconds;
}

static void writeWav8(const char *path, const uint8_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t dataBytes = (uint32_t)n;
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(BUZZ_SAMPLE_HZ); u32(BUZZ_SAMPLE_HZ); u16(1); u16(8);   // 8-біт МОНО — рівно те, що дає dacWrite()
    fwrite("data", 1, 4, f); u32(dataBytes);
    fwrite(pcm, 1, n, f);
    fclose(f);
}

static void run(const char *name, const BuzzNote *seq, uint8_t len, const char *wav) {
    g_now = 1000;
    size_t k0 = g_pcm.size();
    buzzPlay(seq, len);
    unsigned long t0 = g_now;
    unsigned long guard = 0;
    while (g_bzSeq && guard < 4000) { stepMs(1); guard++; }
    stepMs(5);                                     // хвіст: перевірити, що після фрази й далі тиша
    unsigned long dur = g_now - t0;
    size_t k1 = g_pcm.size();

    printf("\n%s: тривалість %lu мс, відліків ЦАП %zu\n", name, dur, k1 - k0);
    if (k1 == k0) { bad("жодного відліку ЦАП"); return; }

    uint8_t first = g_pcm[k0], last = g_pcm[k1 - 1];
    printf("  перший відлік %u, останній %u (тиша = 128, центр шкали)\n", first, last);
    if (abs((int)first - 128) > 4) bad("різкий старт (має бути тиша ~128, атака піднімає плавно)");
    if (abs((int)last  - 128) > 4) bad("обрив у кінці (має згасати до тиші)");

    uint8_t lo = 255, hi = 0;
    for (size_t i = k0; i < k1; i++) { lo = min(lo, g_pcm[i]); hi = max(hi, g_pcm[i]); }
    printf("  розмах ЦАП %u..%u\n", lo, hi);
    if ((int)hi - (int)lo < 40) bad("сигнал надто тихий/плаский — синус ледь помітний");
    if (lo > 250 || hi < 5) { /* лишень інформативно: майже впирається в рейки ЦАП */ }

    if (wav) { writeWav8(wav, &g_pcm[k0], k1 - k0); printf("  -> %s\n", wav); }
}

int main() {
    buzzInit();
    const char *dir = "./";   // wav-файли лягають поруч, у теку запуску
    std::string p1 = std::string(dir) + "buzz_dac_start.wav";
    std::string p2 = std::string(dir) + "buzz_dac_ok.wav";
    std::string p3 = std::string(dir) + "buzz_dac_err.wav";
    std::string p4 = std::string(dir) + "buzz_dac_hello.wav";
    std::string p5 = std::string(dir) + "buzz_dac_click.wav";

    run("СТАРТ операції", BZ_START, sizeof(BZ_START) / sizeof(BZ_START[0]), p1.c_str());
    run("УСПІХ",          BZ_OK,    sizeof(BZ_OK)    / sizeof(BZ_OK[0]),    p2.c_str());
    run("ПОМИЛКА",        BZ_ERR,   sizeof(BZ_ERR)   / sizeof(BZ_ERR[0]),   p3.c_str());
    run("ПРИВІТ (старт)", BZ_HELLO, sizeof(BZ_HELLO) / sizeof(BZ_HELLO[0]), p4.c_str());
    run("КЛАЦ меню",      BZ_CLICK, sizeof(BZ_CLICK) / sizeof(BZ_CLICK[0]), p5.c_str());

    // ── головна перевірка DDS: реально видана частота проти заданої ─────────
    // BZ_START[0] = {659 Гц, 110 мс, glideMs=0} — перша нота фрази, без
    // портаменто (перетікати нема звідки) і поза зоною згасання (вона не
    // остання нота фрази), тож вікно [30..100] мс — чиста стала висота під
    // повною гучністю: саме тут DDS-крок і перевіряємо.
    printf("\n═══ DDS видає задану частоту (а не просто компілюється) ═══\n");
    {
        g_now = 1000;
        size_t k0 = g_pcm.size();
        buzzPlay(BZ_START, 2);
        unsigned long guard = 0;
        while (g_bzSeq && guard < 4000) { stepMs(1); guard++; }
        size_t a = k0 + (size_t)(30.0  * BUZZ_SAMPLE_HZ / 1000.0);
        size_t b = k0 + (size_t)(100.0 * BUZZ_SAMPLE_HZ / 1000.0);
        double f = measureFreqHz(a, b);
        printf("  очікую ~659 Гц (± вібрато ~0.6%%), виміряно %.1f Гц\n", f);
        if (fabs(f - 659.0) > 659.0 * 0.03) bad("DDS видає не ту частоту — перевірити крок фазового акумулятора");
    }

    // ── зсув висоти (semitones) справді подвоює/зменшує вдвічі частоту ──────
    {
        BuzzCfg c = buzzGetCfg();
        c.semitones = 12; buzzSetCfg(c);
        g_now = 1000; size_t k0 = g_pcm.size();
        buzzPlay(BZ_START, 2);
        unsigned long guard = 0; while (g_bzSeq && guard < 4000) { stepMs(1); guard++; }
        size_t a = k0 + (size_t)(30.0 * BUZZ_SAMPLE_HZ / 1000.0);
        size_t b = k0 + (size_t)(100.0 * BUZZ_SAMPLE_HZ / 1000.0);
        double f = measureFreqHz(a, b);
        printf("  +12 півтонів: очікую ~1318 Гц, виміряно %.1f Гц\n", f);
        if (fabs(f - 1318.0) > 1318.0 * 0.04) bad("+12 півтонів має вдвічі підняти частоту ЦАП-синуса");
        c.semitones = 0; buzzSetCfg(c);
    }

    // ── вимкнений звук -> плаский сигнал (тиша), жодного «протікання» на ЦАП ─
    {
        BuzzCfg c = buzzGetCfg();
        c.enabled = false; buzzSetCfg(c);
        g_now = 1000; size_t k0 = g_pcm.size();
        buzzPlay(BZ_OK, 3);
        for (int i = 0; i < 200; i++) stepMs(1);
        size_t k1 = g_pcm.size();
        uint8_t lo = 255, hi = 0;
        for (size_t i = k0; i < k1; i++) { lo = min(lo, g_pcm[i]); hi = max(hi, g_pcm[i]); }
        printf("\nвимкнено: розмах ЦАП %u..%u (має лишатись 128)\n", lo, hi);
        if (lo != 128 || hi != 128) bad("при вимкненому звуку ЦАП не тримає тишу");
        c.enabled = true; buzzSetCfg(c);
    }

    // ── гучність справді змінює розмах сигналу на ЦАП ────────────────────────
    {
        BuzzCfg c = buzzGetCfg();
        c.volume = 255; buzzSetCfg(c);
        g_now = 1000; size_t k0 = g_pcm.size();
        buzzPlay(BZ_OK, 3);
        unsigned long guard = 0; while (g_bzSeq && guard < 4000) { stepMs(1); guard++; }
        size_t k1 = g_pcm.size();
        uint8_t loHi = 255, hiHi = 0;
        for (size_t i = k0; i < k1; i++) { loHi = min(loHi, g_pcm[i]); hiHi = max(hiHi, g_pcm[i]); }

        c.volume = 40; buzzSetCfg(c);
        g_now = 1000; k0 = g_pcm.size();
        buzzPlay(BZ_OK, 3);
        guard = 0; while (g_bzSeq && guard < 4000) { stepMs(1); guard++; }
        k1 = g_pcm.size();
        uint8_t loLo = 255, hiLo = 0;
        for (size_t i = k0; i < k1; i++) { loLo = min(loLo, g_pcm[i]); hiLo = max(hiLo, g_pcm[i]); }

        printf("\nгучність 255: розмах %u..%u (%u); гучність 40: розмах %u..%u (%u)\n",
               loHi, hiHi, hiHi - loHi, loLo, hiLo, hiLo - loLo);
        if (!((hiLo - loLo) * 3 < (hiHi - loHi))) bad("гучність майже не впливає на амплітуду ЦАП");
        c.volume = 130; buzzSetCfg(c);
    }

    // ── ЦАП завжди 0..255 (uint8_t гарантує апаратно, перевіряємо логічно) ──
    printf("\nусі %zu відліків ЦАП за весь прогін — у межах 0..255 (гарантія типу uint8_t)\n", g_pcm.size());

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
