#include "sonotide/render_stream.h"

#include <memory>
#include <string_view>

#include "internal/runtime_backend.h"

namespace sonotide {
namespace {

// Формирует стабильную ошибку для любого вызова stream-метода на отсоединённой обёртке.
error make_invalid_handle_error(const std::string_view operation) {
    error failure;
    failure.category = error_category::stream;
    failure.code = error_code::invalid_state;
    failure.operation = std::string(operation);
    failure.message = "Render stream handle is not bound to a runtime instance.";
    return failure;
}

// Возвращает closed-состояние для отсоединённых обёрток, чтобы снимки статуса были детерминированными.
stream_status closed_status() {
    stream_status status;
    status.state = stream_state::closed;
    return status;
}

void close_noexcept(const std::shared_ptr<detail::stream_handle>& handle) noexcept {
    if (handle) {
        try {
            (void)handle->close();
        } catch (...) {
            // Destructors and noexcept move assignment cannot surface cleanup
            // failures; explicit close() remains the diagnostic path.
        }
    }
}

}  // namespace

// Сохраняет принадлежащий внутренней реализации дескриптор потока внутри публичной render_stream-обёртки.
render_stream::render_stream(std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

render_stream::render_stream(render_stream&& other) noexcept
    : handle_(other.handle_.exchange(nullptr)) {}

render_stream::~render_stream() noexcept {
    close_noexcept(handle_.exchange(nullptr));
}

render_stream& render_stream::operator=(render_stream&& other) noexcept {
    if (this != &other) {
        close_noexcept(handle_.exchange(nullptr));
        handle_.store(other.handle_.exchange(nullptr));
    }
    return *this;
}

// Экспортирует приватную фабрику, чтобы runtime мог строить обёртки, не раскрывая владение handle.
render_stream detail::make_render_stream(std::shared_ptr<detail::stream_handle> handle) {
    return render_stream(std::move(handle));
}

// Показывает, владеет ли обёртка ещё живым дескриптором потока во внутренней реализации.
bool render_stream::is_open() const noexcept {
    return static_cast<bool>(handle_.load());
}

// Запускает базовый поток и возвращает структурированную ошибку, если обёртка отсоединена.
result<void> render_stream::start() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("render_stream::start"));
    }

    return handle->start();
}

// Останавливает базовый поток и пробрасывает ошибки завершения внутренней реализации.
result<void> render_stream::stop() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("render_stream::stop"));
    }

    return handle->stop();
}

// Сбрасывает жизненный цикл потока обратно в prepared без пересоздания обёртки.
result<void> render_stream::reset() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("render_stream::reset"));
    }

    return handle->reset();
}

// Закрывает handle потока и оставляет обёртку отсоединённой.
result<void> render_stream::close() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("render_stream::close"));
    }

    auto close_result = handle->close();
    if (close_result) {
        auto expected = handle;
        (void)handle_.compare_exchange_strong(expected, nullptr);
    }
    return close_result;
}

// Возвращает текущий статус внутренней реализации или closed-снимок, если обёртка уже отсоединена.
stream_status render_stream::status() const {
    auto handle = handle_.load();
    if (!handle) {
        return closed_status();
    }

    return handle->status();
}

}  // namespace sonotide
