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

void close_noexcept(const std::shared_ptr<detail::stream_handle>& handle) noexcept {
    if (handle) {
        try {
            (void)handle->close();
        } catch (...) {
            // Explicit close() is the diagnostic path; implicit cleanup must
            // honor the public noexcept destructor/move contract.
        }
    }
}

}  // namespace

// Сохраняет принадлежащий внутренней реализации дескриптор потока внутри публичной loopback_capture_stream-обёртки.
loopback_capture_stream::loopback_capture_stream(
    std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

loopback_capture_stream::loopback_capture_stream(loopback_capture_stream&& other) noexcept
    : handle_(other.handle_.exchange(nullptr)) {}

loopback_capture_stream::~loopback_capture_stream() noexcept {
    close_noexcept(handle_.exchange(nullptr));
}

loopback_capture_stream& loopback_capture_stream::operator=(
    loopback_capture_stream&& other) noexcept {
    if (this != &other) {
        close_noexcept(handle_.exchange(nullptr));
        handle_.store(other.handle_.exchange(nullptr));
    }
    return *this;
}

// Экспортирует приватную фабрику, чтобы runtime мог строить обёртки, не раскрывая владение handle.
loopback_capture_stream detail::make_loopback_capture_stream(
    std::shared_ptr<detail::stream_handle> handle) {
    return loopback_capture_stream(std::move(handle));
}

// Показывает, владеет ли обёртка ещё живым дескриптором потока во внутренней реализации.
bool loopback_capture_stream::is_open() const noexcept {
    return static_cast<bool>(handle_.load());
}

// Запускает поток loopback-захвата и возвращает структурированную ошибку, если обёртка отсоединена.
result<void> loopback_capture_stream::start() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::start"));
    }

    return handle->start();
}

// Останавливает поток loopback-захвата и пробрасывает ошибки завершения внутренней реализации.
result<void> loopback_capture_stream::stop() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::stop"));
    }

    return handle->stop();
}

// Сбрасывает жизненный цикл потока обратно в prepared без пересоздания обёртки.
result<void> loopback_capture_stream::reset() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::reset"));
    }

    return handle->reset();
}

// Закрывает handle потока и оставляет обёртку отсоединённой.
result<void> loopback_capture_stream::close() {
    auto handle = handle_.load();
    if (!handle) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::close"));
    }

    auto close_result = handle->close();
    if (close_result) {
        auto expected = handle;
        (void)handle_.compare_exchange_strong(expected, nullptr);
    }
    return close_result;
}

// Возвращает текущий статус внутренней реализации или closed-снимок, если обёртка уже отсоединена.
stream_status loopback_capture_stream::status() const {
    auto handle = handle_.load();
    if (!handle) {
        return closed_status();
    }

    return handle->status();
}

}  // namespace sonotide
