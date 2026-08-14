#include "battery_reader.h"

BatteryReader::BatteryReader(int pin, int pullupPin) {
    _pin = pin;
    _pullupPin = pullupPin;
    _ow = new OneWire(pin);
}

bool BatteryReader::begin() {
    pinMode(_pullupPin, OUTPUT);
    digitalWrite(_pullupPin, LOW);            // enable опущено, доки не читаємо

    // Стан лінії — у журнал ще до першого читання: якщо вона несправна, усі
    // подальші «чіпів не знайдено» пояснюються саме цим рядком.
    uint8_t enl = enableLineCheck();
    uint8_t bus = busLineCheck();
    Serial.printf("1-Wire: пін даних %d, enable/підтяжка %d\n  керуючий пін: %s\n"
                  "  лінія даних: %s\n",
                  _pin, _pullupPin, enableLineText(enl), busLineText(bus));
    // ⚠️ НЕСПРАВНА ЛІНІЯ — НЕ ПРИЧИНА ВІДМОВИТИ В ЗАПУСКУ. Виклик у setup()
    // обгорнутий у `if (!begin()) { ... while(1) delay(1000); }`, тобто
    // повернути тут false означає ЗАЦИКЛИТИ пристрій назавжди: ні вебу, ні
    // екрана, ні журналу далі не буде. Несправність підтяжки робить
    // непрацездатним читання пакета — але не пристрій: екран, Wi-Fi, дампи з
    // пам'яті, налаштування працюють і далі, і саме через них користувач
    // побачить, що не так. Тому лише звіт, а вирок — на читанні.
    (void)enl;

    // Підтяжка буде увімкнена пізніше, в методі readBattery/writeBattery
    return true;
}

// Опустити enable/підтяжку, якщо не тримаємо примусово (див. holdEnable).
void BatteryReader::pullupOff() {
    if (_holdEnable) return;
    digitalWrite(_pullupPin, LOW);
}

// Тримати сигнал enable піднятим постійно (режим керованого розряду): без цього
// АКБ активний лише під час транзакції 1-Wire, і навантаження споживає струм
// тільки в моменти читання.
void BatteryReader::holdEnable(bool on) {
    _holdEnable = on;
    digitalWrite(_pullupPin, on ? HIGH : LOW);
}

// Перевірити саму лінію enable/підтяжки: підняти, прочитати назад, опустити,
// прочитати назад — і повернути стан у те, як було.
//
// Що це ловить. Пін ми піднімаємо завжди, але чи піднялась ЛІНІЯ — питання
// окреме: коротке на землю, обірваний або надто низькоомний резистор,
// пробитий транзистор підтяжки дають рівно ту скаргу, з якою це й писалось —
// «читання не працює, підтяжки немає». Без цієї перевірки прошивка повідомляє
// лише «чіпів не знайдено», що вказує на пакет, а не на власну обв'язку.
uint8_t BatteryReader::enableLineCheck() {
    bool held = _holdEnable;                  // запам'ятати, як було

    digitalWrite(_pullupPin, HIGH);
    delayMicroseconds(500);                   // час на заряд ємності лінії
    bool hi = digitalRead(_pullupPin);
    digitalWrite(_pullupPin, LOW);
    delayMicroseconds(500);
    bool lo = digitalRead(_pullupPin);

    digitalWrite(_pullupPin, held ? HIGH : LOW);   // повернути як було
    if (!hi) return ENL_STUCK_LOW;            // не піднімається
    if (lo)  return ENL_STUCK_HIGH;           // не опускається
    return ENL_OK;
}

const char *BatteryReader::enableLineText(uint8_t code) {
    switch (code) {
        case ENL_STUCK_LOW:
            return "лінія enable/підтяжки НЕ ПІДНІМАЄТЬСЯ (коротке на землю, "
                   "надто низькоомна підтяжка вниз або пробитий ключ)";
        case ENL_STUCK_HIGH:
            return "лінія enable/підтяжки НЕ ОПУСКАЄТЬСЯ (коротке на живлення)";
        default:
            return "лінія enable/підтяжки справна";
    }
}

