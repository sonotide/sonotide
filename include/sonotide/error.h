#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sonotide {

enum class error_category {
    configuration,
    initialization,
    device,
    format,
    stream,
    callback,
    platform,
};

enum class error_code {
    invalid_argument,
    invalid_state,
    unsupported_platform,
    initialization_failed,
    device_not_found,
    device_enumeration_failed,
    format_negotiation_failed,
    stream_open_failed,
    stream_start_failed,
    stream_stop_failed,
    callback_failed,
    platform_failure,
    not_implemented,
};

struct error {
    error_category category = error_category::platform;
    error_code code = error_code::platform_failure;
    std::string message;
    std::string operation;
    std::optional<std::int64_t> native_code;
    bool recoverable = false;
};

[[nodiscard]] std::string_view to_string(error_category category) noexcept;
[[nodiscard]] std::string_view to_string(error_code code) noexcept;

}  // namespace sonotide

