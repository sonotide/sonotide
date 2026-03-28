#include "sonotide/render_stream.h"

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
    failure.message = "Render stream handle is not bound to a runtime instance.";
    return failure;
}

stream_status closed_status() {
    stream_status status;
    status.state = stream_state::closed;
    return status;
}

}  // namespace

render_stream::render_stream(std::shared_ptr<detail::stream_handle> handle) noexcept
    : handle_(std::move(handle)) {}

bool render_stream::is_open() const noexcept {
    return static_cast<bool>(handle_);
}

result<void> render_stream::start() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::start"));
    }

    return handle_->start();
}

result<void> render_stream::stop() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::stop"));
    }

    return handle_->stop();
}

result<void> render_stream::reset() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::reset"));
    }

    return handle_->reset();
}

result<void> render_stream::close() {
    if (!handle_) {
        return result<void>::failure(make_invalid_handle_error("render_stream::close"));
    }

    return handle_->close();
}

stream_status render_stream::status() const {
    if (!handle_) {
        return closed_status();
    }

    return handle_->status();
}

}  // namespace sonotide