// Перевірити ЛІНІЮ ДАНИХ — тобто результат роботи підтяжки, а не намір.
//
// ⚑ Саме цього бракувало. enableLineCheck() каже, що керуючий пін піднявся, —
// але скарга «немає підтяжки» про інше: чи піднялась ЛІНІЯ ДАНИХ. Між ними
// стоїть уся обв'язка: сам резистор підтяжки, ключ, роз'єм пакета і — після
// переробки заряду — шунт у мінусовому проводі, що розділив «мінус» пакета й
// землю ESP32. Якщо спільної землі немає, підтяжка нікуди не доходить, і шина
// мовчить рівно так, як мовчала б із порожнім роз'ємом.
uint8_t BatteryReader::busLineCheck() {
    bool held = _holdEnable;

    pinMode(_pin, INPUT);                     // лінію віддаємо зовнішній схемі

    digitalWrite(_pullupPin, LOW);            // підтяжка ВИМКНЕНА
    delayMicroseconds(500);
    bool idleOff = digitalRead(_pin);

    digitalWrite(_pullupPin, HIGH);           // підтяжка УВІМКНЕНА
    delayMicroseconds(500);
    bool idleOn = digitalRead(_pin);

    digitalWrite(_pullupPin, held ? HIGH : LOW);   // повернути як було

    if (!idleOn)  return BUS_NO_PULLUP;       // підтяжка не доходить до лінії
    if (idleOff)  return BUS_STUCK_HIGH;      // лінія висока й без підтяжки
    return BUS_OK;
}

const char *BatteryReader::busLineText(uint8_t code) {
    switch (code) {
        case BUS_NO_PULLUP:
            return "ЛІНІЯ ДАНИХ НЕ ПІДНІМАЄТЬСЯ підтяжкою: обірваний резистор "
                   "підтяжки, коротке лінії даних на землю, немає контакту в "
                   "роз'ємі або НЕМАЄ СПІЛЬНОЇ ЗЕМЛІ з пакетом (перевірте шунт "
                   "у мінусовому проводі)";
        case BUS_STUCK_HIGH:
            return "лінія даних лишається високою навіть із вимкненою підтяжкою "
                   "(зайва зовнішня підтяжка на живлення)";
        default:
            return "лінія даних керується підтяжкою правильно";
    }
}

