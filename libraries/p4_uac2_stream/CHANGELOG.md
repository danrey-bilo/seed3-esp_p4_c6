# Изменения

## 0.1.0 — 2026-09-17

- Выделен переносимый снимок компонента из работающего проекта P4.
- USB HS UAC2 2 IN + 2 OUT, 48 kHz, PCM24 в 32-bit subslot, Seed-clocked.
- SPSC rings, prefill, internal DMA buffers, completion-driven ISO scheduling,
  explicit feedback, DWC2 recovery и диагностический API.
- Зафиксированы ESP-IDF 5.5.5 и TinyUSB 0.19.0~3; DWC2-патч проверяет исходник
  по SHA-256 и не изменяет установленный SDK/managed dependency.
- Добавлены русская документация, пример compile/link check,
  проверка контрольных сумм и DWC2 LF/CRLF/unknown-version tests.
- В build-time скрипте исправлена нормализация CRLF перед SHA-проверкой:
  исходная версия принимала LF, но ошибочно отвергала идентичный CRLF.
  Генерируемый C-драйвер не изменён; это проверяется сравнением с оригиналом.
- Источник снимка прошёл 60 с capture, 100 open/close и 5 мин duplex.
  Оговорки испытаний сохранены в [отчёте](docs/TEST_REPORT_RU.md).
- При упаковке USB/SPI алгоритмы, PID и прошивки подключённых плат не менялись.
