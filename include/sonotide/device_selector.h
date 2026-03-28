#pragma once

#include <string>

#include "sonotide/device_info.h"

namespace sonotide {

/// Описывает, к какому audio endpoint должен привязаться поток.
struct device_selector {
    /// Стратегия выбора, используемая селектором.
    enum class mode {
        /// Разрешить system default endpoint по направлению/роли.
        system_default,
        /// Привязаться к явному endpoint id.
        explicit_id,
    };

    /// Строит selector, который выбирает default device по роли.
    static device_selector system_default(
        device_direction direction,
        device_role role = device_role::multimedia) {
        device_selector selector;
        selector.selection_mode = mode::system_default;
        selector.direction = direction;
        selector.role = role;
        return selector;
    }

    /// Строит selector, который привязывается к конкретному endpoint id.
    static device_selector explicit_id(device_direction direction, std::string id) {
        device_selector selector;
        selector.selection_mode = mode::explicit_id;
        selector.direction = direction;
        selector.device_id = std::move(id);
        return selector;
    }

    /// Режим выбора.
    mode selection_mode = mode::system_default;
    /// Направление endpoint.
    device_direction direction = device_direction::render;
    /// Роль default endpoint, когда `selection_mode` равно `system_default`.
    device_role role = device_role::multimedia;
    /// Явный device id, когда `selection_mode` равно `explicit_id`.
    std::string device_id;
};

}  // namespace sonotide
