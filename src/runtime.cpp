#include "sonotide/runtime.h"

#include <memory>
#include <utility>

#include "internal/runtime_backend.h"

namespace sonotide {

runtime::runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept
    : backend_(std::move(backend)) {}

result<runtime> runtime::create(runtime_options options) {
    auto backend_result = detail::make_runtime_backend(options);
    if (!backend_result) {
        return result<runtime>::failure(backend_result.error());
    }

    return result<runtime>::success(runtime(std::move(backend_result.value())));
}

result<std::vector<device_info>> runtime::enumerate_devices(const device_direction direction) const {
    return backend_->enumerate_devices(direction);
}

result<device_info> runtime::default_device(
    const device_direction direction,
    const device_role role) const {
    return backend_->default_device(direction, role);
}

result<render_stream> runtime::open_render_stream(
    const render_stream_config& config,
    render_callback& callback) {
    auto handle_result = backend_->open_render_stream(config, callback);
    if (!handle_result) {
        return result<render_stream>::failure(handle_result.error());
    }

    return result<render_stream>::success(render_stream(std::move(handle_result.value())));
}

result<capture_stream> runtime::open_capture_stream(
    const capture_stream_config& config,
    capture_callback& callback) {
    auto handle_result = backend_->open_capture_stream(config, callback);
    if (!handle_result) {
        return result<capture_stream>::failure(handle_result.error());
    }

    return result<capture_stream>::success(capture_stream(std::move(handle_result.value())));
}

result<loopback_capture_stream> runtime::open_loopback_stream(
    const loopback_stream_config& config,
    capture_callback& callback) {
    auto handle_result = backend_->open_loopback_stream(config, callback);
    if (!handle_result) {
        return result<loopback_capture_stream>::failure(handle_result.error());
    }

    return result<loopback_capture_stream>::success(
        loopback_capture_stream(std::move(handle_result.value())));
}

}  // namespace sonotide

