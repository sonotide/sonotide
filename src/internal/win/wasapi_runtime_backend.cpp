#include "internal/runtime_backend.h"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <memory>
#include <utility>
#include <vector>

#include <mfapi.h>

#include "internal/win/com_scope.h"
#include "internal/win/device_utils.h"
#include "internal/win/hresult_utils.h"
#include "internal/win/wasapi_stream_handle.h"

namespace sonotide::detail {
namespace {

using Microsoft::WRL::ComPtr;

/// Бэкенд runtime для Windows, который владеет запуском Media Foundation и маршрутизацией WASAPI.
class wasapi_runtime_backend final : public runtime_backend {
public:
    /// Создаёт backend и при необходимости запускает Media Foundation для playback-сессий.
    static result<std::shared_ptr<runtime_backend>> create(runtime_options options) {
        // Backend живёт в heap под разделяемым владением, чтобы stream-сессии могли пережить вызов фабрики.
        auto backend = std::shared_ptr<wasapi_runtime_backend>(
            new wasapi_runtime_backend(options));

        // Media Foundation опциональна во время выполнения, потому что простое перечисление устройств её не требует.
        if (options.initialize_media_foundation) {
            const HRESULT hr = MFStartup(MF_VERSION);
            if (FAILED(hr)) {
                return result<std::shared_ptr<runtime_backend>>::failure(win::map_hresult(
                    "MFStartup",
                    hr,
                    error_category::initialization,
                    error_code::initialization_failed));
            }
            backend->media_foundation_started_ = true;
        }

        return result<std::shared_ptr<runtime_backend>>::success(
            std::static_pointer_cast<runtime_backend>(std::move(backend)));
    }

    /// Завершает Media Foundation, если этот backend запускал её сам.
    ~wasapi_runtime_backend() override {
        if (media_foundation_started_) {
            MFShutdown();
        }
    }

    /// Перечисляет активные устройства в запрошенном направлении.
    result<std::vector<device_info>> enumerate_devices(const device_direction direction) const override {
        // Каждый вызов использует свежий COM scope, чтобы вызывающему коду не нужно было думать о состоянии apartment.
        win::com_scope com;
        auto com_result = com.initialize_multithreaded();
        if (!com_result) {
            return result<std::vector<device_info>>::failure(com_result.error());
        }

        auto enumerator_result = win::create_device_enumerator();
        if (!enumerator_result) {
            return result<std::vector<device_info>>::failure(enumerator_result.error());
        }

        ComPtr<IMMDeviceCollection> collection;
        const HRESULT hr = enumerator_result.value()->EnumAudioEndpoints(
            direction == device_direction::render ? eRender : eCapture,
            DEVICE_STATEMASK_ALL,
            &collection);
        if (FAILED(hr)) {
            return result<std::vector<device_info>>::failure(win::map_hresult(
                "IMMDeviceEnumerator::EnumAudioEndpoints",
                hr,
                error_category::device,
                error_code::device_enumeration_failed));
        }

        UINT count = 0;
        collection->GetCount(&count);

        // Резервируем точное число endpoint-ов, чтобы избежать reallocations при сборке снимка.
        std::vector<device_info> devices;
        devices.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            const HRESULT item_hr = collection->Item(index, &device);
            if (FAILED(item_hr) || device == nullptr) {
                return result<std::vector<device_info>>::failure(win::map_hresult(
                    "IMMDeviceCollection::Item",
                    item_hr,
                    error_category::device,
                    error_code::device_enumeration_failed));
            }

            auto info_result = win::build_device_info(
                *enumerator_result.value().Get(),
                *device.Get(),
                direction);
            if (!info_result) {
                return result<std::vector<device_info>>::failure(info_result.error());
            }

            devices.push_back(std::move(info_result.value()));
        }

        return result<std::vector<device_info>>::success(std::move(devices));
    }

    /// Разрешает endpoint по умолчанию для запрошенного направления и роли.
    result<device_info> default_device(
        const device_direction direction,
        const device_role role) const override {
        // Backend отвечает за преобразование публичных ролей в роли Core Audio.
        win::com_scope com;
        auto com_result = com.initialize_multithreaded();
        if (!com_result) {
            return result<device_info>::failure(com_result.error());
        }

        auto enumerator_result = win::create_device_enumerator();
        if (!enumerator_result) {
            return result<device_info>::failure(enumerator_result.error());
        }

        ComPtr<IMMDevice> device;
        const HRESULT hr = enumerator_result.value()->GetDefaultAudioEndpoint(
            direction == device_direction::render ? eRender : eCapture,
            role == device_role::console ? eConsole :
                (role == device_role::communications ? eCommunications : eMultimedia),
            &device);
        if (FAILED(hr) || device == nullptr) {
            return result<device_info>::failure(win::map_hresult(
                "IMMDeviceEnumerator::GetDefaultAudioEndpoint",
                hr,
                error_category::device,
                error_code::device_not_found));
        }

        return win::build_device_info(*enumerator_result.value().Get(), *device.Get(), direction);
    }

    /// Открывает handle render stream для публичного wrapper-слоя.
    result<std::shared_ptr<stream_handle>> open_render_stream(
        const render_stream_config& config,
        render_callback& callback) override {
        return win::open_render_stream(config, callback, options_);
    }

    /// Открывает handle stream захвата микрофона для публичного wrapper-слоя.
    result<std::shared_ptr<stream_handle>> open_capture_stream(
        const capture_stream_config& config,
        capture_callback& callback) override {
        return win::open_capture_stream(config, callback, options_);
    }

    /// Открывает handle loopback capture stream для публичного wrapper-слоя.
    result<std::shared_ptr<stream_handle>> open_loopback_stream(
        const loopback_stream_config& config,
        capture_callback& callback) override {
        return win::open_loopback_stream(config, callback, options_);
    }

private:
    /// Сохраняет runtime options, чтобы отдельные stream handle-ы могли их учитывать.
    explicit wasapi_runtime_backend(runtime_options options) noexcept
        : options_(options) {}

    /// Кешированные runtime options для создания render/capture worker-ов.
    runtime_options options_;
    /// Отмечает, владеет ли этот экземпляр backend жизненным циклом запуска Media Foundation.
    bool media_foundation_started_ = false;
};

}  // namespace

result<std::shared_ptr<runtime_backend>> make_runtime_backend(runtime_options options) {
    return wasapi_runtime_backend::create(options);
}

}  // namespace sonotide::detail
