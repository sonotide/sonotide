# Публичный API

## Общие соглашения

Почти все операции в Sonotide возвращают `result<T>`. Это значит, что у каждого вызова есть либо значение, либо структурированная ошибка с категорией, кодом и текстом для человека.

```cpp
auto runtime_result = sonotide::runtime::create();
if (!runtime_result) {
    const auto& err = runtime_result.error();
    // логирование, телеметрия или показ сообщения пользователю
}
```

Для потока это особенно удобно: приложение видит не только факт сбоя, но и то, на каком этапе он случился.

## Точка входа

`sonotide::runtime` — основной объект пакета. Через него открываются все основные сценарии:

- `runtime::create(runtime_options)`
- `runtime::enumerate_devices(device_direction)`
- `runtime::default_device(device_direction, device_role)`
- `runtime::open_render_stream(config, callback)`
- `runtime::open_capture_stream(config, callback)`
- `runtime::open_loopback_stream(config, callback)`
- `runtime::open_playback_session(config)`

`runtime_options` позволяет управлять тем, что инициализируется при создании `runtime`:

- `initialize_media_foundation`
- `enable_mmcss`

## Устройства

Sonotide различает два уровня выбора:

- `device_direction` задаёт направление: вывод или захват;
- `device_role` выбирает устройство по умолчанию по роли ОС;
- `device_selector` может ссылаться либо на устройство по умолчанию, либо на явный `device_id`.

`device_info` возвращает стабильный идентификатор, человекочитаемое имя, состояние endpoint и флаги устройства по умолчанию. Этого достаточно, чтобы построить список устройств в интерфейсе, не лезя в COM-объекты.

Пример:

```cpp
auto devices_result = audio.enumerate_devices(sonotide::device_direction::render);
if (devices_result) {
    for (const auto& device : devices_result.value()) {
        std::printf("%s | %s\n", device.id.c_str(), device.friendly_name.c_str());
    }
}
```

## Форматы и конфигурация

`format_request` описывает предпочтения, а не жёсткий контракт. В нём можно задать:

- предпочитаемый тип сэмпла;
- предпочитаемую частоту дискретизации;
- предпочитаемое число каналов;
- чередующуюся раскладку каналов;
- разрешён ли откат к формату микшера устройства.

`audio_format` хранит уже согласованный результат.

Потоковая конфигурация строится вокруг трёх типов:

- `render_stream_config`
- `capture_stream_config`
- `loopback_stream_config`

В них есть выбор устройства, режим совместного использования, режим callback, формат и тайминги. У потоков вывода и захвата отдельно есть поведение, связанное с потерей устройства и заполнением тишиной.

## Потоки

У всех фасадов потоков один и тот же жизненный цикл:

- `is_open()`
- `start()`
- `stop()`
- `reset()`
- `close()`
- `status()`

`status()` возвращает `stream_status`, где есть:

- текущее состояние жизненного цикла;
- запрошенный формат;
- согласованный формат;
- статистика вызовов callback и обработанных кадров;
- флаг `device_lost`.

Минимальный callback для вывода выглядит так:

```cpp
class silence_callback final : public sonotide::render_callback {
public:
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
        return sonotide::result<void>::success();
    }
};
```

А callback захвата получает неизменяемое представление буфера:

```cpp
class capture_sink final : public sonotide::capture_callback {
public:
    sonotide::result<void> on_capture(
        sonotide::const_audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        // buffer.bytes можно читать, но нельзя изменять.
        return sonotide::result<void>::success();
    }
};
```

## Callback правила

- Callback выполняются на рабочем потоке.
- Callback должен быть коротким и предсказуемым.
- Ошибку можно вернуть через `result<void>::failure(...)`.
- `on_stream_error()` используется как асинхронное уведомление, если поток уже столкнулся с проблемой.

## Сессия воспроизведения

`playback_session` — это высокоуровневый API для типичного сценария воспроизведения источника.

Через него доступны:

- `load(std::string source_uri)`
- `play()`
- `pause()`
- `seek_to(std::int64_t position_ms)`
- `set_volume_percent(int volume_percent)`
- `set_equalizer_enabled(bool enabled)`
- `select_equalizer_preset(equalizer_preset_id preset_id)`
- `set_equalizer_band_gain(std::size_t band_index, float gain_db)`
- `add_equalizer_band(float center_frequency_hz, float gain_db = 0.0F)`
- `remove_equalizer_band(std::size_t band_index)`
- `set_equalizer_band_frequency(std::size_t band_index, float center_frequency_hz)`
- `reset_equalizer()`
- `set_equalizer_output_gain(float output_gain_db)`
- `apply_equalizer_state(const equalizer_state& state)`
- `list_output_devices()`
- `select_output_device(std::string device_id)`
- `state()`
- `equalizer_state()`
- `equalizer_band_frequency_range(std::size_t band_index) const`
- `close()`

Это удобный слой для приложения, которому нужно не только отправлять PCM в поток вывода, но и держать транспортное состояние, громкость, устройство вывода и встроенный EQ в одном месте.

Пример использования:

```cpp
auto session_result = audio.open_playback_session();
if (!session_result) {
    return;
}

sonotide::playback_session session = std::move(session_result.value());

if (auto load_result = session.load(R"(C:\Windows\Media\notify.wav)"); !load_result) {
    return;
}

session.set_volume_percent(80);
session.select_equalizer_preset(sonotide::equalizer_preset_id::rock);
session.play();
```

Для более гибкого эквалайзера `playback_session` теперь позволяет:

- держать от `0` до `10` полос;
- добавлять и удалять полосы во время работы;
- менять не только `gain_db`, но и `center_frequency_hz`;
- брать готовые раскладки через `make_default_equalizer_bands(...)`;
- запрашивать глобальные ограничения и допустимый диапазон перемещения конкретной полосы.

## Ошибки

`error` содержит:

- категорию;
- код;
- сообщение;
- имя операции;
- нативный код, если он есть;
- флаг `recoverable`.

Это позволяет приложению вести нормальный журнал ошибок, а не раскладывать всё по строковым сообщениям и догадкам.
