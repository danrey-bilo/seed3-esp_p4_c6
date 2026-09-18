# p4_uac2_stream v0.2.1 — PCM24-only

Активный ESP-IDF 5.5.5 компонент: ESP32-P4 rev1.x, USB HS UAC2,
2 capture + 2 playback, только PCM24, 44,1/48/88,2/96 кГц.
USB: signed packed little-endian, 3 байта/отсчёт, 6 байт/стереокадр.
HS bInterval=3 (0,5 мс), максимум 49 кадров = 294 байта.
DMA: internal SRAM, выравнивание 64 байта, stride 320 байт.
API: interleaved int32_t, PCM24 left-aligned. Seed задаёт часы;
приложение подтверждает их через `p4_uac2_source_rate()` после проверки SPI блока.

Документация:

- [Текущий профиль и испытания](../../../docs/PCM24_ONLY_RU.md)
- [Сохранённый снимок v0.2](../../../libraries/p4_uac2_stream_v2/README_RU.md)
- [API](../../../libraries/p4_uac2_stream_v2/docs/API_RU.md)
- [Интеграция и восстановление](../../../libraries/p4_uac2_stream_v2/docs/INTEGRATION_RU.md)
- [Прежние испытания v0.2](../../../libraries/p4_uac2_stream_v2/docs/TEST_REPORT_RU.md)

Снимок `libraries/p4_uac2_stream_v2` сохранён без изменений: он объявляет
16/24-in-32 и теперь **отличается** от этого активного компонента.
API/механизмы восстановления совместимы; дескрипторы и упаковка отличаются.
USB alt 0 закрыт, alt 1 — только 24 бита; alt 2 больше нет.
VID/PID/serial сохранены, bcdDevice=0x0201 обозначает новую ревизию форматов.
Упаковка и распаковка выполняются в USB task, не в ISR; SPSC, prefill,
DMA и восстановление endpoint сохранены. Пример внутреннего представления:
`int32_t 0x12345600` ↔ USB bytes `56 34 12`; `0x80000000` ↔ `00 00 80`.
В watchdog устранена гонка tick/completion: время читается под USB spinlock,
чтобы новая IRQ-метка не выглядела как истёкший unsigned timeout.

SPI layout/CRC сохранены; runtime rate требует новой прошивки Seed.
Прежняя fixed-48k версия сохранена в `libraries/p4_uac2_stream`.
