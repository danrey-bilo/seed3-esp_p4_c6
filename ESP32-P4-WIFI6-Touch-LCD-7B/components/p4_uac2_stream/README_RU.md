# p4_uac2_stream v0.2

Активный ESP-IDF 5.5.5 компонент: ESP32-P4 rev1.x, USB HS UAC2,
2 capture + 2 playback, PCM16/24, 44,1/48/88,2/96 кГц.
API: interleaved int32_t, PCM24 left-aligned. Seed задаёт часы;
приложение подтверждает их через `p4_uac2_source_rate()` после проверки SPI блока.

Документация и переносимый снимок:

- [Описание и буферы](../../../libraries/p4_uac2_stream_v2/README_RU.md)
- [API](../../../libraries/p4_uac2_stream_v2/docs/API_RU.md)
- [Интеграция и восстановление](../../../libraries/p4_uac2_stream_v2/docs/INTEGRATION_RU.md)
- [Аппаратные испытания](../../../libraries/p4_uac2_stream_v2/docs/TEST_REPORT_RU.md)

SPI layout/CRC сохранены; runtime rate требует новой прошивки Seed.
Прежняя fixed-48k версия сохранена в `libraries/p4_uac2_stream`.
