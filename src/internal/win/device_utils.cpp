#include "internal/win/device_utils.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>

#include <string>

#include "internal/win/hresult_utils.h"

namespace sonotide::detail::win {
namespace {

using Microsoft::WRL::ComPtr;

EDataFlow to_data_flow(const device_direction direction) {
    return direction == device_direction::render ? eRender : eCapture;
}

ERole to_role(const device_role role) {
    switch (role) {
    case device_role::console:
        return eConsole;
    case device_role::multimedia:
        return eMultimedia;
    case device_role::communications:
        return eCommunications;
    }

    return eMultimedia;
}

device_state to_device_state(const DWORD state) {
    if ((state & DEVICE_STATE_ACTIVE) != 0U) {
        return device_state::active;
    }
    if ((state & DEVICE_STATE_DISABLED) != 0U) {
        return device_state::disabled;
    }
    if ((state & DEVICE_STATE_NOTPRESENT) != 0U) {
        return device_state::not_present;
    }
    if ((state & DEVICE_STATE_UNPLUGGED) != 0U) {
        return device_state::unplugged;
    }

    return device_state::unknown;
}

result<std::string> get_device_id(IMMDevice& device) {
    LPWSTR raw_id = nullptr;
    const HRESULT hr = device.GetId(&raw_id);
    if (FAILED(hr)) {
        return result<std::string>::failure(map_hresult(
            "IMMDevice::GetId",
            hr,
            error_category::device,
            error_code::device_enumeration_failed));
    }

    const std::wstring wide_id(raw_id != nullptr ? raw_id : L"");
    if (raw_id != nullptr) {
        CoTaskMemFree(raw_id);
    }

    return result<std::string>::success(utf8_from_utf16(wide_id));
}

result<std::string> get_friendly_name(IMMDevice& device) {
    ComPtr<IPropertyStore> store;
    HRESULT hr = device.OpenPropertyStore(STGM_READ, &store);
    if (FAILED(hr)) {
        return result<std::string>::failure(map_hresult(
            "IMMDevice::OpenPropertyStore",
            hr,
            error_category::device,
            error_code::device_enumeration_failed));
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    hr = store->GetValue(PKEY_Device_FriendlyName, &value);
    if (FAILED(hr)) {
        PropVariantClear(&value);
        return result<std::string>::failure(map_hresult(
            "IPropertyStore::GetValue(PKEY_Device_FriendlyName)",
            hr,
            error_category::device,
            error_code::device_enumeration_failed));
    }

    std::string friendly_name;
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        friendly_name = utf8_from_utf16(value.pwszVal);
    }
    PropVariantClear(&value);
    return result<std::string>::success(std::move(friendly_name));
}

}  // namespace

result<ComPtr<IMMDeviceEnumerator>> create_device_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return result<ComPtr<IMMDeviceEnumerator>>::failure(map_hresult(
            "CoCreateInstance(MMDeviceEnumerator)",
            hr,
            error_category::device,
            error_code::device_enumeration_failed));
    }

    return result<ComPtr<IMMDeviceEnumerator>>::success(std::move(enumerator));
}

result<device_info> build_device_info(
    IMMDeviceEnumerator& enumerator,
    IMMDevice& device,
    const device_direction direction) {
    device_info info;
    info.direction = direction;

    auto id_result = get_device_id(device);
    if (!id_result) {
        return result<device_info>::failure(id_result.error());
    }
    info.id = std::move(id_result.value());

    auto name_result = get_friendly_name(device);
    if (!name_result) {
        return result<device_info>::failure(name_result.error());
    }
    info.friendly_name = std::move(name_result.value());

    DWORD raw_state = 0;
    const HRESULT hr = device.GetState(&raw_state);
    if (FAILED(hr)) {
        return result<device_info>::failure(map_hresult(
            "IMMDevice::GetState",
            hr,
            error_category::device,
            error_code::device_enumeration_failed));
    }
    info.state = to_device_state(raw_state);

    for (const auto role : {device_role::console, device_role::multimedia, device_role::communications}) {
        ComPtr<IMMDevice> default_device;
        const HRESULT default_hr =
            enumerator.GetDefaultAudioEndpoint(to_data_flow(direction), to_role(role), &default_device);
        if (FAILED(default_hr) || default_device == nullptr) {
            continue;
        }

        auto default_id_result = get_device_id(*default_device.Get());
        if (!default_id_result) {
            continue;
        }

        const bool is_default = default_id_result.value() == info.id;
        if (role == device_role::console) {
            info.is_default_console = is_default;
        } else if (role == device_role::multimedia) {
            info.is_default_multimedia = is_default;
        } else {
            info.is_default_communications = is_default;
        }
    }

    return result<device_info>::success(std::move(info));
}

result<device_resolution> resolve_device(
    IMMDeviceEnumerator& enumerator,
    const device_selector& selector) {
    device_resolution resolution;
    HRESULT hr = S_OK;

    if (selector.selection_mode == device_selector::mode::explicit_id) {
        const std::wstring device_id = utf16_from_utf8(selector.device_id);
        hr = enumerator.GetDevice(device_id.c_str(), &resolution.device);
        if (FAILED(hr) || resolution.device == nullptr) {
            return result<device_resolution>::failure(map_hresult(
                "IMMDeviceEnumerator::GetDevice",
                hr,
                error_category::device,
                error_code::device_not_found));
        }
    } else {
        hr = enumerator.GetDefaultAudioEndpoint(
            to_data_flow(selector.direction),
            to_role(selector.role),
            &resolution.device);
        if (FAILED(hr) || resolution.device == nullptr) {
            return result<device_resolution>::failure(map_hresult(
                "IMMDeviceEnumerator::GetDefaultAudioEndpoint",
                hr,
                error_category::device,
                error_code::device_not_found));
        }
    }

    auto info_result = build_device_info(enumerator, *resolution.device.Get(), selector.direction);
    if (!info_result) {
        return result<device_resolution>::failure(info_result.error());
    }
    resolution.info = std::move(info_result.value());
    return result<device_resolution>::success(std::move(resolution));
}

}  // namespace sonotide::detail::win