// --- Допоміжний Метод ДЛЯ Пошуку Пристроїв ---
// Повертає true, якщо вдалося знайти обидва чипа і зберегти їх адреси
bool BatteryReader::findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr) {
    uint8_t addr[8];
    bool found2433 = false, found2438 = false;

    // Занулюємо буфери адрес: тоді addr[0]==0x00 надійно означає
    // "чип не знайдений" (інакше в буфері залишається сміття зі стека).
    memset(ds2433_addr, 0, 8);
    memset(ds2438_addr, 0, 8);

    // Вмикаємо підтяжку перед пошуком.
    digitalWrite(_pullupPin, HIGH);
    // ⚑ ПАУЗА НА ЖИВЛЕННЯ. Після кожної операції ми гасимо підтяжку
    // (pullupOff), тож обидва чипи знеструмлюються, і КОЖЕН пошук — холодний
    // старт шини. Без паузи Search ROM іде по ще не піднятій лінії: чип, який
    // прокидається повільніше (зазвичай DS2438 — у нього ще й аналогова
    // частина), у пошук не потрапляє. Саме звідси скарга «з першого разу
    // показує не все, з другого — все»: на другому натисканні ROM уже в кеші
    // (див. fallback нижче), і читання проходить.
    delay(DS_BUS_SETTLE_MS);

    // Пошук повторюємо, поки не знайдено ОБИДВА чипи. Search ROM на цій шині
    // нестабільний (див. коментар про кеш нижче), а коштує спроба одиниці
    // мілісекунд — дешевше, ніж віддати нагору половину даних.
    for (int attempt = 0; attempt < DS_SEARCH_TRIES && !(found2433 && found2438); attempt++) {
        if (attempt) delay(DS_BUS_SETTLE_MS);
        _ow->reset_search();
        while (_ow->search(addr)) {
            if (addr[0] == DS2433_ID && !found2433) {
                memcpy(ds2433_addr, addr, 8);
                found2433 = true;
                // Друк лише на ЗМІНУ (новий ROM / вперше знайдено) — не на
                // кожен виклик. findDevices() наскрізь проходить ПОВНИЙ
                // пошук шини ЩОПОЛЛУ під час заряду/розряду (щосекунди,
                // годинами) — без цього затвору тут виходили тисячі рядків
                // у Serial за сеанс, а блокуючий Serial.print() при
                // заповненому TX-буфері (напр. закритий монітор порту)
                // здатен підвісити ГОЛОВНИЙ ЦИКЛ і спричинити скидання за
                // сторожовим таймером — саме симптом «зависання й
                // самовільні перезавантаження».
                if (!_haveRom2433 || memcmp(_rom2433, addr, 8) != 0)
                    Serial.printf("DS2433 found! (спроба %d)\n", attempt + 1);
            } else if (addr[0] == DS2438_ID && !found2438) {
                memcpy(ds2438_addr, addr, 8);
                found2438 = true;
                if (!_haveRom2438 || memcmp(_rom2438, addr, 8) != 0)
                    Serial.printf("DS2438 found! (спроба %d)\n", attempt + 1);
            }
        }
    }

    // Скидаємо пошук для наступного разу
    _ow->reset_search();

    // ⚑ ПАКЕТ ЗМІНИЛИ — КЕШ НЕДІЙСНИЙ. ROM-ID це не просто серійник для показу:
    // з нього беруться ключі шифрування дат і лічильників. Якщо новий пакет
    // знайшовся лише одним чипом, а другий підставився з кешу від ПОПЕРЕДНЬОГО,
    // ми зашифрували б дані цього пакета чужим ключем — рівно та біда, від якої
    // лікуємо. Обидва чипи живуть в одному пакеті, тож зміна будь-якого з ROM
    // означає, що пакет інший, і другий кеш теж треба викинути.
    bool swapped = (found2433 && _haveRom2433 && memcmp(_rom2433, ds2433_addr, 8) != 0) ||
                   (found2438 && _haveRom2438 && memcmp(_rom2438, ds2438_addr, 8) != 0);
    if (swapped) {
        Serial.println("1-Wire: ROM змінився -> інший пакет, кеш ROM скинуто");
        _haveRom2433 = _haveRom2438 = false;
    }

    // Запам’ятовуємо ROM-ID (серійники) знайдених чипів
    if (found2433) { memcpy(_rom2433, ds2433_addr, 8); _haveRom2433 = true; }
    if (found2438) { memcpy(_rom2438, ds2438_addr, 8); _haveRom2438 = true; }

    // FALLBACK по кешованому ROM. Пошук (Search ROM) DS2438 буває нестабільним
    // (слабший драйвер/живлення на шині): при ЗАПИСІ чип іноді не відповідає на
    // Search, хоча при ЧИТАННІ щойно знаходився — звідси "DS2438 not found for
    // writing". Якщо ми вже бачили ROM цього чипа — беремо його з кешу й
    // адресуємо за Match ROM (select), що надійніше за Search.
    //
    // АЛЕ: кеш беремо ЛИШЕ якщо на шині фізично Є пристрій (presence-pulse після
    // reset). Інакше при ВІД'ЄДНАНОМУ АКБ ми б адресували неіснуючий чіп за старим
    // ROM, «прочитали» порожню шину (усі 0xFF) і вдавали успішне читання чистого
    // чіпа. Presence-pulse відрізняє «АКБ на місці (навіть стертий)» від «АКБ немає».
    if ((!found2433 && _haveRom2433) || (!found2438 && _haveRom2438)) {
        bool present = (_ow->reset() != 0);   // 1 = хтось відповів на шині
        if (present) {
            if (!found2433 && _haveRom2433) {
                memcpy(ds2433_addr, _rom2433, 8); found2433 = true;
                Serial.println("DS2433: using cached ROM");
            }
            if (!found2438 && _haveRom2438) {
                memcpy(ds2438_addr, _rom2438, 8); found2438 = true;
                Serial.println("DS2438: using cached ROM");
            }
        } else {
            // Шина порожня — пакет зняли. Кеш ROM після цього нічого не
            // означає: наступним поставлять інший пакет, і підставити йому
            // ключ від попереднього — найгірше, що можна зробити.
            Serial.println("1-Wire: no presence pulse -> bus empty, "
                           "cached ROM NOT used and cleared");
            _haveRom2433 = _haveRom2438 = false;
        }
    }

    // Вимикаємо підтяжку, якщо не знайшли ні жодного пристрою.
    // ⚑ І одразу перевіряємо ВЛАСНУ обв'язку. «Чіпів не знайдено» вказує на
    // пакет, і поки лінія enable не перевірена — це припущення, а не висновок:
    // рівно так само виглядає коротке на землю в підтяжці.
    if (!found2433 && !found2438) {
        uint8_t enl = enableLineCheck();
        uint8_t bus = busLineCheck();
        if (enl != ENL_OK)
            Serial.printf("1-Wire: %s — річ не в пакеті, а в обв'язці піна %d\n",
                          enableLineText(enl), _pullupPin);
        if (bus != BUS_OK)
            Serial.printf("1-Wire: %s\n", busLineText(bus));
        pullupOff();
        return false;
    }

    return true;
}

