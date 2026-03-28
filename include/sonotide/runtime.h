#pragma once

#include <memory>
#include <vector>

#include "sonotide/capture_stream.h"
#include "sonotide/device_info.h"
#include "sonotide/loopback_capture_stream.h"
#include "sonotide/playback_session.h"
#include "sonotide/render_stream.h"
#include "sonotide/result.h"
#include "sonotide/stream_callback.h"
#include "sonotide/stream_config.h"

namespace sonotide {
namespace detail {
class runtime_backend;
}

/// Настройки создания runtime.
struct runtime_options {
    /// Нужно ли инициализировать Media Foundation при создании runtime.
    bool initialize_media_foundation = true;
    /// Нужно ли включать MMCSS для worker thread-ов.
    bool enable_mmcss = true;
};

/// Точка входа в Sonotide, владеющая platform backend.
class runtime {
public:
    /// Создаёт runtime с указанными настройками.
    static result<runtime> create(runtime_options options = {});

    /// Перемещает runtime.
    runtime(runtime&&) noexcept = default;
    /// Перемещает runtime.
    runtime& operator=(runtime&&) noexcept = default;

    /// Объект runtime не копируется.
    runtime(const runtime&) = delete;
    /// Объект runtime не копируется.
    runtime& operator=(const runtime&) = delete;

    /// Освобождает backend-ресурсы.
    ~runtime() = default;

    /// Перечисляет устройства в указанном направлении.
    result<std::vector<device_info>> enumerate_devices(device_direction direction) const;
    /// Возвращает default device для указанного направления и роли.
    result<device_info> default_device(
        device_direction direction,
        device_role role = device_role::multimedia) const;

    /// Открывает render stream.
    result<render_stream> open_render_stream(
        const render_stream_config& config,
        render_callback& callback);

    /// Открывает capture stream.
    result<capture_stream> open_capture_stream(
        const capture_stream_config& config,
        capture_callback& callback);

    /// Открывает loopback capture stream.
    result<loopback_capture_stream> open_loopback_stream(
        const loopback_stream_config& config,
        capture_callback& callback);

    /// Открывает high-level playback session.
    result<playback_session> open_playback_session(
        const playback_session_config& config = {});

private:
    /// Приватный конструктор, принимающий уже созданный backend.
    explicit runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept;

    /// Разделяемое владение платформенным backend-ом.
    std::shared_ptr<detail::runtime_backend> backend_;
};

}  // namespace sonotide
