#pragma once

#include <string>

namespace sonotide {

enum class device_direction {
    render,
    capture,
};

enum class device_role {
    console,
    multimedia,
    communications,
};

enum class device_state {
    active,
    disabled,
    not_present,
    unplugged,
    unknown,
};

struct device_info {
    std::string id;
    std::string friendly_name;
    device_direction direction = device_direction::render;
    device_state state = device_state::unknown;
    bool is_default_console = false;
    bool is_default_multimedia = false;
    bool is_default_communications = false;
};

}  // namespace sonotide

