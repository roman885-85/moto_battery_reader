# Заставка дисплея (splash)

Стартова заставка — монохромна битмапа **64×64** у `display.h`
(`static const unsigned char ngu_xbm[] = { ... };`, розмір `NGU_W`/`NGU_H`).

Для **кольорових TFT ST7789** — окремий генератор кольорової заставки нижче.

## Замінити на своє зображення (монохромні дисплеї)

Використайте **своє** зображення (на яке маєте права):

```bash
pip install pillow
python tools/make_splash.py моя_картинка.png            # темне -> світиться
python tools/make_splash.py моя_картинка.png --invert   # інверсія
python tools/make_splash.py моя_картинка.png --dither    # напівтони (дизеринг)
```

Скрипт надрукує готовий блок `#define NGU_W/H` + `ngu_xbm[]` — вставте його у
`display.h` замість наявного масиву. Розмір лишається 64×64.

> ⚠️ Не використовуйте захищені авторським правом персонажі/лого без прав на них.

## Кольорова заставка (TFT ST7789)

Для кольорових дисплеїв — `make_color_splash.py`: генерує `custom_splash.h`
з масивом **RGB565** під вашу роздільність.

```bash
pip install pillow
python tools/make_color_splash.py logo.png -W 240 -H 240     # ST7789VW 240x240
python tools/make_color_splash.py logo.png -W 240 -H 280     # ST7789V3 240x280
python tools/make_color_splash.py logo.png -W 240 -H 320 --fit cover
python tools/make_color_splash.py logo.png --bg 001B3A       # колір фону (HEX)
```

Покладіть згенерований `custom_splash.h` у папку скетчу і ввімкніть у
`settings.h`:  `#define DISPLAY_SPLASH_CUSTOM`. Заставка малюється по центру.

## Готовий нейтральний варіант

`tools/make_splash.py` можна нагодувати будь-якою картинкою. Приклад згенерованої
нейтральної новорічної заставки (сніжинка) — див. запит до асистента.

> ⚠️ Не використовуйте захищені авторським правом персонажі/лого без прав на них.
