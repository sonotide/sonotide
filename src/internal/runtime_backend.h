#pragma once

#include <memory>
#include <vector>

#include "sonotide/device_info.h"
#include "sonotide/loopback_capture_stream.h"
#include "sonotide/render_stream.h"
#include "sonotide/result.h"
#include "sonotide/runtime.h"
#include "sonotide/stream_callback.h"
#include "sonotide/stream_config.h"
#include "sonotide/stream_status.h"
#include "sonotide/capture_stream.h"

namespace sonotide::detail {

/// Абстрактный дескриптор, который владеет рабочим потоком и жизненным циклом одного открытого потока.
class stream_handle {
public:
    /// Освобождает ресурсы, связанные с конкретным потоком.
    virtual ~stream_handle() = default;

    /// Запускает обработку потока.
    virtual result<void> start() = 0;
    /// Останавливает обработку потока без уничтожения дескриптора.
    virtual result<void> stop() = 0;
    /// Сбрасывает состояние потока обратно к базе prepared.
    virtual result<void> reset() = 0;
    /// Полностью закрывает дескриптор.
    virtual result<void> close() = 0;
    /// Возвращает текущий снимок статуса потока.
    [[nodiscard]] virtual stream_status status() const = 0;
};

/// Фабричный и диспетчерский слой, скрывающий конкретный бэкенд от публичного интерфейса.
class runtime_backend {
public:
    /// Освобождает ресурсы, принадлежащие бэкенду.
    virtual ~runtime_backend() = default;

    /// Перечисляет устройства для запрошенного направления.
    [[nodiscard]] virtual result<std::vector<device_info>> enumerate_devices(
        device_direction direction) const = 0;

    /// Определяет устройство по умолчанию для запрошенного направления и роли.
    [[nodiscard]] virtual result<device_info> default_device(
        device_direction direction,
        device_role role) const = 0;

    /// Открывает дескриптор render-потока.
    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_render_stream(
        const render_stream_config& config,
        render_callback& callback) = 0;

    /// Открывает дескриптор capture-потока для микрофона.
    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_capture_stream(
        const capture_stream_config& config,
        capture_callback& callback) = 0;

    /// Открывает дескриптор loopback capture-потока.
    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_loopback_stream(
        const loopback_stream_config& config,
        capture_callback& callback) = 0;
};

/// Создаёт платформенно-специфичную реализацию runtime-бэкенда.
[[nodiscard]] result<std::shared_ptr<runtime_backend>> make_runtime_backend(runtime_options options);
/// Адаптирует внутренний дескриптор потока к публичной обёртке render_stream.
[[nodiscard]] render_stream make_render_stream(std::shared_ptr<stream_handle> handle);
/// Адаптирует внутренний дескриптор потока к публичной обёртке capture_stream.
[[nodiscard]] capture_stream make_capture_stream(std::shared_ptr<stream_handle> handle);
/// Адаптирует внутренний дескриптор потока к публичной обёртке loopback_capture_stream.
[[nodiscard]] loopback_capture_stream make_loopback_capture_stream(std::shared_ptr<stream_handle> handle);

}  // namespace sonotide::detail
