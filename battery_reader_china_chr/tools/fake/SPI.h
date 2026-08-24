#ifndef FAKE_SPI_H
#define FAKE_SPI_H
// ===========================================================================
//  Заглушка SPI для ХОСТОВОЇ ЗБІРКИ. Малює вона в нікуди — перевіряємо не
//  картинку, а те, що драйвер екрана взагалі КОМПІЛЮЄТЬСЯ. Див.
//  tools/display_build_check.cpp: доти цієї перевірки не було зовсім, і будь-яка
//  описка в display_color.h знаходилась уже в Arduino IDE — тобто після того,
//  як прошивку залили в прилад і побачили чорний екран.
// ===========================================================================
#include <stdint.h>
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
class SPIClass {
public:
    void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
    void end() {}
};
static SPIClass SPI;
#endif
