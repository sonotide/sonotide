#pragma once

#include <memory>

#include "internal/runtime_backend.h"

namespace sonotide::detail::win {

[[nodiscard]] result<std::shared_ptr<stream_handle>> open_render_stream(
    const render_stream_config& config,
    render_callback& callback,
    runtime_options options);

[[nodiscard]] result<std::shared_ptr<stream_handle>> open_capture_stream(
    const capture_stream_config& config,
    capture_callback& callback,
    runtime_options options);

[[nodiscard]] result<std::shared_ptr<stream_handle>> open_loopback_stream(
    const loopback_stream_config& config,
    capture_callback& callback,
    runtime_options options);

}  // namespace sonotide::detail::win

