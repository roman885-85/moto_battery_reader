// ===========================================================================
//  ТЕКСТ НА ЕКРАНІ МУСИТЬ ВМІЩАТИСЯ В ТЕ, У ЧОМУ ЙОГО МАЛЮЮТЬ
//
//  Скарга власника: «в банері помилки живлення частина тексту вилазить за межі
//  банера». Причина була арифметична й перевірна наперед: рядок
//  «блок живлення просів або не той» — 31 гліф, шрифтом 9x15 це 279 px, а
//  панель — 240 px. Тобто напис не вміщався навіть у екран, не те що в плашку.
//
//  Такі речі не ловляться оглядом коду: у UTF-8 кирилична літера займає два
//  байти, тож strlen() тут бреше рівно вдвічі, і «на око» ніхто не порахує.
//  Тому — тест.
//
//  display_color.h на хості не збирається (Adafruit GFX, U8g2), тож перевіряємо
//  дві речі окремо:
//    * САМ механізм переносу (textwrap.h) — чиста арифметика, повністю тут;
//    * НАПИСИ банера — на найвужчій із підтримуваних панелей.
//  А те, що display_color.h справді кличе цей механізм, стереже
//  session_guard_check.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "textwrap.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

// Чи з'їв перенос УВЕСЬ рядок, нічого не загубивши. Саме це головне питання:
// обрізаний текст — така сама втрата, як і текст за межами плашки.
static bool wrapKeepsAll(const char *s, int maxG, int maxLines) {
    TxtLine ln[8];
    if (maxLines > 8) maxLines = 8;
    int n = txtWrap(s, maxG, ln, maxLines);
    if (n <= 0) return false;
    int got = 0;
    for (int i = 0; i < n; i++) {
        if (ln[i].glyphs > maxG) return false;      // рядок ширший за дозволене
        got += ln[i].glyphs;
    }
    // Пробіли на розривах з'їдаються, тож сума гліфів може бути меншою рівно
    // на кількість розривів.
    int total = txtGlyphs(s);
    return got >= total - (n - 1) && got <= total;
}

// Панелі, які проєкт підтримує, і ширина комірки шрифту пояснень на кожній
// (див. FONT_BODY / FONT_BODY_W у display_color.h).
struct Panel { const char *name; int w; int bodyW; int modelW; };
static const Panel PANELS[] = {
    { "135x240", 135, 6,  8 },
    { "170x320", 170, 6,  8 },
    { "172x320", 172, 6,  8 },
    { "240x240", 240, 9, 10 },
    { "240x280", 240, 9, 10 },
    { "240x320", 240, 9, 10 },
};
#define PANEL_COUNT ((int)(sizeof(PANELS) / sizeof(PANELS[0])))

// Ті самі числа, що й у display_color.h: EDGE=6 (без скруглення), PSU_PAD=4.
// Скруглені кути дають EDGE=22 — плашка вужча, тож перевіряємо і цей випадок.
static int innerW(int tftW, int edge) { return (tftW - 2 * (edge - 4)) - 2 * 4; }

// Написи банера — дослівно з display_color.h.
static const char *HEADLINES[] = {
    "НЕМАЄ ЖИВЛЕННЯ", "НАПРУГА ЗАНИЖЕНА", "НАПРУГА ЗАВИЩЕНА",
};
static const char *SUBLINES[] = {
    "блок не під'єднано", "блок просів або не той", "не той блок (19 В?)",
};
// Рядки з числами — у найдовшому вигляді, який може дати snprintf.
static const char *NUMLINES[] = {
    "є 19.99 В", "треба 14 В", "треба 14.0 В", "(12.6…15.6)",
};
#define NUMLINE_COUNT ((int)(sizeof(NUMLINES) / sizeof(NUMLINES[0])))

