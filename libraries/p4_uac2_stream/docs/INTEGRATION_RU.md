# Подключение p4_uac2_stream

## Требования

- ESP32-P4 rev1.x; аппаратно проверена rev1.3, ESP-IDF **5.5.5**.
- TinyUSB **0.19.0~3**, версия зафиксирована в `idf_component.yml`.
- USB OTG High Speed, внутренний PHY, `rhport=1`.
- Источник и потребитель PCM работают от одного аудиотакта Seed3.
- В проекте только один владелец USB device stack и его дескрипторов.
  Одновременно подключать `espressif__usb_device_uac`, другой TinyUSB audio
  driver или собственную USB task нельзя.

USB-A HS подключается по безопасной схеме питания конкретной платы.
На испытанном стенде J1 соединён с ПК через data-only адаптер D+/D−/GND
с разорванным VBUS между ПК и платой. Обычный USB-A ↔ USB-A кабель не применять.

## Подключение к своему проекту

1. Скопируйте **всю эту библиотеку** в `your_project/components/p4_uac2_stream`.
   Для сборки нужны корневые CMake/manifest, C-файлы, `include`, `private`,
   `port`. Документацию и пример можно оставить: они не компилируются в компонент.
2. В `main/CMakeLists.txt` добавьте зависимость, сохранив зависимости своего SPI:

   ```cmake
   idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
       REQUIRES p4_uac2_stream esp_driver_spi esp_driver_gpio)
   ```

3. Уберите старый UAC-компонент из зависимостей приложения. Если он должен
   остаться на диске, исключите его до `project.cmake` в корневом CMake:

   ```cmake
   cmake_minimum_required(VERSION 3.16)
   set(EXCLUDE_COMPONENTS espressif__usb_device_uac)
   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   project(my_seed_audio_bridge)
   ```

4. Для rev1.x задайте следующие параметры в `sdkconfig.defaults`:

   ```text
   CONFIG_IDF_TARGET="esp32p4"
   CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
   CONFIG_ESP32P4_REV_MIN_100=y
   CONFIG_FREERTOS_HZ=1000
   CONFIG_COMPILER_OPTIMIZATION_PERF=y
   ```

   На проверенной плате flash 32 MB; выбирайте размер flash по своей плате.
   Если `sdkconfig` уже существует, defaults не заменяют его значения.
   Проверьте `menuconfig` и итоговый `sdkconfig`: для проверенного профиля
   `REV_MIN_FULL=100`, `REV_MAX_FULL=199`, tick rate 1000 Hz. Не обходите
   несовместимую ревизию через `esptool --force`.

5. Вызовите `p4_uac2_init()` один раз. Затем передавайте PCM в существующей
   SPI task при поступлении реального блока Seed3, как показано ниже.
6. В терминале ESP-IDF 5.5.5 соберите приложение:

   ```powershell
   idf.py -DIDF_TARGET=esp32p4 build
   ```

Или подключите папку без копирования: добавьте её в `EXTRA_COMPONENT_DIRS`
до `include(.../project.cmake)`. В одном проекте выбирайте **одну** копию
компонента, чтобы не собирать незаметно другой экземпляр.

## Вызовы из существующего SPI transport

```c
#include "p4_uac2_stream.h"

// Один раз при запуске приложения:
ESP_ERROR_CHECK(p4_uac2_init());

// В task после получения и проверки очередного блока Seed3.
// count = 32 stereo frames = 64 int32_t = 256 bytes на направление.
// from_seed и to_seed содержат L,R,L,R,...; low 8 bits каждого int32_t == 0.
(void)p4_uac2_capture_write(from_seed, 32);
(void)p4_uac2_playback_read(to_seed, 32);
// Передайте to_seed своей существующей следующей SPI-транзакцией.
```

