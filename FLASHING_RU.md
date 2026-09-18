# Прошивка Seed3 и ESP32-P4 — multirate v0.2

Обновлено 18.09.2026. Обновляются **обе платы**. Между ними SPI + READY + UART,
не старая I2S/SAI2 схема. Профиль: 2×2, PCM16/24, 44,1/48/88,2/96 кГц.
Сверяйте образы по `firmware/SHA256SUMS.txt`.

Закройте аудиопотоки и serial monitor. Земля общая, 3V3 плат не соединять.
USB-A J1 P4 — только существующий data-only адаптер **с разрывом VBUS**,
не обычный USB-A↔USB-A. [Проводка](docs/wiring.md).
[Статус испытаний](libraries/p4_uac2_stream_v2/docs/TEST_REPORT_RU.md).

## 1. Seed3

Образ: `Seed3/firmware/Seed3P4SpiAudio.bin`, адрес `0x08000000`,
ROM DFU `0483:df11`, alternate 0. USB Seed должен быть подключён к ПК.

Если новая прошивка работает и UART соединён с P4, вход в DFU без кнопок:

```powershell
cd 'F:\Repos\seed3+esp_p4_c6'
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/audio_control.py --port COM6 --command 'seed boot' --seconds 2
cd Seed3
.\flash.cmd
```

Для первого обновления старой/неработающей прошивки: удерживая **BOOT**,
кратко нажмите **RESET**, отпустите BOOT и выполните `Seed3\flash.cmd`.
После успешной записи скрипт запускает программу сам; обычный дополнительный
RESET не нужен. Кнопки остаются аварийным способом.

Используется dfu-util **0.11** из ESP-IDF и корректный DFU suffix VID/PID.
Код 74 принимается скриптом только после успешного download **и** исчезновения
DFU после leave; другие ошибки не скрываются.

Пересборка:

```powershell
cd 'F:\Repos\seed3+esp_p4_c6\Seed3'
.\build.cmd
```

Нужны DaisyToolchain и libDaisy. Скрипт использует `Seed3/libDaisy` либо ignored
junction к `F:\Repos\seed3 audio usb\Seed3MonoUsbInput\libDaisy`.
Общая libDaisy не изменяется. `-Clean` очищает результаты сборки проекта Seed.
Тестовые синусы заменяются АЦП настройкой `kUseTestTones = false`
в `Seed3/src/main.cpp` с последующей пересборкой.

## 2. ESP32-P4-WIFI6-Touch-LCD-7B

Образ `firmware/ESP32P4_Seed3_SPI_UAC2_2x2_Merged.bin` записывается
**с offset 0x2000**, не 0; содержит загрузчик, partition table и приложение.
Target **ESP32-P4 rev1.x**, проверенная плата rev1.3. Не применять `--force`.

Разъём прошивки — **USB TO UART**, сейчас COM6. USB-A J1 — рабочее HS аудио.

```powershell
cd 'F:\Repos\seed3+esp_p4_c6\ESP32-P4-WIFI6-Touch-LCD-7B'
.\flash.cmd COM6
```

Замените COM6 своим портом, если он изменился. Используются ROM `--no-stub`
и 115200 baud для обхода прежнего зависания после `Running stub...`.
Если автоматический вход не сработал, используйте BOOT+RESET P4 и повторите.
Seed восстановит связь после возвращения P4.

Сборка в установленном ESP-IDF 5.5.5: `.\build.cmd`.
Адреса отдельных файлов: `bootloader.bin` → `0x2000`,
`partition-table.bin` → `0x8000`, `seed3_p4_spi_uac2.bin` → `0x10000`.
Не прошивайте старые `*Capture*` / `*SaiBridge*` образы от I2S.

## 3. Формат и буферы на ПК

В свойствах записи и воспроизведения → «Дополнительно» выберите одинаковую
частоту. В WASAPI exclusive её задаёт приложение. Перед сменой частоты
закройте **оба** потока. 24 значащих бита передаются в 32-битном контейнере.

Буфер приложения выбирается в DAW/WASAPI, а не настройкой разрядности Windows.
`--period min` в тестере округляет минимум драйвера вверх до целого кадра:
144 при 48k, 288 при 96k, 133 при 44,1k и 265 при 88,2k.
Минимум драйвера не гарантирует устойчивость на загруженном ПК;
результаты разных периодов приведены в отчёте. На этом ПК для длительной
работы проверены **10 мс** (`--period 10`), а на 3 мс найден пропуск 6 мс.

Запас прошивки, только при закрытых потоках:

```powershell
cd 'F:\Repos\seed3+esp_p4_c6'
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/audio_control.py --command 'buffer 1'
```

Варианты `buffer 1`, `buffer 2`, `buffer 4`; после reset P4 возвращается 1 мс.
При артефактах на загруженном ПК сначала увеличьте буфер приложения.

Timeout SPI ~500 мс или IWDG ~2 с могут автоматически перезапустить Seed,
очистить DMA и восстановить сессию. При отключении питания/провода непрерывный
звук невозможен; после возврата связи ручной RESET обычно не нужен.
