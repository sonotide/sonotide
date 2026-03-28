#pragma once

#include <memory>

#include "sonotide/result.h"
#include "sonotide/stream_status.h"

namespace sonotide {
class render_stream;
namespace detail {
class stream_handle;
render_stream make_render_stream(std::shared_ptr<stream_handle> handle);
}

/// Высокоуровневая RAII-обёртка над render stream handle.
class render_stream {
public:
    /// Создаёт пустую не владеющую обёртку stream.
    render_stream() = default;
    /// Освобождает stream handle при уничтожении обёртки.
    ~render_stream() = default;

    /// Перемещает владение базовым handle.
    render_stream(render_stream&&) noexcept = default;
    /// Перемещает владение базовым handle.
    render_stream& operator=(render_stream&&) noexcept = default;

    /// Поток render не копируется.
    render_stream(const render_stream&) = delete;
    /// Поток render не копируется.
    render_stream& operator=(const render_stream&) = delete;

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
    explicit render_stream(std::shared_ptr<detail::stream_handle> handle) noexcept;

    /// Разделяемое владение backend stream handle.
    std::shared_ptr<detail::stream_handle> handle_;

    /// Runtime имеет право создавать обёртки потоков.
    friend class runtime;
    /// Playback session использует внутреннее повторное открытие render stream.
    friend class playback_session;
    /// Внутренняя фабрика для создания обёрток из backend handle.
    friend render_stream detail::make_render_stream(std::shared_ptr<detail::stream_handle> handle);
};

}  // namespace sonotide
