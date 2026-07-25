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

## Розбір і звірка дампів (`decode_dump.py`)

Анотований розбір дампа IMPRES-чіпа й побайтова звірка «еталон vs клон».
Перевіряє ті самі інваріанти, що й прошивка: заголовок DS2433 Σ≡0x41,
TLV-записи Σ≡0x5A, дзеркало DS2438[24:50]↔DS2433[1:27], ємність (запис 0x17+21),
ETM/ICA/CCA/DCA у DS2438.

```bash
# Розбір дампа (DS2438 — необов'язковий)
python tools/decode_dump.py dump33.bin dump38.bin

# Витягти вшитий еталон із templates.h у .bin (для звірки клонів)
python tools/decode_dump.py --template PMNN4409B --out ref
#   -> ref_33.bin (512) + ref_38.bin (64)

# Побайтова звірка: «●» — розбіжність в ідентичності/калібруванні (важливо),
#                   «○» — лічильники/живі виміри (норма після скидання)
python tools/decode_dump.py ref_33.bin ref_38.bin --diff clone33.bin clone38.bin
```

## Готовий нейтральний варіант

`tools/make_splash.py` можна нагодувати будь-якою картинкою. Приклад згенерованої
нейтральної новорічної заставки (сніжинка) — див. запит до асистента.

> ⚠️ Не використовуйте захищені авторським правом персонажі/лого без прав на них.
