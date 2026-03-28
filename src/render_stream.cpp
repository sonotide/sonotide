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

}  // namespace

// Сохраняет принадлежащий внутренней реализации дескриптор потока внутри публичной render_stream-обёртки.
render_stream::render_stream(std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

// Экспортирует приватную фабрику, чтобы runtime мог строить обёртки, не раскрывая владение handle.
render_stream detail::make_render_stream(std::shared_ptr<detail::stream_handle> handle) {
    return render_stream(std::move(handle));
}

// Показывает, владеет ли обёртка ещё живым дескриптором потока во внутренней реализации.
bool render_stream::is_open() const noexcept {
    return static_cast<bool>(handle_);
}

// Запускает базовый поток и возвращает структурированную ошибку, если обёртка отсоединена.
result<void> render_stream::start() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::start"));
    }

    return handle_->start();
}

// Останавливает базовый поток и пробрасывает ошибки завершения внутренней реализации.
result<void> render_stream::stop() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::stop"));
    }

    return handle_->stop();
}

// Сбрасывает жизненный цикл потока обратно в prepared без пересоздания обёртки.
result<void> render_stream::reset() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::reset"));
    }

    return handle_->reset();
}

// Закрывает handle потока и оставляет обёртку отсоединённой.
result<void> render_stream::close() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::close"));
    }

    return handle_->close();
}

// Возвращает текущий статус внутренней реализации или closed-снимок, если обёртка уже отсоединена.
stream_status render_stream::status() const {
    if (!handle_) {
        return closed_status();
    }

    return handle_->status();
}

}  // namespace sonotide
