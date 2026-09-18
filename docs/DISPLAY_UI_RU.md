# Экран и телеметрия Seed3 / ESP32-P4

[← Главная](../README.md) · [Проект P4](../ESP32-P4-WIFI6-Touch-LCD-7B/README.md) · [Макет EEZ Studio](../ui/seed3_p4_ui/README_RU.md)

Текущая прошивка использует дисплей 1024×600 платы
ESP32-P4-WIFI6-Touch-LCD-7B. Дисплей запускается через BSP Waveshare 3.0.1 и
LVGL 9.5.0. Вся отрисовка выполняется отдельно от USB и SPI audio tasks.

## Что показывает главный экран

| Область | Значение |
| :--- | :--- |
| `SEED3 CPU` | Оценка загрузки аудиопроцессора Seed3, 0–100% |
| `INPUT / MICROPHONE` | Последняя частота открытия capture endpoint и его состояние |
| `OUTPUT / PLAYBACK` | Последняя частота открытия playback endpoint и его состояние |
| `IN 1`, `IN 2` | Peak уровни двух каналов ADC Seed3 |
| `OUT 1`, `OUT 2` | Peak уровни двух каналов USB playback |
| Нижняя строка | USB UAC2, buffer budget и счётчики SPI/CRC |

Состояние `READY` означает, что USB устройство смонтировано Windows, но
endpoint сейчас закрыт. `ACTIVE` появляется только при реально запущенном
потоке соответствующего направления. Поэтому вход и выход могут менять
состояние независимо.

## Почему на экране две частоты

Windows хранит формат capture и playback отдельно. Раньше интерфейс дважды
показывал общий `runtime_rate`, поэтому обе строки принимали последнее
полученное значение. Теперь компонент запоминает частоту в момент открытия
каждого streaming interface:

- `capture_sample_rate` — последнее открытие микрофона;
- `playback_sample_rate` — последнее открытие динамиков;
- `sample_rate` — текущая физическая частота Seed3.

Проверенный пример:

```text
format rate=44100 usb=96000/44100 seed=44100
```

Здесь Windows последовательно открыла вход на 96 кГц и выход на 44,1 кГц;
экран сохраняет оба значения. Seed3 имеет один физический аудиоклок, поэтому
одновременный full-duplex без ASRC должен использовать одинаковую частоту в
обоих направлениях. Интерфейс показывает настройки честно и не создаёт
скрытый ресемплинг.

## Путь телеметрии

CPU Seed3 кодируется в свободных старших битах существующего поля `status`
SPI-заголовка. Размер кадра, CRC и версия протокола не изменились. Peak levels
считаются P4 при уже выполняемой проверке PCM блока и публикуются атомарно.

Задача интерфейса работает примерно с частотой 30 Гц. В USB ISR нет LVGL,
логарифмов, выделения памяти, блокировок или пользовательских callbacks.
Это сохраняет приоритет аудиотранспорта и предсказуемую задержку.

## Исходники

- [`audio_dashboard.c`](../ESP32-P4-WIFI6-Touch-LCD-7B/main/audio_dashboard.c) — создание и обновление экрана;
- [`audio_dashboard.h`](../ESP32-P4-WIFI6-Touch-LCD-7B/main/audio_dashboard.h) — неблокирующий интерфейс телеметрии;
- [`p4_uac2_stream.h`](../ESP32-P4-WIFI6-Touch-LCD-7B/components/p4_uac2_stream/include/p4_uac2_stream.h) — статистика двух endpoints;
- [`spi_audio_protocol.h`](../protocol/spi_audio_protocol.h) — CPU telemetry в SPI status;
- [`seed3_p4_ui.eez-project`](../ui/seed3_p4_ui/seed3_p4_ui.eez-project) — редактируемый макет.

## Проверка

После сборки и прошивки P4 выполнены отдельные exclusive-тесты:

1. capture: PCM24 stereo, 96 кГц, 192 000 кадров за 2 секунды;
2. playback: PCM24 stereo, 44,1 кГц, минимальный период Windows;
3. итоговая телеметрия: `usb=96000/44100`;
4. CRC и sequence errors SPI: 0.

Для отдельного теста выхода диагностическая программа поддерживает:

```powershell
.\tools\WasapiCapture\bin\Release\net10.0-windows\WasapiCapture.exe `
  --packed24 --rate 44100 --bits 24 --period min `
  --render-only PLAYBACK_GUID 5 exclusive
```
