# Интеграция, частоты и восстановление

## Сборка P4

1. Скопируйте пакет в `components/p4_uac2_stream`.
2. В CMake приложения добавьте `REQUIRES p4_uac2_stream`.
3. Уберите другого владельца TinyUSB/UAC. Не включайте одновременно
   `espressif__usb_device_uac`.
4. Используйте ESP-IDF **5.5.5**, target esp32p4, min chip rev 100,
   max chip rev 199. Не прошивайте v3.x image в rev1.x и не применяйте `--force`.
5. Сохраните `CFG_TUD_DWC2_DMA_ENABLE=1` и версии из `idf_component.yml`.

Приватный DWC2 patch генерируется в build directory. Он не редактирует
ESP-IDF или `managed_components`. При неизвестном upstream SHA сборка
останавливается; подменять ожидаемый hash без проверки кода нельзя.
CRLF и LF исходники дают одинаковый результат.

## Протокол приложения

USB-компонент не знает GPIO/SPI. В штатном приложении P4 помещает
`p4_uac2_requested_rate()` в существующий SPI `sample_rate` header.
Seed завершает текущий DMA, останавливает SAI, перенастраивает PLL3/SAI,
очищает старые аудиобуферы и подтверждает частоту следующим TX header.
Первый пакет содержит `SPI_AUDIO_FLAG_SESSION_START`. CRC и размер пакета
288 bytes / 32 frames сохраняются, SCLK поднят до проверяемых 20 MHz.
CRC-32 ускорен byte-table без изменения полинома или результата.

P4 проверяет header и CRC до вызова `source_rate` / `capture_write`.
Не используйте rate из невалидного пакета. Один READY-edge разрешает
ровно одну SPI-транзакцию, даже если READY ещё высок после completion.

Clock adapter Seed3 находится в основном проекте `Seed3/src/seed_audio_clock.*`.
Он использует публичный libDaisy API и HAL RCC; общая установленная libDaisy
не изменяется. PLL3 также может питать другие периферийные блоки STM32:
если приложение использует их, смену часов необходимо согласовать с ними.
Для DSP используйте фактические 44100/88200, а не округлённый libDaisy enum.

При переключении частоты закройте capture **и** playback. Запрос другой
частоты при активном втором потоке отклоняется: независимых clock domains
и скрытого ресемплинга нет.

## Автовосстановление Seed

Зависший SPI DMA имеет программный timeout 500 мс; независимый IWDG
контролирует выполнение foreground (~2 с, точность LSI не гарантируется).
Перезапуск MCU очищает HAL/DMA и создаёт новую сессию, после чего P4 повторно
согласует текущую частоту. Это восстановление со слышимым перерывом,
не обещание непрерывного звука при отключении платы.

UART RX переармируется после ошибки. Диагностические команды P4:

```text
buffer 1
buffer 2
buffer 4
seed reset
seed boot
seed stats
spi pause 1200
```

`spi pause` — только закрытые USB streams, управляемый тест timeout.
`seed stats` — только закрытые streams: последние измеренные CPU busy/audio
в промилле и счётчики Seed. Сама печать временно задерживает foreground,
поэтому для чистого acceptance её выполняют после записи, не во время.
`seed boot` переводит работающий Seed в ROM DFU для прошивки. Если прошивка
не запускается или UART недоступен, физические BOOT+RESET остаются аварийным
способом входа в DFU. Автовосстановление не исправляет питание/обрыв провода.

В Seed настроены приоритеты завершения DMA: TX=0, RX=1, SPI EOT=2.
Это предотвращает гонку libDaisy, когда SPI EOT удаляет DMA owner до очистки
оставшегося TX IRQ. UART DMA SRAM очищается перед запуском приёмника, чтобы
после software reset не повторилась сохранённая команда `@BOOT`.
Foreground спит в race-free WFI, вместо постоянного busy-polling.
P4 оценивает CPU через runtime counters четырёх постоянных задач. Не вызывайте
`uxTaskGetSystemState()` в работающем low-latency тракте: этот API дополнительно
сканирует стеки под scheduler lock. Используйте `vTaskGetInfo` с
`xGetFreeStackSpace = pdFALSE`, без обхода всех задач. В нашей проверке замена
этой диагностики убрала периодические задержки USB completion.
Для семейства 44,1 kHz clock adapter явно устанавливает MCKDIV=4/2:
вычисление HAL по ближайшему enum 48/96 даёт неправильный делитель.

## ПК

Тестер основного проекта `tools/WasapiCapture` принимает:

Сборка на Windows с .NET 10 SDK:
`dotnet build tools/WasapiCapture/WasapiCapture.csproj -c Release`.
EXE создаётся в `tools/WasapiCapture/bin/Release/net10.0-windows`.

```text
WasapiCapture.exe --probe CAPTURE_GUID RENDER_GUID
WasapiCapture.exe CAPTURE_GUID out.wav 60 exclusive RENDER_GUID --rate 48000 --bits 24 --period min
```

Для проверенного на этом ПК длительного режима замените `--period min` на
`--period 10`. Тестер сохраняет весь PCM в RAM до остановки: около 231 MB
для 301 s / 96k / PCM24-in-32; это диагностический инструмент, не DAW.

Не меняет default endpoint, громкость или драйверы. Для минимальной задержки
приложение должно поддерживать WASAPI exclusive. Shared mode добавляет
буферы аудиодвижка Windows. ASIO4ALL не требуется для этой реализации.

Для воспроизводимости `tools/run_multirate_matrix.py` меняет семейства частот,
разрядность и capture/duplex; `tools/run_uac2_acceptance.py` выполняет 60-секундную
запись, 100 open/close и пятиминутный duplex. Отдельно считывайте UART counters.

В exclusive event capture используйте `GetBuffer`/`ReleaseBuffer` один раз на
событие. `GetNextPacketSize` предназначен для shared mode, не для exclusive.
Не записывайте WAV на диск, удерживая буфер WASAPI: тестер заранее выделяет
память и сохраняет запись после Stop. Он проверяет devicePosition, flags,
число кадров, уровни и непрерывность фазы тестовых синусов. Одного RMS и
поиска нулей недостаточно: потерянный целый буфер может не содержать тишины.

Один раз после первого capture IN completion компонент отбрасывает избыток,
накопленный между SET_INTERFACE и началом опроса хостом. Это учитывается в
`capture_discard_frames` и ограничивает начальную задержку; в установившемся
потоке PCM не удаляется для подстройки частоты. При проверке измеряйте отдельно
startup и steady-state counters, не выдавая startup recovery за steady-state PASS.

После обновления форматов Windows может создать новые endpoint GUID даже
при неизменных PID/serial. Получайте актуальные ID, не используйте старые вслепую.

## Проводка и безопасность

Используйте уже проверенные SPI+READY+UART соединения основного проекта.
Питание плат раздельное, земля общая, 3V3 не соединять. J1 USB-A HS подключается
существующим data-only адаптером с разрывом VBUS. Обычный USB-A↔USB-A не использовать.

Официальные источники:

- [Microsoft UAC2](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers).
- [Microsoft low-latency audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio).
- [IAudioCaptureClient::GetNextPacketSize — shared mode only](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getnextpacketsize).
- [Espressif SPI Master](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/spi_master.html).
