#include "internal/runtime_backend.h"

#include <memory>
#include <string>

namespace sonotide::detail {
namespace {

error make_unsupported_error(const std::string& operation) {
    error failure;
    failure.category = error_category::platform;
    failure.code = error_code::unsupported_platform;
    failure.operation = operation;
    failure.message = "Sonotide currently provides a WASAPI backend and requires Windows.";
    return failure;
}

class unsupported_runtime_backend final : public runtime_backend {
public:
    result<std::vector<device_info>> enumerate_devices(device_direction) const override {
        return result<std::vector<device_info>>::failure(
            make_unsupported_error("runtime::enumerate_devices"));
    }

    result<device_info> default_device(device_direction, device_role) const override {
        return result<device_info>::failure(make_unsupported_error("runtime::default_device"));
    }

    result<std::shared_ptr<stream_handle>> open_render_stream(
        const render_stream_config&,
        render_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_render_stream"));
    }

    result<std::shared_ptr<stream_handle>> open_capture_stream(
        const capture_stream_config&,
        capture_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_capture_stream"));
    }

    result<std::shared_ptr<stream_handle>> open_loopback_stream(
        const loopback_stream_config&,
        capture_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_loopback_stream"));
    }
};

}  // namespace

result<std::shared_ptr<runtime_backend>> make_runtime_backend(runtime_options) {
    return result<std::shared_ptr<runtime_backend>>::success(
        std::make_shared<unsupported_runtime_backend>());
}

}  // namespace sonotide::detail

