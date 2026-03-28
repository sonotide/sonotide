#pragma once

#include <string>

#include "sonotide/device_info.h"

namespace sonotide {

struct device_selector {
    enum class mode {
        system_default,
        explicit_id,
    };

    static device_selector system_default(
        device_direction direction,
        device_role role = device_role::multimedia) {
        device_selector selector;
        selector.selection_mode = mode::system_default;
        selector.direction = direction;
        selector.role = role;
        return selector;
    }

    static device_selector explicit_id(device_direction direction, std::string id) {
        device_selector selector;
        selector.selection_mode = mode::explicit_id;
        selector.direction = direction;
        selector.device_id = std::move(id);
        return selector;
    }

    mode selection_mode = mode::system_default;
    device_direction direction = device_direction::render;
    device_role role = device_role::multimedia;
    std::string device_id;
};

}  // namespace sonotide

