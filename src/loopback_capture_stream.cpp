#include "sonotide/loopback_capture_stream.h"

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
    failure.message = "Loopback stream handle is not bound to a runtime instance.";
    return failure;
}

// Возвращает closed-состояние для отсоединённых обёрток, чтобы снимки статуса были детерминированными.
stream_status closed_status() {
    stream_status status;
    status.state = stream_state::closed;
    return status;
}

}  // namespace

// Сохраняет принадлежащий внутренней реализации дескриптор потока внутри публичной loopback_capture_stream-обёртки.
loopback_capture_stream::loopback_capture_stream(
    std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

// Экспортирует приватную фабрику, чтобы runtime мог строить обёртки, не раскрывая владение handle.
loopback_capture_stream detail::make_loopback_capture_stream(
    std::shared_ptr<detail::stream_handle> handle) {
    return loopback_capture_stream(std::move(handle));
}

// Показывает, владеет ли обёртка ещё живым дескриптором потока во внутренней реализации.
bool loopback_capture_stream::is_open() const noexcept {
    return static_cast<bool>(handle_);
}

// Запускает поток loopback-захвата и возвращает структурированную ошибку, если обёртка отсоединена.
result<void> loopback_capture_stream::start() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::start"));
    }

    return handle_->start();
}

// Останавливает поток loopback-захвата и пробрасывает ошибки завершения внутренней реализации.
result<void> loopback_capture_stream::stop() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::stop"));
    }

    return handle_->stop();
}

// Сбрасывает жизненный цикл потока обратно в prepared без пересоздания обёртки.
result<void> loopback_capture_stream::reset() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::reset"));
    }

    return handle_->reset();
}

// Закрывает handle потока и оставляет обёртку отсоединённой.
result<void> loopback_capture_stream::close() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::close"));
    }

    return handle_->close();
}

// Возвращает текущий статус внутренней реализации или closed-снимок, если обёртка уже отсоединена.
stream_status loopback_capture_stream::status() const {
    if (!handle_) {
        return closed_status();
    }

    return handle_->status();
}

}  // namespace sonotide
