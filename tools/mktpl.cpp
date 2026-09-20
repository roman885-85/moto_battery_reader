// Очистити навчені дані донора з дампа-кандидата в еталон — ТИМ САМИМ кодом
// пошуку блоків, яким користуються прошивка й tpl_check. Наївний обчислювач
// адрес розходився з impresBmsVector() і чистив не ті блоки.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#define DUMP_SIZE 512
#define DS2438_MEM_SIZE 64
#define DS2438_RSENSE_OHM 0.025f
#define DS2438_MAH_PER_LSB (0.4882f / DS2438_RSENSE_OHM)
#include "settings.h"
#include "impres_format.h"
#include "impres_bms.h"
int main(int c, char **v) {
    uint8_t d[DUMP_SIZE];
    FILE *f = fopen(v[1], "rb"); fread(d, 1, DUMP_SIZE, f); fclose(f);
    for (int vv : {70, 76, 78, 80, 84, 88, 92}) {
        uint16_t a = impresBmsVector(d, vv);
        if (a == BMS_INVALID || a < IMPRES_LEARNED_BEGIN) continue;
        uint8_t ln = d[a];
        if (ln < 4 || a + ln > DUMP_SIZE) continue;
        uint8_t keep = d[a + ln - 2];
        for (int j = 1; j < ln - 1; j++) d[a + j] = 0;
        if (vv == 88) d[a + ln - 2] = keep;      // EOSPercentage — стала моделі
        impresFixRecord(d, a, ln);
        fprintf(stderr, "очищено v%d @0x%03X len=%u\n", vv, (unsigned)a, ln);
    }
    impresFixHeader(d);
    f = fopen(v[2], "wb"); fwrite(d, 1, DUMP_SIZE, f); fclose(f);
    fprintf(stderr, "заголовок Σ=0x%02X\n", (unsigned)(([&]{uint8_t s=0;for(int i=0;i<0x20;i++)s+=d[i];return s;})()));
    return 0;
}