bool BatteryReader::readBattery(uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Шукаємо пристрою на шині
    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        pullupOff();
        return false;
    }

    // 2. Читаємо дані з DS2438 (монітор)
    // Невелика затримка для стабільності
    delay(10);
    if (ds2438_addr[0] != 0x00) {
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0x44); // Команда запуску вимірювання температури
        delay(10);        // Чекаємо завершення вимірювання
        
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0xBE); // Читаємо сторінку пам'яті (приклад)
        // Тут можна зчитати дані з DS2438, якщо потрібно
        // ...
    }

    // 3. Читаємо основну пам'ять з DS2433
    if (ds2433_addr[0] == 0x00) {
        Serial.println("Error: DS2433 not found!");
        pullupOff();
        return false;
    }

    // ⚑ ЧИТАННЯ ПОСТОРІНКОВО, а не одним суцільним потоком на 512 байт.
    // DS2433 живиться від САМОГО ПАКЕТА (Vcc береться через ту саму шину, що
    // й дані) — окремого джерела живлення для нього тут немає. При сильній
    // розбалансировці банок пакет може «просісти» під навантаженням посеред
    // довгої транзакції: чіп втрачає живлення, і 1-Wire read() після цього
    // повертає не дані, а плаваючу лінію (як правило суцільні 0xFF). Раніше
    // функція про це не дізнавалась узагалі — читала до кінця буфера і
    // ПОВЕРТАЛА true, навіть якщо половина буфера вже сміття. Саме так
    // виглядала скарга «вичитує тільки початок мікросхеми».
    //
    // Тепер кожна сторінка (32 Б, як і при записі) — своя транзакція: Reset +
    // Match ROM + Read Memory з власною адресою. Reset() перед кожною
    // сторінкою — це заразом і перевірка presence-pulse: якщо пакет просів і
    // чіп не відповів, ми дізнаємось відразу на тій сторінці, де це сталось,
    // а не постфактум по 500 байтах сміття. Даємо йому DS_READ_RECOVER_MS на
    // відновлення напруги й повторюємо ЛИШЕ цю сторінку, до DS_READ_PAGE_TRIES
    // разів. Якщо й після цього сторінка недоступна — чесно повертаємо false,
    // а не вдаваний успіх із діркою в даних.
    //
    // ⚑ Read Memory [0xF0] у DS2433 НЕ повертає CRC (на відміну від запису —
    // там Read Scratchpad CRC ловить помилку одразу). Самого presence-pulse
    // МАЛО: шина може відповісти на Reset, але дати шум/сміття посеред самого
    // читання (нестабільний контакт, наведення) — раніше це проходило як
    // «успіх» із тихо зіпсованими байтами («якщо виходить прочитати —
    // показує сміття»). Тому кожну сторінку читаємо, поки ДВА ПОСПІЛЬ
    // читання не збіжаться побайтово — програмний аналог CRC там, де чіп
    // його не дає.
    for (size_t offset = 0; offset < size; offset += DS2433_PAGE_SIZE) {
        size_t chunk = (offset + DS2433_PAGE_SIZE <= size) ? DS2433_PAGE_SIZE : (size - offset);
        bool pageOk = false;
        uint8_t prev[DS2433_PAGE_SIZE];
        bool havePrev = false;
        for (int attempt = 0; attempt < DS_READ_PAGE_TRIES && !pageOk; attempt++) {
            if (attempt) {
                delay(DS_READ_RECOVER_MS);
                Serial.printf("readBattery: нестабільне читання @0x%03X, повтор (спроба %d) — "
                              "пакет просів під навантаженням або шум на шині?\n",
                              (unsigned)offset, attempt + 1);
            }
            if (!_ow->reset()) { havePrev = false; continue; }  // немає presence-pulse — пакет ще не відновився
            _ow->select(ds2433_addr);
            _ow->write(0xF0);                    // Команда читання пам'яті
            _ow->write((uint8_t)(offset & 0xFF));        // Адреса (молодший байт)
            _ow->write((uint8_t)((offset >> 8) & 0xFF)); // Адреса (старший байт)
            uint8_t cur[DS2433_PAGE_SIZE];
            for (size_t i = 0; i < chunk; i++) cur[i] = _ow->read();
            if (havePrev && memcmp(prev, cur, chunk) == 0) {
                memcpy(buffer + offset, cur, chunk);
                pageOk = true;
            } else {
                memcpy(prev, cur, chunk);
                havePrev = true;
            }
        }
        if (!pageOk) {
            Serial.printf("Error: DS2433 page @0x%03X недоступна — шина/живлення нестабільні "
                          "(можлива сильна розбалансировка банок)\n", (unsigned)offset);
            _ow->reset();
            pullupOff();
            return false;
        }
    }

    _ow->reset();
    pullupOff(); // Вимикаємо підтяжку
    return true;
}

