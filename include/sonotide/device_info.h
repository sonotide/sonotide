#pragma once

#include <string>

namespace sonotide {

/// Направление физического или виртуального endpoint.
enum class device_direction {
    /// Путь воспроизведения/render.
    render,
    /// Путь микрофона/capture.
    capture,
};

/// Роль default device, которую сообщает Windows.
enum class device_role {
    /// Интерактивный endpoint Console по умолчанию.
    console,
    /// Медийный endpoint Multimedia по умолчанию.
    multimedia,
    /// Endpoint Communications для звонков и чатов.
    communications,
};

/// Текущее состояние доступности аудио endpoint.
enum class device_state {
    /// Endpoint активен и пригоден к использованию.
    active,
    /// Endpoint отключён на уровне ОС.
    disabled,
    /// Endpoint существует, но сейчас не присутствует.
    not_present,
    /// Endpoint отключён или временно недоступен.
    unplugged,
    /// Состояние endpoint не удалось классифицировать.
    unknown,
};

/// Описывает один перечисленный endpoint и его default flags.
struct device_info {
    /// Стабильный идентификатор устройства.
    std::string id;
    /// Человекочитаемое имя устройства.
    std::string friendly_name;
    /// Направление endpoint.
    device_direction direction = device_direction::render;
    /// Состояние runtime, которое сообщает ОС.
    device_state state = device_state::unknown;
    /// `true`, когда устройство является default для console audio.
    bool is_default_console = false;
    /// `true`, когда устройство является default для multimedia audio.
    bool is_default_multimedia = false;
    /// `true`, когда устройство является default для communications audio.
    bool is_default_communications = false;
};

}  // namespace sonotide
