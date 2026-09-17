# API p4_uac2_stream v0.1.0

Публичный заголовок: [p4_uac2_stream.h](../include/p4_uac2_stream.h).
Пример использования: [compile_check](../examples/compile_check/main/main.c).

## Формат и потоки

Фиксированный профиль: 2 capture + 2 playback, 48 000 Hz, signed PCM24
left-aligned в `int32_t`, порядок `L0,R0,L1,R1,...`. Младшие 8 бит нулевые.
Один **frame** — одна пара L/R, 8 bytes. Размер вызова `frames` — не число
samples и не число bytes. Блок 32 frames содержит 64 значения `int32_t`.

Нет преобразования float/PCM16/packed24 в API. Источник готовит корректный
PCM заранее. Capture данные не нормализует и не маскирует: неправильный
формат не будет исправлен библиотекой. Playback поддерживает USB mute/volume
в своей task; при 0 dB и без mute данные проходят без изменения масштаба.

Для capture ровно один producer, для playback ровно один consumer.
Обе роли может исполнять одна SPI task. Не вызывайте их одновременно
из нескольких task и не вызывайте из ISR. Инициализацию выполняйте один
раз последовательно до запуска обмена; это не потокобезопасный multi-init.

## Функции

### `esp_err_t p4_uac2_init(void)`

Создаёт USB task и возвращается, не ожидая enumeration или открытия потока.
`ESP_OK` означает, что task создана, **не** что устройство уже готово.
При повторной инициализации возвращается `ESP_ERR_INVALID_STATE`;
при невозможности создать task — `ESP_ERR_NO_MEM`.
Проверка внутренней DMA-памяти также может вернуть `ESP_ERR_INVALID_STATE`.

Смотрите `get_stats().initialization_state`:

| Значение | Смысл |
|---|---|
| 0 | Не инициализировано / создание task не удалось |
| 1 | Асинхронный запуск |
| 2 | USB stack запущен, но хост может быть не подключён |
| 3 | Ошибка self-test/PHY/USB initialization |

Инициализация выделяет память для task; остальные data API не выделяют
память. Публичных `deinit`, reset stats и изменения профиля нет.

### `size_t p4_uac2_capture_write(const int32_t *pcm, size_t frames)`

Копирует в capture ring до `frames` stereo frames, возвращает принятое число.
При заполненном ring возможна частичная запись; новые лишние frames
отбрасываются и попадают в `capture_overrun_frames`. Старые непрочитанные
frames не перезаписываются.

Вызывайте один раз для каждого реального блока Seed3, включая закрытый
capture. **Все** корректно переданные `frames` учитываются в clock tracking,
независимо от возвращённого числа. При закрытом capture функция возвращает
0 без записи в ring. Не повторяйте rejected frames и не пропускайте вызов
по условию `capture_active()` — иначе feedback будет измерять неверную частоту.

При `pcm == NULL`, `frames == 0` или `frames > 512` возвращает 0 без учёта
часов и без чтения буфера. После возврата исходный буфер можно использовать
снова: библиотека не хранит указатель на него.

### `size_t p4_uac2_playback_read(int32_t *pcm, size_t frames)`

Валидный вызов заполняет весь выходной буфер. Возвращает число реальных
frames из playback ring. Текущая реализация выдаёт либо полный запрос,
либо 0 и тишину: закрытый поток, смена эпохи, ожидание prefill, underrun.
Вызывайте в темпе потребления Seed3; буфер сразу готов для своего SPI TX.

После открытия/восстановления ждёт 96 frames. Если после старта данных
меньше запроса, увеличивает `playback_underruns` и возвращается в prefill.
Первые нули до prefill и закрытый поток в underrun не записываются.

При `pcm == NULL`, `frames == 0` или `frames > 512` возвращает 0 **без
изменения буфера**. Обещание zero-fill относится только к валидному вызову.
Хотя допустимо до 512 frames, для штатного стенда используйте блоки 32;
большие запросы изменяют задержку и поведение prefill и не квалифицированы.

