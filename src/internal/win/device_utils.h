#pragma once

#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <string>

#include "sonotide/device_info.h"
#include "sonotide/device_selector.h"
#include "sonotide/result.h"

namespace sonotide::detail::win {

/// Полный результат разрешения device selector в конкретное WASAPI-устройство.
struct device_resolution {
    /// Сам активированный endpoint, которым будет пользоваться backend.
    Microsoft::WRL::ComPtr<IMMDevice> device;
    /// Снимок публичной информации об устройстве, пригодный для API и логирования.
    device_info info;
};

/// Создаёт COM enumerator для работы с endpoint-ами.
[[nodiscard]] result<Microsoft::WRL::ComPtr<IMMDeviceEnumerator>> create_device_enumerator();
/// Собирает `device_info` для уже активированного endpoint-а.
[[nodiscard]] result<device_info> build_device_info(
    IMMDeviceEnumerator& enumerator,
    IMMDevice& device,
    device_direction direction);
/// Разрешает `device_selector` в реальный endpoint и сопровождающий `device_info`.
[[nodiscard]] result<device_resolution> resolve_device(
    IMMDeviceEnumerator& enumerator,
    const device_selector& selector);

}  // namespace sonotide::detail::win