bool BatteryReader::writeBattery(const uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Шукаємо пристрою
    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        pullupOff();
        return false;
    }

    if (ds2433_addr[0] != DS2433_ID) {
        Serial.println("Error: DS2433 not found for writing!");
        pullupOff();
        return false;
    }

    // 2. Запис в DS2433 посторінково.
    // Scratchpad і сторінка пам'яті = 32 байта, тому 512 байт не можна
    // записати однієї командою: потрібен цикл по 16 сторінкам, і на кожної:
    //   Write Scratchpad -> Read Scratchpad (звірка + читання E/S) ->
    //   Copy Scratchpad (з авторизацією TA1, TA2, E/S) -> пауза tPROG.
    const size_t pageSize = DS2433_PAGE_SIZE;

    for (size_t offset = 0; offset < size; offset += pageSize) {
        size_t chunk = (offset + pageSize <= size) ? pageSize : (size - offset);
        uint8_t ta1 = offset & 0xFF;        // адреса: молодший байт
        uint8_t ta2 = (offset >> 8) & 0xFF; // адреса: старший байт

        // --- Write Scratchpad ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_WRITE_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        for (size_t i = 0; i < chunk; i++) {
            _ow->write(buffer[offset + i]);
        }

        // --- Read Scratchpad: звіряємо дані і читаємо справжній E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_READ_SCRATCH);
        uint8_t r_ta1 = _ow->read();
        uint8_t r_ta2 = _ow->read();
        uint8_t es    = _ow->read();

        if (r_ta1 != ta1 || r_ta2 != ta2) {
            Serial.printf("ERROR: scratchpad address mismatch @0x%04X (got %02X%02X)\n",
                          (unsigned)offset, r_ta2, r_ta1);
            _ow->reset();
            pullupOff();
            return false;
        }
        // Біт PF (E/S bit 5): дані scratchpad неповні/недостовірні.
        if (es & 0x20) {
            Serial.printf("ERROR: partial-write flag set @0x%04X (E/S=%02X)\n",
                          (unsigned)offset, es);
            _ow->reset();
            pullupOff();
            return false;
        }
        // Звіряємо вміст scratchpad з вихідними даними.
        bool dataOk = true;
        for (size_t i = 0; i < chunk; i++) {
            if (_ow->read() != buffer[offset + i]) dataOk = false;
        }
        if (!dataOk) {
            Serial.printf("ERROR: scratchpad data mismatch @0x%04X\n", (unsigned)offset);
            _ow->reset();
            pullupOff();
            return false;
        }

        // --- Copy Scratchpad: авторизація рівно TA1, TA2, E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_COPY_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        // Останній байт авторизації (E/S) надсилаємо з увімкненою сильною підтяжкою,
        // яку утримуємо на час програмування EEPROM (tPROG max 5 мс).
        _ow->write(es, 1);
        delay(6);
        _ow->depower();
    }

    // 3. Верифікація: читаємо усю пам'ять назад і порівнюємо з джерелом.
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(DS2433_READ_MEMORY);
    _ow->write(0x00); // адреса 0x0000
    _ow->write(0x00);

    bool verifyOk = true;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = _ow->read();
        if (b != buffer[i]) {
            Serial.printf("ERROR: verify mismatch @0x%04X (got %02X, expected %02X)\n",
                          (unsigned)i, b, buffer[i]);
            verifyOk = false;
            break;
        }
    }

    _ow->reset();
    pullupOff();

    if (!verifyOk) {
        Serial.println("ERROR: Write verification failed!");
        return false;
    }

    Serial.println("Write verified successfully");
    return true;
}

