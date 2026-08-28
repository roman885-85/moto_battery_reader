// ===========================================================================
//  МЕНЮ ПРИСТРОЮ: ПОРЯДОК, ГРУПИ Й ТЕ, ЩО НАЗВИ ВЛАЗЯТЬ У ЕКРАН
//
//  Скарга власника: «переділай меню дисплея в логічнішому розташуванні й
//  сортуванні пунктів, зроби перемикання функцій зручнішим — можливо, списком».
//
//  Було: усі дії лежали ОДНИМ ПЛОСКИМ КІЛЬЦЕМ, яке гортається лише вперед. До
//  останнього пункту — стільки натискань, скільки в пристрої операцій (а їх
//  32 разом із шаблонами), назад — ніяк, і поруч, в одному кільці, щоденний
//  «Заряд» і незворотнє «СТЕРТИ 2433».
//
//  Стало: СПИСОК із групами (operations.h), стрибок по групах довгим
//  натисканням, курсор ходить в обидва боки.
//
//  ⚑ ЧОМУ ЦЕ ТРЕБА ПЕРЕВІРЯТИ ТЕСТОМ, А НЕ ОКОМ. Порядок показу тепер описано
//  ОКРЕМОЮ таблицею (коди OP_* міняти не можна — вони в API), тобто з'явилось
//  рівно те місце, де операцію легко ЗАБУТИ: додав OP_*, а в меню не вписав —
//  і на пристрої її просто немає. Ще й назви частини пунктів збираються на
//  льоту («Ціль заряду 100%»), а кирилиця в UTF-8 — два байти на літеру, тож
//  ні довжина буфера, ні ширина в пікселях оком не рахуються.
//
//  display.h/display_color.h на хості не збираються (u8g2, Adafruit GFX), тому
//  перевіряємо те, що від них НЕ залежить: модель меню (operations.h) і
//  арифметику ширини (textwrap.h). А те, що екран справді малює саме її,
//  стереже session_guard_check.
// ===========================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

#include "radio_mode.h"   // RADIO_* потрібні заглушці radioMode() нижче

// Цілі, профілі й ручні уставки живуть у charge.h/discharge.h, які тут не
// потрібні: operations.h оголошує їх наперед, а нам досить сталих значень.
// Заряд буває вимкнений — це властивість екземпляра пристрою (див. charge.h).
// Меню від нього залежить напряму, тож тут прапорець і живе, і перемикається.
static bool g_haveCharge = true;
inline bool     chargeAvailable()    { return g_haveCharge; }
inline uint16_t dischargeTargetMv()  { return 7200; }
inline uint8_t  chargeTargetPct()    { return 100; }
inline uint8_t  chargeProfile()      { return 0; }
inline uint16_t chargeManualMa()     { return 0; }
inline uint16_t chargeManualMv()     { return 0; }
inline uint8_t  dischargeProfile()   { return 0; }
inline uint16_t dischargeManualMa()  { return 0; }

#include "operations.h"
#include "textwrap.h"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { if (c) printf("   ок    %s\n", m); else bad(m); }

// Панелі й ширина комірки шрифту списку. Кольорові — FONT_BODY_W із
// display_color.h, монохромні — MENU_GLYPH_W із display.h. Числа продубльовані
// свідомо: самі заголовки на хості не збираються.
struct Panel { const char *name; int budget; bool color; };
static int colorBudget(int w, int edge, int cell) {
    int inner = w - 2 * edge - 4;          // 4 — під смужку прокрутки
    return (inner - 3 - 14) / cell;        // 3 — смужка небезпеки, 14 — відступ
}
static int monoBudget(int w, int cell) { return w / cell - 2; }   // 2 — «! »

