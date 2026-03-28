#pragma once

#include <memory>
#include <vector>

#include "sonotide/capture_stream.h"
#include "sonotide/device_info.h"
#include "sonotide/loopback_capture_stream.h"
#include "sonotide/render_stream.h"
#include "sonotide/result.h"
#include "sonotide/stream_callback.h"
#include "sonotide/stream_config.h"

namespace sonotide {
namespace detail {
class runtime_backend;
}

struct runtime_options {
    bool initialize_media_foundation = true;
    bool enable_mmcss = true;
};

class runtime {
public:
    static result<runtime> create(runtime_options options = {});

    runtime(runtime&&) noexcept = default;
    runtime& operator=(runtime&&) noexcept = default;

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    ~runtime() = default;

    result<std::vector<device_info>> enumerate_devices(device_direction direction) const;
    result<device_info> default_device(
        device_direction direction,
        device_role role = device_role::multimedia) const;

    result<render_stream> open_render_stream(
        const render_stream_config& config,
        render_callback& callback);

    result<capture_stream> open_capture_stream(
        const capture_stream_config& config,
        capture_callback& callback);

    result<loopback_capture_stream> open_loopback_stream(
        const loopback_stream_config& config,
        capture_callback& callback);

private:
    explicit runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept;

    std::shared_ptr<detail::runtime_backend> backend_;
};

}  // namespace sonotide

