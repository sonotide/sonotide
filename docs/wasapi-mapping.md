# Соответствие WASAPI и Sonotide

Этот документ нужен, чтобы без гаданий сопоставить старый монолитный backend из `windows_streaming_audio_backend.cpp` и слои Sonotide. В старом коде в одном месте были собраны и перечисление устройств, и открытие `IAudioClient`, и потоковая логика, и часть policy. В Sonotide эти обязанности разнесены по более узким объектам, и это именно то, что делает фреймворк удобным для повторного использования.

## Карта соответствия

| Что делал старый backend | Нативный WASAPI/Windows слой | Что делает Sonotide | Комментарий |
| --- | --- | --- | --- |
| Перечисление audio endpoints | `IMMDeviceEnumerator::EnumAudioEndpoints` | `runtime::enumerate_devices(device_direction)` | Возвращает нормализованный список `device_info`, а не COM-объекты. |
| Получение устройства по умолчанию | `IMMDeviceEnumerator::GetDefaultAudioEndpoint` | `runtime::default_device(direction, role)` | Роль по умолчанию совпадает с мультимедийным сценарием, как в основном плеере. |
| Открытие потока вывода | `IMMDevice::Activate(IAudioClient)` | `runtime::open_render_stream(...)` | Поток владеет всем жизненным циклом клиента и рабочим потоком. |
| Подбор формата для вывода | `IAudioClient::GetMixFormat` и `IsFormatSupported` | `render_stream.status().negotiated_format` | Sonotide хранит не только согласованный формат, но и снимок состояния потока. |
| Работа render buffer | `IAudioRenderClient` | `render_callback::on_render(...)` | Callback получает `audio_buffer_view`, а не сырой COM-буфер. |
| Открытие потока захвата | `IAudioClient::Initialize(...)` + `IAudioCaptureClient` | `runtime::open_capture_stream(...)` | Поток захвата устроен так же строго, как поток вывода, без смешивания ролей. |
| Loopback capture | `AUDCLNT_STREAMFLAGS_LOOPBACK` + `IAudioCaptureClient` | `runtime::open_loopback_stream(...)` | Для клиента это отдельный stream type, а не особый режим внутри render-кода. |
| Состояние и диагностика | Ручное хранение полей и COM HRESULT | `stream_status`, `error`, `result<T>` | Нативный код ошибки сохраняется, но наружу выходят уже доменные типы Sonotide. |
| Реакция на потерю устройства | Проверка `HRESULT` и собственные ветки восстановления | `stream_status.device_lost` + ошибки потока | Флаг потери устройства виден на уровне API, а не спрятан в приватной логике backend-а. |

## Что намеренно не переносится в WASAPI-слой

Sonotide не пытается быть копией старого backend-а, где в одном классе были и транспорт, и декодирование, и логика интерфейса. В фреймворке остаются только те вещи, которые действительно переиспользуются:

- перечисление и выбор устройств;
- потоки вывода, захвата и loopback;
- согласование формата;
- жизненный цикл потоков;
- состояние, ошибки и диагностика.

А вот эти вещи находятся выше фреймворка или уже в отдельном верхнем слое:

- загрузка источника и декодирование;
- transport `load / play / pause / seek`;
- состояние интерфейса плеера;
- persistence настроек;
- любые продуктовые правила, завязанные на конкретное приложение.

## Как это выглядит в коде

Базовый сценарий в Sonotide начинается не с COM-объектов, а с `runtime`:

```cpp
auto runtime_result = sonotide::runtime::create();
if (!runtime_result) {
    std::cerr << runtime_result.error().message << '\n';
    return 1;
}

sonotide::runtime audio_runtime = std::move(runtime_result.value());
auto devices_result = audio_runtime.enumerate_devices(sonotide::device_direction::render);
if (!devices_result) {
    std::cerr << devices_result.error().message << '\n';
    return 1;
}
```

Если нужен только низкий уровень вывода, дальше открывается `render_stream` и под него пишется callback:

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

silence_callback callback;
sonotide::render_stream_config config;

auto stream_result = audio_runtime.open_render_stream(config, callback);
if (!stream_result) {
    std::cerr << stream_result.error().message << '\n';
    return 1;
}

auto start_result = stream_result.value().start();
if (!start_result) {
    std::cerr << start_result.error().message << '\n';
    return 1;
}
```

Если нужен более высокий уровень, Sonotide уже даёт и `playback_session`. Это отдельный слой поверх потока вывода, а не часть WASAPI-обёртки.

## Практический вывод

Старый backend и Sonotide покрывают одну предметную область, но на разных уровнях. Старый код был удобен для одного приложения, Sonotide удобен как библиотека, которую можно подключить и к плееру, и к сервису захвата, и к отдельному инструменту работы с устройствами.
