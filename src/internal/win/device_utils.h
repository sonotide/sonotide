#pragma once

#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <string>

#include "sonotide/device_info.h"
#include "sonotide/device_selector.h"
#include "sonotide/result.h"

namespace sonotide::detail::win {

struct device_resolution {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    device_info info;
};

[[nodiscard]] result<Microsoft::WRL::ComPtr<IMMDeviceEnumerator>> create_device_enumerator();
[[nodiscard]] result<device_info> build_device_info(
    IMMDeviceEnumerator& enumerator,
    IMMDevice& device,
    device_direction direction);
[[nodiscard]] result<device_resolution> resolve_device(
    IMMDeviceEnumerator& enumerator,
    const device_selector& selector);

}  // namespace sonotide::detail::win