int main() {
    printf("=== МЕНЮ ПРИСТРОЮ ===\n");

    const int total = menuCount();
    const int ops   = opCount();

    printf("\n1) склад списку\n");
    printf("   шаблонів %d, операцій %d, пунктів меню %d\n",
           BATTERY_TEMPLATE_COUNT, ops, total);
    check(total > ops, "у списку є не лише операції (ще й переходи на сторінки)");
    {
        uint8_t k, g; int c;
        check(!menuRow(-1, &k, &c, &g), "від'ємний індекс відхиляється");
        check(!menuRow(total, &k, &c, &g), "індекс за кінцем списку відхиляється");
        check(menuRow(total - 1, &k, &c, &g), "останній пункт існує");
    }

    printf("\n2) ЖОДНА операція не загубилась і не подвоїлась\n");
    {
        int *seen = (int *)calloc(ops, sizeof(int));
        int pages = 0;
        for (int i = 0; i < total; i++) {
            uint8_t k, g; int c;
            if (!menuRow(i, &k, &c, &g)) { bad("рядок не розібрався"); break; }
            if (k == MI_OP) {
                if (c < 0 || c >= ops) { bad("код операції поза межами каталогу"); break; }
                seen[c]++;
            } else {
                if (c < 0 || c >= MPG_COUNT) { bad("код сторінки поза межами"); break; }
                pages++;
            }
        }
        int missing = 0, dup = 0;
        for (int o = 0; o < ops; o++) { if (!seen[o]) missing++; if (seen[o] > 1) dup++; }
        free(seen);
        printf("   переходів на сторінки: %d\n", pages);
        check(missing == 0, "кожна операція каталогу є в меню");
        check(dup == 0,     "жодна операція не трапляється двічі");
        check(pages == MPG_COUNT, "усі службові сторінки досяжні зі списку");
    }
    check(menuIndexOfOp(OP_CELLSWAP) >= 0 && menuIndexOfOp(OP_CHARGE_WAKE) >= 0,
          "зворотний пошук по коду операції працює");

    printf("\n3) групи йдуть суцільно й у заданому порядку\n");
    {
        int prevGroup = -1, starts = 0;
        bool order = true, contiguous = true;
        bool used[MG_COUNT] = { false };
        for (int i = 0; i < total; i++) {
            uint8_t k, g; int c; menuRow(i, &k, &c, &g);
            if ((int)g != prevGroup) {
                if (used[g]) contiguous = false;         // група вже була й повернулась
                if ((int)g < prevGroup) order = false;
                used[g] = true; prevGroup = g; starts++;
                if (!menuGroupStarts(i)) { bad("початок групи не позначений"); break; }
            } else if (menuGroupStarts(i)) {
                bad("позначено початок групи там, де група не змінилась"); break;
            }
        }
        printf("   груп у списку: %d\n", starts);
        check(contiguous, "жодна група не розривається іншою");
        check(order,      "групи йдуть у порядку оголошення (ремонт -> ... -> небезпечне)");
        check(starts >= 7, "груп достатньо, щоб стрибок по них мав сенс");
    }

    printf("\n4) перше — вихід, останнє — незворотнє\n");
    {
        uint8_t k, g; int c;
        menuRow(0, &k, &c, &g);
        check(k == MI_PAGE && c == MPG_HOME,
              "перший пункт — вихід до показань (єдиний спосіб вийти з меню без окремої кнопки)");
        char buf[OP_NAME_BUF]; const char *n, *l1, *l2; uint8_t d, ch;
        menuRow(total - 1, &k, &c, &g);
        check(g == MG_DANGER, "остання група — «НЕБЕЗПЕЧНО»");
        // У найнебезпечнішій групі не сміє бути нічого, крім стирань.
        bool onlyWipe = true;
        for (int i = 0; i < total; i++) {
            uint8_t k2, g2; int c2; menuRow(i, &k2, &c2, &g2);
            if (g2 != MG_DANGER) continue;
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            if (d != OPD_WIPE) onlyWipe = false;
        }
        check(onlyWipe, "у групі «НЕБЕЗПЕЧНО» лише незворотні операції");
        // ...а в щоденних групах не сміє бути незворотного.
        bool dailyClean = true;
        for (int i = 0; i < total; i++) {
            uint8_t k2, g2; int c2; menuRow(i, &k2, &c2, &g2);
            if (g2 != MG_REPAIR && g2 != MG_CHARGE && g2 != MG_DISCHARGE &&
                g2 != MG_COUNTERS) continue;
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            if (d == OPD_WIPE) dailyClean = false;
        }
        check(dailyClean, "у щоденних групах немає незворотного (воно винесене окремо)");
    }

    printf("\n5) перемикач цілі стоїть ОДРАЗУ ПІСЛЯ своєї дії\n");
    check(menuIndexOfOp(OP_CHARGE_TGT) == menuIndexOfOp(OP_CHARGE_DCDC) + 1,
          "«Ціль заряду» — наступний пункт після «Заряд ШІМ»");
    check(menuIndexOfOp(OP_DISCHARGE_TGT) == menuIndexOfOp(OP_DISCHARGE) + 1,
          "«Ціль розряду» — наступний пункт після «Розряд»");

    printf("\n5а) заряд і розряд — ОДНАКОВОЇ будови\n");
    {
        // Скарга власника: «зроби для заряду, розряду й оживлення однотипний
        // вигляд». На рівні меню це означає: обидві силові машини — свої
        // групи однакової форми «дія -> ціль -> ручні уставки -> профіль», а
        // не купа впереміш в одній групі «ЖИВЛЕННЯ», як було.
        int c0 = menuIndexOfOp(OP_CHARGE_DCDC), d0 = menuIndexOfOp(OP_DISCHARGE);
        check(menuGroupStarts(c0), "група ЗАРЯД починається саме з дії заряду");
        check(menuGroupStarts(d0), "група РОЗРЯД — з дії розряду");
        uint8_t k, g; int c;
        menuRow(c0, &k, &c, &g); check(g == MG_CHARGE,    "…і це справді група ЗАРЯД");
        menuRow(d0, &k, &c, &g); check(g == MG_DISCHARGE, "…а та — РОЗРЯД");
        // Профіль — останній «налаштувальний» пункт у кожній групі.
        check(menuIndexOfOp(OP_CHARGE_PROFILE) > menuIndexOfOp(OP_CHARGE_MANUAL_MA),
              "у заряді профіль іде після ручних уставок");
        check(menuIndexOfOp(OP_DIS_PROFILE) > menuIndexOfOp(OP_DIS_MANUAL_MA),
              "у розряді так само");
        // Пробудження — у групі ЗАРЯДУ: це особливий спосіб його почати.
        menuRow(menuIndexOfOp(OP_CHARGE_WAKE), &k, &c, &g);
        check(g == MG_CHARGE, "пробудження стоїть у групі заряду, а не окремо");
        // Усі нові пункти — безпечні: вони нічого не пишуть у чипи, тож
        // вмикаються коротким натисканням, без утримання.
        char buf[OP_NAME_BUF]; const char *n, *l1, *l2; uint8_t d, ch;
        bool allSafe = true;
        for (int op : { OP_CHARGE_PROFILE, OP_CHARGE_MANUAL_MA, OP_CHARGE_MANUAL_MV,
                        OP_DIS_PROFILE, OP_DIS_MANUAL_MA }) {
            int i = menuIndexOfOp(op);
            if (i < 0) { allSafe = false; break; }
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            if (d != OPD_SAFE || ch != OPC_NONE) allSafe = false;
        }
        check(allSafe, "уставки й профілі позначені як безпечні (нічого не пишуть у чипи)");
    }

    printf("\n6) стрибок по групах обходить УСІ групи й замикається в коло\n");
    {
        int i = 0, steps = 0, guard = 0;
        int firstStarts = 0;
        for (int k = 0; k < total; k++) if (menuGroupStarts(k)) firstStarts++;
        do {
            i = menuNextGroup(i);
            steps++;
            if (++guard > 100) break;
        } while (i != 0);
        check(steps == firstStarts, "«наступна група» обходить усі групи рівно за один оберт");
        // Назад із середини групи спершу повертає на її початок — так поводяться
        // всі списки з такою навігацією, і це рятує від «промахнувся».
        int mid = menuIndexOfOp(OP_DISCHARGE_TGT);  // не перший у своїй групі
        check(mid > 0 && !menuGroupStarts(mid), "для перевірки взято пункт усередині групи");
        check(menuGroupStarts(menuPrevGroup(mid)), "«попередня група» з середини стає на початок групи");
        check(menuPrevGroup(menuGroupFirst(mid)) < menuGroupFirst(mid),
              "…а з початку групи — на попередню");
    }

    printf("\n7) скільки натискань до найдальшого пункту\n");
    {
        // Було: плоске кільце лише вперед -> у найгіршому разі стільки
        // натискань, скільки операцій мінус одна.
        int oldWorst = ops - 1;
        // Стало: до потрібної групи стрибками, далі всередині неї.
        int groups = 0, longest = 0, cur = 0;
        for (int i = 0; i < total; i++) {
            if (menuGroupStarts(i)) { groups++; if (cur > longest) longest = cur; cur = 0; }
            cur++;
        }
        if (cur > longest) longest = cur;
        int newWorst = groups / 2 + longest;         // групи по колу в обидва боки
        printf("   було %d натискань, стало не більше %d (груп %d, найдовша %d)\n",
               oldWorst, newWorst, groups, longest);
        check(newWorst < oldWorst, "найдальший пункт став ближчим, а не лише інакше розкладеним");
    }

    printf("\n8) назви влазять у буфер (кирилиця — 2 байти на літеру)\n");
    {
        int worstBytes = 0; const char *worstName = "";
        for (int i = 0; i < total; i++) {
            char buf[256]; const char *n, *l1, *l2; uint8_t d, ch;
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            int b = (int)strlen(n);
            if (b > worstBytes) { worstBytes = b; worstName = n; }
        }
        printf("   найдовша назва: «%s» — %d байт, буфер %d\n",
               worstName, worstBytes, (int)OP_NAME_BUF);
        check(worstBytes < OP_NAME_BUF, "найдовша назва вміщається в OP_NAME_BUF без обрізання");
        // Контроль: із колишнім буфером 26 байт вона б НЕ вмістилась — саме так
        // «Ціль заряду 100%» і показувалась без відсотка.
        check(worstBytes >= 26, "…і саме тому старий буфер 26 байт її різав");
    }

    printf("\n9) назви влазять у рядок списку на КОЖНІЙ панелі\n");
    {
        const Panel PANELS[] = {
            { "кольор. 135",  colorBudget(135, 6, 6), true  },
            { "кольор. 170",  colorBudget(170, 6, 6), true  },
            { "кольор. 172",  colorBudget(172, 6, 6), true  },
            { "кольор. 240",  colorBudget(240, 6, 9), true  },
            { "моно 128x128", monoBudget(128, 6),     false },
            { "моно 128x64",  monoBudget(128, 5),     false },
            { "моно 84x48",   monoBudget(84, 5),      false },
        };
        const int PN = (int)(sizeof(PANELS) / sizeof(PANELS[0]));
        for (int p = 0; p < PN; p++) {
            int over = 0, cut = 0;
            for (int i = 0; i < total; i++) {
                char buf[256], fit[256]; const char *n, *l1, *l2; uint8_t d, ch;
                menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
                txtFit(fit, sizeof(fit), n, PANELS[p].budget);
                if (txtGlyphs(fit) > PANELS[p].budget) over++;
                if (txtGlyphs(n) > PANELS[p].budget) cut++;
            }
            printf("   %-12s місця на %2d гліфів: обрізано %d назв, вилізло %d\n",
                   PANELS[p].name, PANELS[p].budget, cut, over);
            if (over) bad("назва вилізла за рядок навіть після обрізання");
        }
        // На ШТАТНІЙ панелі власника обрізати не мусить нічого: обрізана назва
        // читається як інша операція, а ціна помилки тут — чужа ідентичність.
        int cut240 = 0;
        for (int i = 0; i < total; i++) {
            char buf[256]; const char *n, *l1, *l2; uint8_t d, ch;
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            if (txtGlyphs(n) > colorBudget(240, 6, 9)) cut240++;
        }
        check(cut240 == 0, "на панелі 240 жодна назва не обрізається");
        // Найвужча кольорова панель — теж без обрізання: 135 px це серійний
        // варіант, а не екзотика.
        int cut135 = 0;
        for (int i = 0; i < total; i++) {
            char buf[256]; const char *n, *l1, *l2; uint8_t d, ch;
            menuInfo(i, &n, &l1, &l2, &d, &ch, buf, sizeof(buf));
            if (txtGlyphs(n) > colorBudget(135, 6, 6)) cut135++;
        }
        check(cut135 == 0, "і на найвужчій кольоровій (135) теж");
    }

    printf("\n10) назви ГРУП влазять у шапку\n");
    {
        // Шапка ділиться з лічильником «36/36» (5 гліфів) плюс відступ.
        const int HDR240 = (240 - 6 * 2) / 10 - 6;    // FONT_HDR_W=10 на 240
        const int HDR135 = (135 - 6 * 2) / 7 - 6;     // FONT_HDR_W=7 на вузьких
        int over = 0;
        for (int g = 0; g < MG_COUNT; g++) {
            const char *n = menuGroupName(g);
            if (!*n) continue;
            if (txtGlyphs(n) > HDR240 || txtGlyphs(n) > HDR135) {
                printf("   не влазить: «%s» (%d гліфів, місця %d/%d)\n",
                       n, txtGlyphs(n), HDR240, HDR135);
                over++;
            }
        }
        printf("   місця на назву групи: 240 -> %d гліфів, 135 -> %d\n", HDR240, HDR135);
        check(over == 0, "усі назви груп влазять поруч із лічильником");
    }

    printf("\n11) txtFit: обрізає по гліфах і позначає обрізане\n");
    {
        char b[64];
        txtFit(b, sizeof(b), "Ціль заряду 100%", 16);
        check(!strcmp(b, "Ціль заряду 100%"), "рівно за розміром — не чіпає");
        txtFit(b, sizeof(b), "Ціль заряду 100%", 10);
        check(txtGlyphs(b) == 10, "довше — рівно стільки гліфів, скільки дозволено");
        check(b[strlen(b) - 1] == '.', "…і позначено крапкою, що назва довша");
        txtFit(b, sizeof(b), "Модель PMNN4409B", 0);
        check(b[0] == '\0', "нульова ширина — порожньо, а не сміття");
        // Головне, заради чого позначка: дві різні моделі не сміють обрізатись
        // в один і той самий текст МОВЧКИ.
        char x[64], y[64];
        txtFit(x, sizeof(x), "Модель PMNN4409B", 12);
        txtFit(y, sizeof(y), "Модель PMNN4409A", 12);
        check(!strcmp(x, y) && x[strlen(x) - 1] == '.',
              "однаково обрізані назви несуть позначку обрізання");
    }

    // ── 12. Версія пристрою без заряду ─────────────────────────────────────
    //  Є екземпляри, зібрані без силової частини. Заряд у них вимикають на
    //  самому приладі, і меню мусить це пережити: не «сірі пункти», а їх
    //  відсутність — разом із заголовком групи.
    printf("\n12) версія пристрою без заряду: група ЗАРЯД зникає цілком\n");
    {
        const int CHG_OPS[] = { OP_CHARGE_DCDC, OP_CHARGE_TGT, OP_CHARGE_WAKE,
                                OP_CHARGE_PROFILE, OP_CHARGE_MANUAL_MA, OP_CHARGE_MANUAL_MV };
        const int NCHG = (int)(sizeof(CHG_OPS) / sizeof(CHG_OPS[0]));

        // а) opNeedsCharge() описує РІВНО ці операції — не менше й не більше.
        //    Саме за цим переліком дію відхиляють у вебі, по USB і зі сценарію
        //    Майстра, тож зайвий пункт у ньому вимкнув би працездатну функцію,
        //    а забутий — лишив би працювати ту, якої в пристрої немає.
        int mism = 0;
        for (int c = 0; c < opCount(); c++) {
            bool inList = false;
            for (int k = 0; k < NCHG; k++) if (CHG_OPS[k] == c) inList = true;
            if (opNeedsCharge(c) != inList) { mism++; printf("      код %d\n", c); }
        }
        check(mism == 0, "opNeedsCharge() називає рівно операції групи ЗАРЯД");
        check(!opNeedsCharge(OP_SETCHARGE),
              "«Заряд з напруги» силової частини НЕ потребує — лишається робочим");

        int full = menuCount();
        g_haveCharge = false;
        int cut = menuCount();
        printf("   пунктів: %d -> %d\n", full, cut);
        check(cut == full - NCHG, "зникло рівно стільки пунктів, скільки в групі");

        // б) жодного пункту заряду в списку, і жодного «порожнього» рядка.
        int leftovers = 0, broken = 0;
        for (int i = 0; i < cut; i++) {
            uint8_t k, g; int c;
            if (!menuRow(i, &k, &c, &g)) { broken++; continue; }
            if (g == MG_CHARGE) leftovers++;
            if (k == MI_OP && opNeedsCharge(c)) leftovers++;
        }
        check(broken == 0,    "усі рядки коротшого списку розбираються");
        check(leftovers == 0, "у списку не лишилось ні пункту заряду, ні його групи");

        // в) решта каталогу нікуди не поділась — вимкнули заряд, а не пристрій.
        int lost = 0;
        for (int c = 0; c < opCount(); c++)
            if (!opNeedsCharge(c) && menuIndexOfOp(c) < 0) { lost++; printf("      зник код %d\n", c); }
        check(lost == 0, "жодна операція поза групою ЗАРЯД не зникла");

        // г) навігація по групах не спотикається об порожній відрізок.
        int at = 0, hops = 0;
        bool sane = true;
        for (; hops < MG_COUNT + 4; hops++) {
            at = menuNextGroup(at);
            uint8_t k, g; int c;
            if (!menuRow(at, &k, &c, &g)) { sane = false; break; }
            if (g == MG_CHARGE)           { sane = false; break; }
            if (at == 0) break;
        }
        check(sane && at == 0, "стрибок по групах обходить список і не потрапляє в порожню групу");

        // д) курсор, що стояв у кінці довгого списку, не лишається за межами.
        check(menuClampSel(full - 1) == cut - 1, "курсор із кінця довгого списку затискається");
        check(menuClampSel(0) == 0,              "початок списку затиск не чіпає");

        // е) увімкнули назад — усе повернулось на місце, байт у байт.
        g_haveCharge = true;
        check(menuCount() == full, "увімкнули заряд назад — список той самий");
        int back = 0;
        for (int k = 0; k < NCHG; k++) if (menuIndexOfOp(CHG_OPS[k]) >= 0) back++;
        check(back == NCHG, "усі пункти заряду повернулись");
    }

    printf("\n13) зв'язок: ПЕРШИЙ пункт списку, і зникнути йому нема з чим\n");
    {
        // ⚑ ПОЗИЦІЮ ЗАДАВ ВЛАСНИК, І ВОНА НЕ КОСМЕТИЧНА. До перемикача радіо
        //  доводиться діставатись саме тоді, коли зв'язку немає, — тобто не
        //  через клієнта, а кнопками, наосліп. Кожен рядок над ним — зайве
        //  натискання навпомацки.
        int idx = menuIndexOfOp(OP_RADIO);
        char m[140];
        snprintf(m, sizeof(m), "«Зв'язок» стоїть %d-м рядком (треба 2-м: під «‹ Показання»)", idx + 1);
        check(idx == 1, m);

        // ⚑ І ЦЕ РІВНО ТОЙ РЯДОК, НА ЯКОМУ КУРСОР СТОЇТЬ ПРИ ВХОДІ В МЕНЮ.
        //  Драйвер починає з g_menuSel = 1, бо рядок 0 — не пункт, а вихід.
        //  Якби перемикач поставили НАД виходом, дорога назад щоразу шукалась
        //  би не там, де вона є.
        uint8_t k0 = MI_OP, g0 = MG_NAV; int c0 = 0;
        check(menuRow(0, &k0, &c0, &g0) && k0 == MI_PAGE,
              "…а нульовий рядок лишається виходом, а не пунктом");

        // Група своя, і заголовок у неї є: без заголовка пункт прочитався б як
        // продовження РЕМОНТУ.
        uint8_t kind = MI_OP, group = MG_NAV; int code = 0;
        bool got = menuRow(idx, &kind, &code, &group);
        check(got && kind == MI_OP && code == OP_RADIO, "…і це справді пункт «Зв'язок»");
        check(got && group == MG_LINK, "…у власній групі");
        check(menuGroupStarts(idx), "…і група з нього починається");
        check(menuGroupName(MG_LINK)[0] != '\0', "…а в групи є заголовок");

        // ⚑ І ГОЛОВНЕ — ВІН НЕ ЗНИКАЄ РАЗОМ ІЗ ЗАРЯДОМ. Група ЗАРЯД на версії
        //  без силової частини зникає цілком; якби «Зв'язок» поїхав із нею, то
        //  на приладі без заряду перемкнути радіо було б нічим — а це рівно
        //  той прилад, до якого достукатись можна тільки по радіо.
        g_haveCharge = false;
        int idxNo = menuIndexOfOp(OP_RADIO);
        snprintf(m, sizeof(m), "на версії без заряду лишається на місці (%d-м)", idxNo + 1);
        check(idxNo == 1, m);
        g_haveCharge = true;

        // Заряду цей пункт не потребує — інакше гейт chargeGateClosed() відхиляв
        // би його на тому самому приладі.
        check(!opNeedsCharge(OP_RADIO), "…і заряду для нього не потрібно");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
