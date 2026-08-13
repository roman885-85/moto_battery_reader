// Перевірка звукового двигуна: крутимо СПРАВЖНІЙ buzzTask() у модельному часі,
// записуємо кожен його виклик у LEDC і перевіряємо, що звук справді м'який —
// тобто без розривів частоти й гучності. Заразом рендеримо WAV, щоб послухати.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

// ── заглушки Arduino ────────────────────────────────────────────────────────
using std::min;
using std::max;
static unsigned long g_now = 0;
static unsigned long millis() { return g_now; }
static void delay(unsigned long ms) { g_now += ms; }

struct Sample { unsigned long t; uint16_t f; uint32_t duty; };
static std::vector<Sample> g_log;
static uint16_t g_curF = 0;

// Перевіряємо СТАРИЙ (ШІМ) шлях явно, незалежно від того, що зараз обрано в
// settings.h за замовчуванням (з переходу на ЦАП BUZZER_DAC_PIN — заводський
// варіант). settings.h включаємо тут САМІ й одразу перемикаємо на BUZZER_PIN,
// щоб дублювати логіку settings.h не довелось: значення пінів беремо звідти ж.
#define ESP_ARDUINO_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(3,0,0)
#include "settings.h"
#undef BUZZER_DAC_PIN
#define BUZZER_PIN 2
static bool ledcAttachChannel(int, int, int, int) { return true; }
static void ledcWriteTone(int, uint32_t f) { g_curF = (uint16_t)f; }
static void ledcWrite(int, uint32_t duty) { g_log.push_back({g_now, duty ? g_curF : (uint16_t)0, duty}); }
struct FakeSerial { void printf(const char *, ...) {} void println(const char *) {} } Serial;

#include "buzzer.h"

static int fails = 0;

