# Изменения

## 0.2.0 — 18.09.2026

- PCM16 и PCM24-in-32, 44,1/48/88,2/96 кГц, общий программируемый clock.
- HS 0,5 ms, фракционный packetizer, rate-dependent feedback/prefill.
- Подтверждение часов источника и смена сессии через неблокирующий API.
- Выбор запаса 1/2/4 мс, отдельно от размера Windows buffer.
- В приложении: Seed PLL adapter, автоматическое SPI/DMA recovery,
  неблокирующие сервисные команды, WASAPI minimum-period tester.
- SPI SCLK 20 MHz; CRC-32 ускорен совместимой byte-table без изменения wire layout.
- Исправлены приоритеты DMA TX/RX/EOT и очистка UART NOLOAD при soft reset Seed.
- CPU telemetry P4 не сканирует стеки задач в realtime тракте.
- Startup backlog capture ограничивается после начала реального IN polling,
  без длительного превышения номинального потока в минимальный буфер Windows.
- WASAPI exclusive recorder больше не вызывает shared-only GetNextPacketSize;
  во время записи нет дискового I/O, контролируется devicePosition.
- Анализатор v2 отклоняет также ненулевые разрывы фазы; ранние v1 PASS
  не считаются доказательством непрерывности.
- Минимум WASAPI округляется вверх до целого кадра; аппаратные результаты
  для минимального и более устойчивого буфера документируются раздельно.
- Снимок v0.1 не изменён. См. текущие ограничения в docs/TEST_REPORT_RU.md.
- Аппаратно проверены 60 s 48k capture, 100 open/close, 301 s 96k duplex
  с WASAPI exclusive 10 ms. Минимум 3 ms доступен, но не квалифицирован.
