#include "sonotide/loopback_capture_stream.h"

#include <memory>
#include <string_view>

#include "internal/runtime_backend.h"

namespace sonotide {
namespace {

error make_invalid_handle_error(const std::string_view operation) {
    error failure;
    failure.category = error_category::stream;
    failure.code = error_code::invalid_state;
    failure.operation = std::string(operation);
    failure.message = "Loopback stream handle is not bound to a runtime instance.";
    return failure;
}

stream_status closed_status() {
    stream_status status;
    status.state = stream_state::closed;
    return status;
}

}  // namespace

loopback_capture_stream::loopback_capture_stream(
    std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

bool loopback_capture_stream::is_open() const noexcept {
    return static_cast<bool>(handle_);
}

result<void> loopback_capture_stream::start() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::start"));
    }

    return handle_->start();
}

result<void> loopback_capture_stream::stop() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::stop"));
    }

    return handle_->stop();
}

result<void> loopback_capture_stream::reset() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::reset"));
    }

    return handle_->reset();
}

result<void> loopback_capture_stream::close() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("loopback_capture_stream::close"));
    }

    return handle_->close();
}

stream_status loopback_capture_stream::status() const {
    if (!handle_) {
        return closed_status();
    }

    return handle_->status();
}

}  // namespace sonotide