// ── рендер у WAV: п'єзо їсть меандр, гучність = шпаруватість ────────────────
static void writeWav(const char *path, const std::vector<Sample> &log, unsigned long endMs) {
    const int SR = 44100;
    const uint32_t FULL = (1UL << BUZZ_LEDC_BITS) / 2;   // 50 % = максимум
    std::vector<int16_t> pcm;
    pcm.reserve((size_t)(endMs + 60) * SR / 1000);
    double phase = 0.0, lp = 0.0;
    size_t k = 0;
    for (unsigned long ms = 0; ms < endMs + 50; ms++) {
        while (k + 1 < log.size() && log[k + 1].t <= ms) k++;
        double f = (k < log.size()) ? log[k].f : 0.0;
        double amp = (k < log.size() && FULL) ? (double)log[k].duty / FULL : 0.0;
        if (ms >= endMs) { f = 0; amp = 0; }
        for (int i = 0; i < SR / 1000; i++) {
            double s = 0.0;
            if (f > 0.0 && amp > 0.0) {
                phase += f / SR;
                if (phase >= 1.0) phase -= 1.0;
                // меандр зі шпаруватістю amp/2 — саме так звучить п'єзо на LEDC
                s = (phase < amp * 0.5) ? 1.0 : -1.0;
                s *= amp;
            }
            // п'єзо не відтворює гострих фронтів — легке згладжування
            lp += (s - lp) * 0.35;
            int v = (int)(lp * 9000.0);
            pcm.push_back((int16_t)std::max(-32000, std::min(32000, v)));
        }
    }
    uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(SR); u32(SR * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(dataBytes);
    fwrite(pcm.data(), 1, dataBytes, f);
    fclose(f);
}

// ── прогін однієї фрази ─────────────────────────────────────────────────────
static void run(const char *name, const BuzzNote *seq, uint8_t len, const char *wav) {
    g_log.clear(); g_now = 1000; g_curF = 0;
    uint32_t total = buzzPhraseMs(seq, len);

    buzzPlay(seq, len);
    unsigned long t0 = g_now;
    for (int i = 0; i < 4000 && g_bzSeq; i++) { buzzTask(); g_now++; }
    unsigned long dur = g_now - t0;

    printf("\n%s: нот %u, тривалість %lu мс (задано %u), оновлень LEDC %zu\n",
           name, len, dur, total, g_log.size());
    if (g_log.empty()) { printf("  ПОМИЛКА: жодного оновлення\n"); fails++; return; }

    // 1) фраза має починатись і закінчуватись тишею
    uint32_t first = g_log.front().duty, last = g_log.back().duty;
    printf("  перша шпаруватість %u, остання %u\n", first, last);
    if (first > 12) { printf("  ПОМИЛКА: різкий старт (має наростати з нуля)\n"); fails++; }
    if (last != 0)  { printf("  ПОМИЛКА: обрив у кінці (має згасати в нуль)\n"); fails++; }

    // 2) НЕПЕРЕРВНІСТЬ гучності: між сусідніми оновленнями не має бути стрибка
    uint32_t FULL = (1UL << BUZZ_LEDC_BITS) / 2;
    int maxJumpPct = 0; unsigned long jumpAt = 0;
    for (size_t i = 1; i < g_log.size(); i++) {
        int d = (int)g_log[i].duty - (int)g_log[i - 1].duty;
        int pct = abs(d) * 100 / (int)FULL;
        if (pct > maxJumpPct) { maxJumpPct = pct; jumpAt = g_log[i].t - t0; }
    }
    printf("  найбільший стрибок гучності між тіками: %d %% (на %lu мс)\n", maxJumpPct, jumpAt);
    if (maxJumpPct > 12) { printf("  ПОМИЛКА: стрибок гучності — це і чується як клац\n"); fails++; }

    // 3) НЕПЕРЕРВНІСТЬ частоти: портаменто не має давати стрибків більш ніж на
    //    півтона за тік (100 центів). Тиша (f=0) з перевірки виключена.
    double maxCents = 0; unsigned long centsAt = 0;
    for (size_t i = 1; i < g_log.size(); i++) {
        if (!g_log[i].f || !g_log[i - 1].f) continue;
        double c = fabs(1200.0 * log2((double)g_log[i].f / g_log[i - 1].f));
        if (c > maxCents) { maxCents = c; centsAt = g_log[i].t - t0; }
    }
    printf("  найбільший стрибок висоти між тіками: %.0f центів (на %lu мс)\n", maxCents, centsAt);
    if (maxCents > 100.0) { printf("  ПОМИЛКА: стрибок висоти — ноти не перетікають\n"); fails++; }

    // 4) portamento справді відбувається: між першою й останньою нотою частота
    //    має пройти проміжні значення, а не перескочити
    if (len >= 2) {
        uint16_t a = seq[0].f, b = seq[len - 1].f;
        uint16_t lo = std::min(a, b), hi = std::max(a, b);
        int between = 0;
        for (auto &s : g_log) if (s.f > lo + 3 && s.f < hi - 3) between++;
        printf("  проміжних частот між %u і %u Гц: %d\n", a, b, between);
        if (between < 5) { printf("  ПОМИЛКА: портаменто не працює (ноти стрибають)\n"); fails++; }
    }

    // 5) жодного разу не голосніше за стелю
    for (auto &s : g_log)
        if (s.duty > FULL) { printf("  ПОМИЛКА: шпаруватість %u > %u\n", s.duty, FULL); fails++; break; }

    if (wav) { writeWav(wav, g_log, dur); printf("  -> %s\n", wav); }
}

int main() {
    buzzInit();
    const char *dir = "./";   // wav-файли лягають поруч, у теку запуску
    auto path = [&](const char *n) {
        static std::string s; s = std::string(dir) + n; return s.c_str();
    };
    std::string p1 = std::string(dir) + "buzz_start.wav";
    std::string p2 = std::string(dir) + "buzz_ok.wav";
    std::string p3 = std::string(dir) + "buzz_err.wav";
    std::string p4 = std::string(dir) + "buzz_hello.wav";
    std::string p5 = std::string(dir) + "buzz_click.wav";
    (void)path;

    run("СТАРТ операції", BZ_START, sizeof(BZ_START)/sizeof(BZ_START[0]), p1.c_str());
    run("УСПІХ",          BZ_OK,    sizeof(BZ_OK)/sizeof(BZ_OK[0]),       p2.c_str());
    run("ПОМИЛКА",        BZ_ERR,   sizeof(BZ_ERR)/sizeof(BZ_ERR[0]),     p3.c_str());
    run("ПРИВІТ (старт)", BZ_HELLO, sizeof(BZ_HELLO)/sizeof(BZ_HELLO[0]), p4.c_str());
    run("КЛАЦ меню",      BZ_CLICK, sizeof(BZ_CLICK)/sizeof(BZ_CLICK[0]), p5.c_str());

    // 6) повторний запуск посеред фрази не має лишати звук увімкненим
    g_log.clear(); g_now = 5000;
    buzzPlay(BZ_OK, 3);
    for (int i = 0; i < 100; i++) { buzzTask(); g_now++; }
    buzzPlay(BZ_ERR, 2);                       // перебили новою фразою
    for (int i = 0; i < 2000 && g_bzSeq; i++) { buzzTask(); g_now++; }
    printf("\nперебивання фрази: остання шпаруватість %u (має бути 0)\n",
           g_log.empty() ? 999u : g_log.back().duty);
    if (g_log.empty() || g_log.back().duty != 0) {
        printf("  ПОМИЛКА: після перебивання звук лишився\n"); fails++; }

    // 7) buzzTask() без фрази — нічого не робить
    size_t n = g_log.size();
    for (int i = 0; i < 50; i++) { buzzTask(); g_now++; }
    if (g_log.size() != n) { printf("ПОМИЛКА: buzzTask() шумить у спокої\n"); fails++; }

    // ── НАЛАШТУВАННЯ, ЩО МІНЯЮТЬСЯ НА ХОДУ ─────────────────────────────────
    //  Кожна ручка мусить робити рівно те, що обіцяє її підпис у клієнті.
    printf("\n═══ налаштування звуку ═══\n");
    const BuzzCfg DEF = buzzGetCfg();

    // прогін «наосліп»: повертає тривалість, кількість оновлень і крайні частоти
    struct Res { unsigned long dur; size_t upd; uint16_t fmin, fmax; uint32_t vmax; int between; };
    auto play = [&](const BuzzNote *seq, uint8_t len) {
        g_log.clear(); g_now = 20000; g_curF = 0;
        buzzPlay(seq, len);
        unsigned long t0 = g_now;
        for (int i = 0; i < 8000 && g_bzSeq; i++) { buzzTask(); g_now++; }
        Res r{ g_now - t0, g_log.size(), 0xFFFF, 0, 0, 0 };
        uint16_t lo = min(seq[0].f, seq[len - 1].f), hi = max(seq[0].f, seq[len - 1].f);
        for (auto &s : g_log) {
            if (s.f) { if (s.f < r.fmin) r.fmin = s.f; if (s.f > r.fmax) r.fmax = s.f; }
            if (s.duty > r.vmax) r.vmax = s.duty;
            if (s.f > lo + 3 && s.f < hi - 3) r.between++;
        }
        if (r.fmin == 0xFFFF) r.fmin = 0;
        return r;
    };
    auto setCfg = [&](BuzzCfg c) { buzzSetCfg(c); return buzzGetCfg(); };

    // -- 8) темп розтягує фразу пропорційно ---------------------------------
    Res base = play(BZ_OK, 3);
    BuzzCfg c = DEF; c.tempoPct = 200; setCfg(c);
    Res slow = play(BZ_OK, 3);
    c = DEF; c.tempoPct = 50; setCfg(c);
    Res fast = play(BZ_OK, 3);
    printf("темп: 50%% -> %lu мс, 100%% -> %lu мс, 200%% -> %lu мс\n",
           fast.dur, base.dur, slow.dur);
    if (slow.dur < base.dur * 19 / 10 || slow.dur > base.dur * 21 / 10) {
        printf("  ПОМИЛКА: 200%% має дати приблизно подвійну тривалість\n"); fails++; }
    if (fast.dur > base.dur * 6 / 10 || fast.dur < base.dur * 4 / 10) {
        printf("  ПОМИЛКА: 50%% має дати приблизно половину\n"); fails++; }
    // повільніша фраза не сміє розсипатись на окремі ноти
    {
        double mx = 0;
        for (size_t i = 1; i < g_log.size(); i++) { }
        c = DEF; c.tempoPct = 200; setCfg(c); play(BZ_OK, 3);
        for (size_t i = 1; i < g_log.size(); i++) {
            if (!g_log[i].f || !g_log[i - 1].f) continue;
            double ct = fabs(1200.0 * log2((double)g_log[i].f / g_log[i - 1].f));
            if (ct > mx) mx = ct;
        }
        printf("  на 200%% найбільший стрибок висоти %.0f центів\n", mx);
        if (mx > 100.0) { printf("  ПОМИЛКА: повільна фраза розпалась на ноти\n"); fails++; }
    }

    // -- 9) тривалість перетікання ------------------------------------------
    //  Рахуємо на ДВОнотній фразі: у тризвуччі середня нота сама лежить між
    //  крайніми, і лічильник проміжних частот показував би її навіть тоді, коли
    //  ковзання вимкнене.
    setCfg(DEF);
    Res gl100 = play(BZ_START, 2);
    c = DEF; c.glidePct = 0; setCfg(c);
    Res noGl = play(BZ_START, 2);
    c = DEF; c.glidePct = 300; setCfg(c);
    Res bigGl = play(BZ_START, 2);
    printf("перетікання: 0%% -> проміжних частот %d, 100%% -> %d, 300%% -> %d\n",
           noGl.between, gl100.between, bigGl.between);
    if (noGl.between != 0) {
        printf("  ПОМИЛКА: при 0%% ноти мають перемикатись одразу\n"); fails++; }
    if (bigGl.between <= gl100.between) {
        printf("  ПОМИЛКА: довше перетікання має дати більше проміжних частот\n"); fails++; }
    // сама тривалість фрази від перетікання не залежить
    if (noGl.dur != gl100.dur || bigGl.dur != gl100.dur) {
        printf("  ПОМИЛКА: перетікання змінило довжину фрази (%lu/%lu/%lu)\n",
               noGl.dur, gl100.dur, bigGl.dur); fails++; }

    // -- 10) зсув висоти ----------------------------------------------------
    c = DEF; c.semitones = 12; setCfg(c);
    Res up = play(BZ_OK, 3);
    c = DEF; c.semitones = -12; setCfg(c);
    Res dn = play(BZ_OK, 3);
    printf("висота: -12 півтонів -> %u..%u Гц, 0 -> %u..%u, +12 -> %u..%u\n",
           dn.fmin, dn.fmax, base.fmin, base.fmax, up.fmin, up.fmax);
    if (abs((int)up.fmax - (int)base.fmax * 2) > 6) {
        printf("  ПОМИЛКА: +12 півтонів має подвоїти частоту\n"); fails++; }
    if (abs((int)dn.fmax * 2 - (int)base.fmax) > 6) {
        printf("  ПОМИЛКА: -12 півтонів має вдвічі знизити\n"); fails++; }

    // -- 11) гучність -------------------------------------------------------
    c = DEF; c.volume = 255; setCfg(c); Res vHi = play(BZ_OK, 3);
    c = DEF; c.volume = 64;  setCfg(c); Res vLo = play(BZ_OK, 3);
    printf("гучність: 255 -> шпаруватість %u, 64 -> %u\n", vHi.vmax, vLo.vmax);
    if (!(vLo.vmax * 3 < vHi.vmax)) {
        printf("  ПОМИЛКА: гучність майже не змінилась\n"); fails++; }

    // -- 12) вимкнення: цілковита тиша --------------------------------------
    c = DEF; c.enabled = false; setCfg(c);
    Res off = play(BZ_OK, 3);
    printf("вимкнено: оновлень %zu, максимальна шпаруватість %u\n", off.upd, off.vmax);
    if (off.vmax != 0) { printf("  ПОМИЛКА: при вимкненому звуку щось прозвучало\n"); fails++; }
    g_log.clear(); buzzClick(); for (int i = 0; i < 200; i++) { buzzTask(); g_now++; }
    for (auto &s : g_log) if (s.duty) { printf("  ПОМИЛКА: блiп прозвучав при вимкненому звуку\n"); fails++; break; }

    // блiп глушиться окремо, решта сигналів лишається
    c = DEF; c.clickOn = false; setCfg(c);
    g_log.clear(); g_now = 30000; buzzClick();
    for (int i = 0; i < 200; i++) { buzzTask(); g_now++; }
    uint32_t clickMax = 0; for (auto &s : g_log) clickMax = max(clickMax, s.duty);
    Res okStill = play(BZ_OK, 3);
    printf("блiп вимкнено окремо: блiп %u, «успіх» %u\n", clickMax, okStill.vmax);
    if (clickMax) { printf("  ПОМИЛКА: блiп не заглушився\n"); fails++; }
    if (!okStill.vmax) { printf("  ПОМИЛКА: разом із блiпом зникли й інші сигнали\n"); fails++; }

    // -- 13) атака й згасання ------------------------------------------------
    c = DEF; c.attackMs = 0; c.releaseMs = 0; setCfg(c);
    g_log.clear(); g_now = 40000; buzzPlay(BZ_OK, 3);
    for (int i = 0; i < 4000 && g_bzSeq; i++) { buzzTask(); g_now++; }
    uint32_t firstDuty = g_log.empty() ? 0 : g_log.front().duty;
    printf("без огинаючої: перша шпаруватість %u (з огинаючою була 0), остання %u\n",
           firstDuty, g_log.empty() ? 999u : g_log.back().duty);
    if (!firstDuty) { printf("  ПОМИЛКА: атака 0 мс мала дати одразу повну гучність\n"); fails++; }
    if (!g_log.empty() && g_log.back().duty != 0) {
        printf("  ПОМИЛКА: після фрази звук лишився увімкненим\n"); fails++; }

    // -- 14) затиск меж -------------------------------------------------------
    BuzzCfg bad = { true, true, 200, 5, 9000, 9000, 9000, 100 };
    setCfg(bad);
    const BuzzCfg &g = buzzGetCfg();
    printf("затиск: темп %u, перетікання %u, атака %u, згасання %u, півтони %d\n",
           g.tempoPct, g.glidePct, g.attackMs, g.releaseMs, (int)g.semitones);
    if (g.tempoPct != 25 || g.glidePct != 300 || g.attackMs != 200 ||
        g.releaseMs != 400 || g.semitones != 12) {
        printf("  ПОМИЛКА: межі не затиснуто\n"); fails++; }
    bad.tempoPct = 0; bad.semitones = -100; setCfg(bad);
    if (buzzGetCfg().tempoPct != 25 || buzzGetCfg().semitones != -12) {
        printf("  ПОМИЛКА: нижні межі не затиснуто\n"); fails++; }

    // -- 15) прослуховування за ключем ---------------------------------------
    setCfg(DEF);
    printf("сигнали для клієнта: %d\n", BZ_SIGNAL_COUNT);
    for (int i = 0; i < BZ_SIGNAL_COUNT; i++) {
        g_log.clear(); g_now = 50000 + i * 5000;
        uint32_t ms = buzzPlayNamed(BZ_SIGNALS[i].key);
        for (int k = 0; k < 4000 && g_bzSeq; k++) { buzzTask(); g_now++; }
        uint32_t vmax = 0; for (auto &s : g_log) vmax = max(vmax, s.duty);
        printf("  %-6s %-24s %4u мс, гучність %u\n",
               BZ_SIGNALS[i].key, BZ_SIGNALS[i].title, ms, vmax);
        if (!ms || !vmax) { printf("  ПОМИЛКА: сигнал не прозвучав\n"); fails++; }
    }
    if (buzzPlayNamed("немає") || buzzPlayNamed(nullptr)) {
        printf("  ПОМИЛКА: невідомий ключ мав дати 0\n"); fails++; }
    // блiп прослуховується навіть коли він вимкнений у налаштуваннях
    c = DEF; c.clickOn = false; setCfg(c);
    g_log.clear(); g_now = 90000;
    buzzPlayNamed("click");
    for (int i = 0; i < 400 && g_bzSeq; i++) { buzzTask(); g_now++; }
    uint32_t prev = 0; for (auto &s : g_log) prev = max(prev, s.duty);
    printf("  прослуховування блiпа при вимкненому блiпі: гучність %u\n", prev);
    if (!prev) { printf("  ПОМИЛКА: кнопка «прослухати» мовчить\n"); fails++; }

    // -- 16) на слух: рендеримо крайні налаштування --------------------------
    setCfg(DEF);
    struct { const char *file; BuzzCfg cfg; } DEMO[] = {
        { "buzz_slow.wav",  { true, true, BUZZER_VOLUME, 180, 160, 40, 90,  0 } },
        { "buzz_quick.wav", { true, true, BUZZER_VOLUME,  60,  80, 12, 30,  0 } },
        { "buzz_low.wav",   { true, true, BUZZER_VOLUME, 100, 100, 28, 60, -7 } },
        { "buzz_step.wav",  { true, true, BUZZER_VOLUME, 100,   0, 28, 60,  0 } },
    };
    printf("\nдемо-файли під різні налаштування:\n");
    for (auto &d : DEMO) {
        setCfg(d.cfg);
        g_log.clear(); g_now = 100000; g_curF = 0;
        buzzPlay(BZ_OK, 3);
        unsigned long t0 = g_now;
        for (int i = 0; i < 8000 && g_bzSeq; i++) { buzzTask(); g_now++; }
        std::string p = std::string(dir) + d.file;
        writeWav(p.c_str(), g_log, g_now - t0);
        printf("  %-15s темп %3u%%, перетікання %3u%%, півтони %+d -> %lu мс\n",
               d.file, d.cfg.tempoPct, d.cfg.glidePct, (int)d.cfg.semitones, g_now - t0);
    }
    setCfg(DEF);

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