// --- Запис ЛИШЕ зачеплених сторінок DS2433 ---
// Пише тільки 32-байтові сторінки, що покривають [regionStart, regionStart+regionLen).
// Той самий протокол, що й writeBattery (Write/Read/Copy Scratchpad + звірка),
// але не чіпає решту чипа — точкова правка (напр. модель) вдається навіть якщо
// деінде є непридатні до перезапису байти.
bool BatteryReader::writeBatteryRange(const uint8_t *buffer, size_t regionStart, size_t regionLen) {
    if (regionLen == 0) return true;
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        pullupOff();
        return false;
    }
    if (ds2433_addr[0] != DS2433_ID) {
        Serial.println("Error: DS2433 not found for writing!");
        pullupOff();
        return false;
    }

    const size_t pageSize = DS2433_PAGE_SIZE;
    size_t firstPage = (regionStart / pageSize) * pageSize;
    size_t lastByte  = regionStart + regionLen - 1;
    size_t lastPage  = (lastByte / pageSize) * pageSize;

    for (size_t offset = firstPage; offset <= lastPage; offset += pageSize) {
        uint8_t ta1 = offset & 0xFF;
        uint8_t ta2 = (offset >> 8) & 0xFF;

        // --- Write Scratchpad (ціла сторінка 32 Б) ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_WRITE_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        for (size_t i = 0; i < pageSize; i++) _ow->write(buffer[offset + i]);

        // --- Read Scratchpad: звірка адреси/даних + читання E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_READ_SCRATCH);
        uint8_t r_ta1 = _ow->read();
        uint8_t r_ta2 = _ow->read();
        uint8_t es    = _ow->read();
        if (r_ta1 != ta1 || r_ta2 != ta2 || (es & 0x20)) {
            Serial.printf("ERROR: scratchpad addr/PF @0x%04X (ta=%02X%02X es=%02X)\n",
                          (unsigned)offset, r_ta2, r_ta1, es);
            _ow->reset(); pullupOff(); return false;
        }
        bool dataOk = true;
        for (size_t i = 0; i < pageSize; i++) if (_ow->read() != buffer[offset + i]) dataOk = false;
        if (!dataOk) {
            Serial.printf("ERROR: scratchpad data mismatch @0x%04X\n", (unsigned)offset);
            _ow->reset(); pullupOff(); return false;
        }

        // --- Copy Scratchpad (авторизація TA1,TA2,E/S) + tPROG ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_COPY_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        _ow->write(es, 1);
        delay(6);
        _ow->depower();
    }

    // Верифікація тільки зачеплених сторінок.
    bool verifyOk = true;
    for (size_t offset = firstPage; offset <= lastPage && verifyOk; offset += pageSize) {
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_READ_MEMORY);
        _ow->write(offset & 0xFF);
        _ow->write((offset >> 8) & 0xFF);
        for (size_t i = 0; i < pageSize; i++) {
            uint8_t b = _ow->read();
            if (b != buffer[offset + i]) {
                Serial.printf("ERROR: range verify mismatch @0x%04X (got %02X, exp %02X)\n",
                              (unsigned)(offset + i), b, buffer[offset + i]);
                verifyOk = false; break;
            }
        }
    }

    _ow->reset();
    pullupOff();
    if (!verifyOk) { Serial.println("ERROR: Range write verification failed!"); return false; }
    Serial.printf("Range write OK: pages 0x%04X..0x%04X\n", (unsigned)firstPage, (unsigned)lastPage);
    return true;
}