int main() {
    printf("1) лічильник гліфів UTF-8 — не байтів\n");
    {
        check(txtGlyphs("abc") == 3, "латиниця рахується як є");
        check(txtGlyphs("блок") == 4, "кирилиця: 4 гліфи, а не 8 байтів");
        check(strlen("блок") == 8, "…і саме тому strlen() тут не годиться");
        check(txtGlyphs("не той блок (19 В?)") == 19, "змішаний рядок із дужками");
        check(txtGlyphs("") == 0, "порожній рядок");
        check(txtGlyphs(nullptr) == 0, "nullptr не валить");
        check(txtGlyphs("…") == 1, "трикрапка — один гліф (3 байти)");
    }

    printf("\n2) зсув гліфа в байтах\n");
    {
        check(txtByteAt("блок", 0) == 0, "нульовий гліф на нульовому байті");
        check(txtByteAt("блок", 1) == 2, "другий гліф — на другому байті");
        check(txtByteAt("блок", 4) == 8, "за кінцем — довжина рядка");
        check(txtByteAt("a блок", 2) == 2, "після пробілу зсув правильний ('б' — третій гліф, другий байт)");
    }

    printf("\n3) перенос по словах\n");
    {
        TxtLine ln[4];
        int n = txtWrap("блок просів або не той", 12, ln, 4);
        printf("   «блок просів або не той» по 12 гліфів -> %d рядки\n", n);
        check(n >= 2, "довгий рядок справді розбито");
        for (int i = 0; i < n; i++)
            if (ln[i].glyphs > 12) bad("рядок ширший за дозволене");
        check(wrapKeepsAll("блок просів або не той", 12, 4), "нічого не загублено");

        // Слово, довше за рядок, мусить бути порізане, а не вилізти.
        n = txtWrap("абвгдеєжзиійклмнопр", 5, ln, 4);
        printf("   слово з 19 гліфів по 5 -> %d рядки\n", n);
        for (int i = 0; i < n; i++)
            if (ln[i].glyphs > 5) bad("довге слово вилізло за межу");
        check(n == 4, "розбито рівно на стільки рядків, скільки дозволено");

        // Рядок, що вміщається, не чіпаємо.
        n = txtWrap("коротко", 20, ln, 2);
        check(n == 1 && ln[0].glyphs == 7, "короткий рядок лишається одним");

        check(txtFits("блок не під'єднано", 20), "txtFits: вміщається");
        check(!txtFits("блок не під'єднано", 10), "txtFits: не вміщається");
    }

    printf("\n4) написи банера НЕ вилазять за плашку на ЖОДНІЙ панелі\n");
    {
        int worstH = 0, worstS = 0;
        for (int p = 0; p < PANEL_COUNT; p++) {
            for (int round = 0; round < 2; round++) {
                int edge = round ? 22 : 6;              // зі скругленням і без
                int inner = innerW(PANELS[p].w, edge);
                int gH = inner / PANELS[p].modelW;
                int gB = inner / PANELS[p].bodyW;
                if (gH < 1 || gB < 1) continue;
                // Резерв рядків рахується ТАК САМО, як у display_color.h:
                // за тим, скільки гліфів лишається після відступів, а не за
                // шириною панелі (див. PSU_HEAD_LINES / PSU_SUB_LINES).
                const int HEAD_MAX_G = 16, SUB_MAX_G = 22;
                int headLines = (gH >= HEAD_MAX_G) ? 1 : 2;
                int subLines  = (gB >= SUB_MAX_G)  ? 1 : 2;
                for (int i = 0; i < 3; i++) {
                    if (!wrapKeepsAll(HEADLINES[i], gH, headLines)) {
                        printf("   %s edge %d: заголовок «%s» не влазить у %d рядк. по %d гліфів\n",
                               PANELS[p].name, edge, HEADLINES[i], headLines, gH);
                        worstH++;
                    }
                    if (!wrapKeepsAll(SUBLINES[i], gB, subLines)) {
                        printf("   %s edge %d: пояснення «%s» не влазить у %d рядк. по %d гліфів\n",
                               PANELS[p].name, edge, SUBLINES[i], subLines, gB);
                        worstS++;
                    }
                }
                for (int i = 0; i < NUMLINE_COUNT; i++)
                    if (!wrapKeepsAll(NUMLINES[i], gB, 1)) {
                        printf("   %s edge %d: рядок із числами «%s» не влазить у рядок (%d гліфів)\n",
                               PANELS[p].name, edge, NUMLINES[i], gB);
                        worstS++;
                    }
            }
        }
        check(worstH == 0, "заголовки вміщаються у відведені їм рядки скрізь");
        check(worstS == 0, "пояснення й числа вміщаються скрізь");
        // Константи резерву мусять відповідати РЕАЛЬНИМ написам: якщо текст
        // подовжити й не поправити число, розмітка мовчки поїде.
        int maxH = 0, maxS = 0;
        for (int i = 0; i < 3; i++) {
            if (txtGlyphs(HEADLINES[i]) > maxH) maxH = txtGlyphs(HEADLINES[i]);
            if (txtGlyphs(SUBLINES[i])  > maxS) maxS = txtGlyphs(SUBLINES[i]);
        }
        printf("   найдовші написи: заголовок %d гліфів, пояснення %d\n", maxH, maxS);
        check(maxH == 16, "PSU_HEAD_MAX_GLYPHS збігається з найдовшим заголовком");
        check(maxS == 22, "PSU_SUB_MAX_GLYPHS збігається з найдовшим поясненням");
    }

    printf("\n5) на штатній панелі 240 пояснення вміщаються В ОДИН рядок\n");
    {
        // Перенос — запобіжник, а не спосіб верстки: два рядки там, де досить
        // одного, читаються гірше. На активній панелі має вистачати рядка.
        int inner = innerW(240, 6);
        int gB = inner / 9;
        printf("   плашка 240 px: %d px усередині, %d гліфів шрифтом 9x15\n", inner, gB);
        int over = 0;
        for (int i = 0; i < 3; i++)
            if (!txtFits(SUBLINES[i], gB)) {
                printf("   «%s» — %d гліфів, а влазить %d\n",
                       SUBLINES[i], txtGlyphs(SUBLINES[i]), gB);
                over++;
            }
        check(over == 0, "усі три пояснення — в один рядок");
        for (int i = 0; i < 3; i++)
            if (!txtFits(HEADLINES[i], inner / 10)) over++;
        check(over == 0, "і всі три заголовки теж");

        // ⚑ І ПЕРЕВІРКА «ВІД ПРОТИЛЕЖНОГО» ПРЯМО ТУТ: старий напис, на який
        //  скаржився власник, мусить цю перевірку НЕ проходити. Інакше тест
        //  зелений з будь-яким текстом і нічого не стереже.
        check(!txtFits("блок живлення просів або не той", gB),
              "старий напис зі скарги цю перевірку НЕ проходить");
        check(txtGlyphs("блок живлення просів або не той") * 9 > 240,
              "…і він справді ширший за саму панель, а не лише за плашку");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
