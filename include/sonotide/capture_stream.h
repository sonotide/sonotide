#pragma once

#include <memory>

#include "sonotide/result.h"
#include "sonotide/stream_status.h"

namespace sonotide {
class capture_stream;
namespace detail {
class stream_handle;
capture_stream make_capture_stream(std::shared_ptr<stream_handle> handle);
}

/// Высокоуровневая RAII-обёртка над capture stream handle.
class capture_stream {
public:
    /// Создаёт пустую не владеющую обёртку stream.
    capture_stream() = default;
    /// Освобождает stream handle при уничтожении обёртки.
    ~capture_stream() = default;

    /// Перемещает владение базовым handle.
    capture_stream(capture_stream&&) noexcept = default;
    /// Перемещает владение базовым handle.
    capture_stream& operator=(capture_stream&&) noexcept = default;

    /// Поток capture не копируется.
    capture_stream(const capture_stream&) = delete;
    /// Поток capture не копируется.
    capture_stream& operator=(const capture_stream&) = delete;

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
    explicit capture_stream(std::shared_ptr<detail::stream_handle> handle) noexcept;

    /// Разделяемое владение backend stream handle.
    std::shared_ptr<detail::stream_handle> handle_;

    /// Runtime имеет право создавать обёртки потоков.
    friend class runtime;
    /// Внутренняя фабрика для создания обёрток из backend handle.
    friend capture_stream detail::make_capture_stream(std::shared_ptr<detail::stream_handle> handle);
};

}  // namespace sonotide