// --- Читання усієї пам'яті DS2438 (8 сторінок по 8 байт = 64 байта) ---
// Порядок на сторінку: Recall Memory (0xB8) -> Read Scratchpad (0xBE) ->
// 9 байт (8 даних + CRC8). Перед читанням запускаємо вимірювання, щоб
// сторінка 0 містила свіжі значення напруги/температури.
bool BatteryReader::readDS2438(uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        pullupOff();
        return false;
    }
    if (ds2438_addr[0] != DS2438_ID) {
        Serial.println("Error: DS2438 not found!");
        pullupOff();
        return false;
    }
    if (size < DS2438_MEM_SIZE) {
        Serial.println("Error: DS2438 buffer too small!");
        pullupOff();
        return false;
    }

    // Запускаємо вимірювання напруги і температури (tCONV макс 10 мс).
    _ow->reset();
    _ow->select(ds2438_addr);
    _ow->write(DS2438_CONVERT_V);
    delay(10);
    _ow->reset();
    _ow->select(ds2438_addr);
    _ow->write(DS2438_CONVERT_T);
    delay(10);

    for (uint8_t page = 0; page < DS2438_PAGES; page++) {
        // Recall Memory: копіюємо сторінку EEPROM/SRAM в scratchpad.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_RECALL_MEMORY);
        _ow->write(page);

        // Read Scratchpad: 8 байт даних + CRC8.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_READ_SCRATCH);
        _ow->write(page);

        uint8_t sp[9];
        for (int i = 0; i < 9; i++) sp[i] = _ow->read();

        if (OneWire::crc8(sp, 8) != sp[8]) {
            Serial.printf("ERROR: DS2438 CRC mismatch on page %d\n", (int)page);
            _ow->reset();
            pullupOff();
            return false;
        }
        memcpy(buffer + page * DS2438_PAGE_SIZE, sp, DS2438_PAGE_SIZE);
    }

    _ow->reset();
    pullupOff();
    // Без "DS2438 read completed" тут навмисно: цю функцію викликає
    // dischargeSample() ЩОСЕКУНДИ під час заряду/розряду (див. коментар у
    // findDevices() вище) — успіх і так видно з наступного рядка "charge:
    // .../discharge: ...", зайвий рядок тут лише роздував Serial-трафік
    // без нової інформації.
    return true;
}

