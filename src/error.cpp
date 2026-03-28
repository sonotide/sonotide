#include "sonotide/error.h"

namespace sonotide {

std::string_view to_string(const error_category category) noexcept {
    switch (category) {
    case error_category::configuration:
        return "configuration";
    case error_category::initialization:
        return "initialization";
    case error_category::device:
        return "device";
    case error_category::format:
        return "format";
    case error_category::stream:
        return "stream";
    case error_category::callback:
        return "callback";
    case error_category::platform:
        return "platform";
    }

    return "platform";
}

std::string_view to_string(const error_code code) noexcept {
    switch (code) {
    case error_code::invalid_argument:
        return "invalid_argument";
    case error_code::invalid_state:
        return "invalid_state";
    case error_code::unsupported_platform:
        return "unsupported_platform";
    case error_code::initialization_failed:
        return "initialization_failed";
    case error_code::device_not_found:
        return "device_not_found";
    case error_code::device_enumeration_failed:
        return "device_enumeration_failed";
    case error_code::format_negotiation_failed:
        return "format_negotiation_failed";
    case error_code::stream_open_failed:
        return "stream_open_failed";
    case error_code::stream_start_failed:
        return "stream_start_failed";
    case error_code::stream_stop_failed:
        return "stream_stop_failed";
    case error_code::callback_failed:
        return "callback_failed";
    case error_code::platform_failure:
        return "platform_failure";
    case error_code::not_implemented:
        return "not_implemented";
    }

    return "platform_failure";
}

}  // namespace sonotide

