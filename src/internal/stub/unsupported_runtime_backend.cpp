#include "internal/runtime_backend.h"

#include <memory>
#include <string>

namespace sonotide::detail {
namespace {

/// Формирует единообразную ошибку для каждого заглушенного входа.
error make_unsupported_error(const std::string& operation) {
    /// Сообщение намеренно направляет пользователя к бэкенду Windows WASAPI.
    error failure;
    failure.category = error_category::platform;
    failure.code = error_code::unsupported_platform;
    failure.operation = operation;
    failure.message = "Sonotide currently provides a WASAPI backend and requires Windows.";
    return failure;
}

/// Реализация бэкенда, которая существует только для корректных ошибок вне Windows.
class unsupported_runtime_backend final : public runtime_backend {
public:
    /// Перечисление устройств недоступно в заглушечном бэкенде.
    result<std::vector<device_info>> enumerate_devices(device_direction) const override {
        return result<std::vector<device_info>>::failure(
            make_unsupported_error("runtime::enumerate_devices"));
    }

    /// Определение устройства по умолчанию недоступно в заглушечном бэкенде.
    result<device_info> default_device(device_direction, device_role) const override {
        return result<device_info>::failure(make_unsupported_error("runtime::default_device"));
    }

    /// Открытие render-потока недоступно в заглушечном бэкенде.
    result<std::shared_ptr<stream_handle>> open_render_stream(
        const render_stream_config&,
        render_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_render_stream"));
    }

    /// Открытие capture-потока недоступно в заглушечном бэкенде.
    result<std::shared_ptr<stream_handle>> open_capture_stream(
        const capture_stream_config&,
        capture_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_capture_stream"));
    }

    /// Открытие loopback capture-потока недоступно в заглушечном бэкенде.
    result<std::shared_ptr<stream_handle>> open_loopback_stream(
        const loopback_stream_config&,
        capture_callback&) override {
        return result<std::shared_ptr<stream_handle>>::failure(
            make_unsupported_error("runtime::open_loopback_stream"));
    }
};

}  // namespace

/// Создаёт заглушку бэкенда для платформ, отличных от Windows.
result<std::shared_ptr<runtime_backend>> make_runtime_backend(runtime_options) {
    return result<std::shared_ptr<runtime_backend>>::success(
        std::make_shared<unsupported_runtime_backend>());
}

}  // namespace sonotide::detail