### `bool p4_uac2_capture_active(void)` / `bool p4_uac2_playback_active(void)`

Текущий флаг активного streaming alternate setting (alt 1, не suspended).
Это не гарантия USB completion или наличия реальных samples. При закрытии,
сбросе или suspend флаг снимается. Обычные гонки открытия/закрытия возможны:
после чтения флага следующее чтение данных всё равно может вернуть 0.

### `void p4_uac2_get_stats(p4_uac2_stats_t *out)`

Записывает диагностический снимок; `NULL` допустим, ничего не делает.
Поля читаются независимо атомарно, **не** под общим lock. Не проверяйте
равенство суммы соседних полей до одного frame в момент работы.
Числовые накопительные поля — `uint32_t` modulo 2^32.
Для интервала используйте `uint32_t delta = after - before`.
Рекомендуется отдельная low-priority task и редкий вывод, например раз в секунду.

## Основные счётчики

| Поля | Единицы / интерпретация |
|---|---|
| `capture_source_frames` | Все валидные frames API, в том числе при закрытом capture; основа часов |
| `capture_completed_frames`, `capture_packets` | Успешно завершённые USB IN frames / packets, включая вынужденную тишину |
| `capture_short_packets`, `capture_long_packets` | Успешные пакеты 47 / 49 frames; нормальная коррекция частоты |
| `capture_silence_frames` | Frames вынужденной тишины в активном capture; ошибка качества |
| `capture_overrun_frames` | Новые frames, не поместившиеся в capture ring |
| `capture_discard_frames` | Очистки ring при закрытии/восстановлении, не исчерпывающий счётчик всех потерь |
| `playback_received_frames`, `playback_consumed_frames` | Получено через USB / реально выдано приложению |
| `playback_short_packets`, `playback_long_packets` | Полученные пакеты меньше / больше 384 bytes |
| `playback_underruns` | Число событий нехватки после старта; не frames и не underrun Seed3 DAC |
| `playback_overrun_frames`, `playback_bad_packets` | Отброшенные frames / malformed USB packets |
| `*_incomplete` | Неуспешные завершения соответствующего endpoint |
| `endpoint_recoveries` | Попытки watchdog recovery; общий, не разделённый по endpoint счётчик |
| `controller_faults`, `controller_restarts` | Обнаруженные аппаратные fault / выполненные controller restart |
| `capture_opens`, `playback_opens` | Принятые SET_INTERFACE alt 1; не обязательно уникальные сессии приложения |
| `usb_resets`, `usb_suspends` | Вызовы обработки reset / suspend; reset также учитывает программную очистку |
| `feedback_packets`, `feedback_16_16` | Успешные feedback packets / последнее рассчитанное wire-значение Q16.16 frames за OUT packet, номинально 48 |
| `control_requests`, `control_stalls` | Control SETUP-запросы класса / отвергнутые обработчиком |
| `capture_fill`, `playback_fill` | Текущее заполнение SPSC ring в stereo frames |
| `capture_queued_frames` | Capture frames в готовых/DMA slots отдельно от ring |
| `capture_min_fill`, `capture_max_fill` | Наблюдавшаяся суммарная capture occupancy ring + slots |
| `playback_min_fill`, `playback_max_fill` | Наблюдавшаяся occupancy playback ring |
| `*_max_gap_uframes` | Максимум между успешными completion; в HS единица 125 µs, 8 = 1 ms |
| `mounted`, `high_speed` | Состояние подключения / согласованная скорость HS |

Часть incomplete/recovery возникает при пробных открытиях Windows без
последующего чтения ISO. Оценивайте их вместе с приростом потерь, реальной
записью и завершениями playback, а не как единственный критерий стабильности.
Счётчики P4 не измеряют потери в отдельном буфере/ЦАП прошивки Seed3.

Подробнее: [архитектура](../README_RU.md), [подключение](INTEGRATION_RU.md),
[проверенный профиль и ограничения](TEST_REPORT_RU.md).