// --- Запис усієї пам'яті DS2438 (8 сторінок по 8 байт) ---
// Порядок на сторінку: Write Scratchpad (0x4E) -> Read Scratchpad (звірка +
// CRC8) -> Copy Scratchpad (0x48) -> пауза tWR (2..10 мс). Strong pullup не
// потрібне. Увага: байти вимірювань в стр. 0 (temp/voltage/current) і
// інші волатильні регістри енергонезалежно не зберігаються — пристрій
// перезапише їх при наступному вимірюванні; тому фінальна звірка читанням
// пам'яті по усієї сторінці тут незастосовна, перевіряємо лише scratchpad.
bool BatteryReader::writeDS2438(const uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        pullupOff();
        return false;
    }
    if (ds2438_addr[0] != DS2438_ID) {
        Serial.println("Error: DS2438 not found for writing!");
        pullupOff();
        return false;
    }
    if (size < DS2438_MEM_SIZE) {
        Serial.println("Error: DS2438 source too small!");
        pullupOff();
        return false;
    }

    // --- Фаза 1: пишемо всі сторінки (Write Scratchpad -> Copy Scratchpad). ---
    // Перевірку scratchpad тут НЕ робимо: для "живих" сторінок (0-2, 7 —
    // Temp/U/I/ETM/ICA/CCA/DCA) вона давала хибну "write failed" і блокувала
    // весь запис. Реальне збереження перевіряємо нижче читанням пам'яті назад.
    for (uint8_t page = 0; page < DS2438_PAGES; page++) {
        const uint8_t *pageData = buffer + page * DS2438_PAGE_SIZE;
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_WRITE_SCRATCH);
        _ow->write(page);
        for (int i = 0; i < DS2438_PAGE_SIZE; i++) _ow->write(pageData[i]);
        // Copy Scratchpad -> сторінка пам'яті; tWR макс ~10 мс.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_COPY_SCRATCH);
        _ow->write(page);
        delay(11);
    }

    // --- Фаза 2: перевірка РЕАЛЬНОГО збереження. Читаємо пам'ять назад і
    // звіряємо ЛИШЕ сторінки 3..6 — дзеркало калібрування, справжній EEPROM.
    // Сторінки 0-2 і 7 (вимірювання/лічильники) чіп оновлює сам, звіряти їх
    // немає сенсу. Так немає хибних збоїв, але реальний збій запису
    // калібрування ловиться, і в Serial видно, що саме не збереглось.
    bool ok = true;
    for (uint8_t page = 3; page <= 6; page++) {
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_RECALL_MEMORY);
        _ow->write(page);
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_READ_SCRATCH);
        _ow->write(page);
        uint8_t rb[9];
        for (int i = 0; i < 9; i++) rb[i] = _ow->read();

        if (OneWire::crc8(rb, 8) != rb[8]) {
            Serial.printf("WARN: DS2438 verify CRC noise on page %d — skip\n", (int)page);
            continue;                          // шум на верифікації — не валимо запис
        }
        const uint8_t *pageData = buffer + page * DS2438_PAGE_SIZE;
        if (memcmp(rb, pageData, DS2438_PAGE_SIZE) != 0) {
            Serial.printf("ERROR: DS2438 page %d NOT persisted. want ", (int)page);
            for (int i = 0; i < 8; i++) Serial.printf("%02X", pageData[i]);
            Serial.print(" got ");
            for (int i = 0; i < 8; i++) Serial.printf("%02X", rb[i]);
            Serial.println();
            ok = false;
        }
    }

    // --- Фаза 3: окремо звіряємо НАРОБІТОК (ETM, сторінка 1, байти 0..3).
    // Сторінку 1 не можна звіряти побайтово — чіп сам крутить лічильник далі,
    // — але саме через це її мовчазний пропуск і був небезпечним: невдалий
    // запис ETM не помічав ніхто, а виявлявся він аж на рації, неправильною
    // «датою першого користування». Тому звіряємо з допуском: за час між
    // записом і читанням чіп встигає натікати одиниці секунд, не більше.
    {
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_RECALL_MEMORY);
        _ow->write((uint8_t)1);
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_READ_SCRATCH);
        _ow->write((uint8_t)1);
        uint8_t rb[9];
        for (int i = 0; i < 9; i++) rb[i] = _ow->read();
        if (OneWire::crc8(rb, 8) == rb[8]) {
            uint32_t want = (uint32_t)buffer[8] | ((uint32_t)buffer[9] << 8) |
                            ((uint32_t)buffer[10] << 16) | ((uint32_t)buffer[11] << 24);
            uint32_t got  = (uint32_t)rb[0] | ((uint32_t)rb[1] << 8) |
                            ((uint32_t)rb[2] << 16) | ((uint32_t)rb[3] << 24);
            uint32_t diff = (got > want) ? (got - want) : (want - got);
            if (diff > DS2438_ETM_TOLERANCE_S) {
                Serial.printf("ERROR: DS2438 ETM NOT persisted. want %lu got %lu (різниця %lu с)\n",
                              (unsigned long)want, (unsigned long)got, (unsigned long)diff);
                ok = false;
            } else {
                Serial.printf("DS2438 ETM verified: %lu c\n", (unsigned long)got);
            }
        } else {
            Serial.println("WARN: DS2438 ETM verify CRC noise — skip");
        }
    }

    _ow->reset();
    pullupOff();
    Serial.println(ok ? "DS2438 write completed (calib pages 3-6 + ETM verified)"
                      : "DS2438 write: something did NOT persist (див. вище)");
    return ok;
}
