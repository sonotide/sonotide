#pragma once

#include <memory>
#include <vector>

#include "sonotide/device_info.h"
#include "sonotide/result.h"
#include "sonotide/runtime.h"
#include "sonotide/stream_callback.h"
#include "sonotide/stream_config.h"
#include "sonotide/stream_status.h"

namespace sonotide::detail {

class stream_handle {
public:
    virtual ~stream_handle() = default;

    virtual result<void> start() = 0;
    virtual result<void> stop() = 0;
    virtual result<void> reset() = 0;
    virtual result<void> close() = 0;
    [[nodiscard]] virtual stream_status status() const = 0;
};

class runtime_backend {
public:
    virtual ~runtime_backend() = default;

    [[nodiscard]] virtual result<std::vector<device_info>> enumerate_devices(
        device_direction direction) const = 0;

    [[nodiscard]] virtual result<device_info> default_device(
        device_direction direction,
        device_role role) const = 0;

    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_render_stream(
        const render_stream_config& config,
        render_callback& callback) = 0;

    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_capture_stream(
        const capture_stream_config& config,
        capture_callback& callback) = 0;

    [[nodiscard]] virtual result<std::shared_ptr<stream_handle>> open_loopback_stream(
        const loopback_stream_config& config,
        capture_callback& callback) = 0;
};

[[nodiscard]] result<std::shared_ptr<runtime_backend>> make_runtime_backend(runtime_options options);

}  // namespace sonotide::detail

