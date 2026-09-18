# API v0.2

Все вызовы — из task, не из ISR. Синглтон, ровно один producer capture и один
consumer playback. Входной и выходной PCM: `L0,R0,L1,R1,...`, signed int32,
значащие старшие 24 бита, младшие 8 нулевые. Counts всегда в stereo frames.
Буфер вызывающего можно переиспользовать после возврата. API не блокирует.
`init` однократно создаёт task и ресурсы платформы; отсутствие динамического
выделения памяти гарантируется для streaming API/USB ISR, не для инициализации.

| Вызов | Контракт |
|---|---|
| `p4_uac2_init()` | Один раз; запускает USB асинхронно. Статус 2 = ready, 3 = failed. |
| `p4_uac2_requested_rate()` | Частота, которую хост запросил у общего clock entity. |
| `p4_uac2_source_rate(actual, new_session)` | Подтверждение источника перед каждым валидным capture block. `new_session` только на первом блоке новой сессии/частоты. |
| `p4_uac2_capture_write(pcm, frames)` | 1..512 frames; возвращает принятое число. Вызывать по реальному темпу источника даже при закрытом capture для feedback. Отброшенные frames не повторять. |
| `p4_uac2_playback_read(pcm, frames)` | Возвращает число реальных frames, остаток всегда нули. При startup ждёт prefill. Вызывать с темпом потребления Seed. |
| `p4_uac2_capture_active()` / `playback_active()` | Включённый streaming alt, не гарантия наличия сигнала. |
| `p4_uac2_set_buffer_ms(ms)` | 1, 2 или 4; только при закрытых потоках. Иначе `ESP_ERR_INVALID_STATE`. Это запас устройства, не WASAPI period. |
| `p4_uac2_get_stats(&s)` | Атомарные 32-битные поля; весь snapshot не транзакционный. Счётчики modulo 2^32. |

Постоянные `P4_UAC2_RATE` / `P4_UAC2_PREFILL` сохранены для совместимости кода,
но не отражают текущий режим. Используйте `sample_rate`, `source_sample_rate`,
`capture_bits`, `playback_bits`, `prefill_frames`, `buffer_ms` из stats.

`source_rate` и capture producer должны быть одним потоком. Подтверждать
частоту до того, как Seed действительно перестроил часы, нельзя. Новую сессию
нужно сообщать и после перезагрузки Seed на той же частоте.

## Диагностика

- `capture_source_frames`: весь валидный источник, включая закрытый capture.
- `capture_completed_frames`, `capture_packets`: USB completion, не постановка.
- `capture_silence_frames`, `capture_overrun_frames`: потери; должны не расти
  в установившемся приёмочном прогоне.
- `playback_underruns`, `playback_overrun_frames`, `playback_bad_packets`:
  отдельно проверить под одновременной нагрузкой capture/playback.
- `capture_incomplete`, `playback_incomplete`, `feedback_incomplete`:
  неудачные ISO попытки, в том числе при открытии/закрытии. Не равны числу
  пропущенных аудиоотсчётов; проверяйте WAV и USB completion gap.
- `endpoint_recoveries`, `controller_restarts`, `controller_faults`: восстановление.
- `rate_changes`, `source_restarts`: переключения и новые сессии источника.
- `feedback_16_16`: совместимое с Windows Q16.16 frames **на OUT-пакет**,
  при HS bInterval=3 номинально `rate / 2000`, а не `rate / 8000`.
- `capture_max_gap_uframes`, `playback_max_gap_uframes`: единицы HS 125 мкс.

Сравнивайте дельты счётчиков до/после теста; startup и намеренные fault-injection
прогоны не смешивайте с чистым приёмочным интервалом. Логируйте из отдельной
низкоприоритетной task, не из USB control/audio callback.