Это точки интеграции, а не новый SPI-протокол. Не меняйте обработку READY,
CRC/sequence, порядок транзакций и DMA-буферы работающего SPI-драйвера.
`capture_write` нужно вызывать **даже когда Windows не записывает звук**:
он измеряет часы Seed3 для обратной связи playback. Возврат 0 при закрытом
capture нормален. Повторно передавать отвергнутый блок нельзя: это повторно
учтёт его в измерении частоты.

Не заменяйте события Seed3 таймером P4 на 48 kHz/1500 Hz. USB feedback
должен отражать реальное поступление/потребление аудио, а не часы ESP32.
Если данные приходят из ISR, сначала разбудите свою task; публичный API
этой библиотеки не предназначен для вызова из ISR.

## Что компонент создаёт сам

- USB task на core 0, priority 12, stack 6144 bytes; приложение не вызывает
  `tud_task()` и не создаёт второй USB-поток.
- Внутренние SPSC rings по 512 stereo frames, DMA-слоты с alignment 64 bytes.
- Prefill, пакетизацию, обработку USB control, explicit feedback и recovery.
- Собственные дескрипторы и TinyUSB callbacks. Для нового composite-устройства
  потребуется отдельная переработка, а не добавление второго владельца callbacks.

При `init` FreeRTOS выделяет память для task. В рабочем data API и USB ISR
выделения памяти нет. Освобождения/deinit/reinit API в этой версии нет.
Выводить диагностику нужно из отдельной низкоприоритетной task, не из
USB task, не из ISR и не при каждой SPI-транзакции.

## Проверка переносимости без перепрошивки

Из папки библиотеки в терминале ESP-IDF:

```powershell
python tests/verify_package.py
cd examples/compile_check
idf.py -DIDF_TARGET=esp32p4 build
```

`compile_check` проверяет компиляцию/линковку всего компонента и его API.
В нём нет SPI-драйвера и реального источника часов: **не прошивайте этот
пример вместо рабочей прошивки связки**. Компонент — библиотека, не готовый BIN.

Офлайн-вариант, если точная зависимость уже установлена в другом проекте:

```powershell
$env:IDF_COMPONENT_MANAGER = '0'
idf.py -DIDF_TARGET=esp32p4 '-DP4_UAC2_LOCAL_TINYUSB_DIR=C:/path/to/espressif__tinyusb' build
Remove-Item Env:IDF_COMPONENT_MANAGER
```

Этот параметр предусмотрен только в CMake примера. Укажите каталог именно
`espressif__tinyusb` версии 0.19.0~3. Отключение component manager означает,
что выбор точных версий IDF/TinyUSB — ответственность вызывающего;
проверка хеша DCD при этом остаётся включённой.

Отдельная проверка патча (из корня библиотеки):

```powershell
python tests/verify_package.py --dcd 'C:/path/to/espressif__tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c'
```

Она проверяет одинаковый результат для LF/CRLF и отказ при неизвестном
исходнике DCD. Для сравнения с исходной рабочей копией добавьте
`--reference 'C:/path/to/project/components/p4_uac2_stream'`.
SHA-256 нормализует только окончания строк; содержание файлов не меняется.

## Типичные проблемы

| Симптом | Что проверить |
|---|---|
| Windows Code 10 | Версии, отсутствие второго USB-стека, сохранность дескрипторов и реальный поток часов Seed3; не менять PID наугад |
| Видно устройство, но нет записи | `initialization_state`, `capture_active`, рост `capture_source_frames`; capture ждёт 96 frames |
| Playback расходится по ring fill | Вызовы write/read в темпе Seed3, feedback, USB HS; не ограничивать write флагом capture_active |
| Щелчки при логировании | Убрать printf/log из USB task и горячего SPI-пути; смотреть дельты потерь |
| Ошибка SHA DWC2 при CMake | Установить точную версию TinyUSB; не убирать проверку хеша |
| Firmware требует rev3.x | Исправить sdkconfig и пересобрать для rev1.x; не использовать --force |

Публичные функции и точные единицы счётчиков: [API](API_RU.md).
Границы аппаратной проверки: [отчёт](TEST_REPORT_RU.md).
