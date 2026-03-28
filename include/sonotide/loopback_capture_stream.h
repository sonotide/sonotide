#pragma once

#include <memory>

#include "sonotide/result.h"
#include "sonotide/stream_status.h"

namespace sonotide {
class loopback_capture_stream;
namespace detail {
class stream_handle;
loopback_capture_stream make_loopback_capture_stream(std::shared_ptr<stream_handle> handle);
}

class loopback_capture_stream {
public:
    /// Создаёт пустую не владеющую обёртку stream.
    loopback_capture_stream() = default;
    /// Освобождает stream handle при уничтожении обёртки.
    ~loopback_capture_stream() = default;

    /// Перемещает владение базовым handle.
    loopback_capture_stream(loopback_capture_stream&&) noexcept = default;
    /// Перемещает владение базовым handle.
    loopback_capture_stream& operator=(loopback_capture_stream&&) noexcept = default;

    /// Поток loopback capture не копируется.
    loopback_capture_stream(const loopback_capture_stream&) = delete;
    /// Поток loopback capture не копируется.
    loopback_capture_stream& operator=(const loopback_capture_stream&) = delete;

    /// Возвращает `true`, когда обёртка владеет handle.
    [[nodiscard]] bool is_open() const noexcept;
    /// Запускает worker потока.
    result<void> start();
    /// Останавливает worker потока.
    result<void> stop();
    /// Сбрасывает поток обратно в состояние prepared.
    result<void> reset();
    /// Закрывает поток навсегда.
    result<void> close();
    /// Возвращает актуальный snapshot состояния.
    [[nodiscard]] stream_status status() const;

private:
    /// Создаёт владеющую обёртку из backend handle.
    explicit loopback_capture_stream(std::shared_ptr<detail::stream_handle> handle) noexcept;

    /// Разделяемое владение backend stream handle.
    std::shared_ptr<detail::stream_handle> handle_;

    /// Runtime имеет право создавать обёртки потоков.
    friend class runtime;
    /// Внутренняя фабрика для создания обёрток из backend handle.
    friend loopback_capture_stream detail::make_loopback_capture_stream(
        std::shared_ptr<detail::stream_handle> handle);
};

}  // namespace sonotide
