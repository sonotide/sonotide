#include <iostream>

#include "sonotide/runtime.h"

int main() {
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }

    sonotide::runtime audio_runtime = std::move(runtime_result.value());
    auto devices_result = audio_runtime.enumerate_devices(sonotide::device_direction::render);
    if (!devices_result) {
        std::cerr << devices_result.error().message << '\n';
        return 1;
    }

    for (const auto& device : devices_result.value()) {
        std::cout << device.id << " | " << device.friendly_name;
        if (device.is_default_multimedia) {
            std::cout << " | default multimedia";
        }
        std::cout << '\n';
    }

    return 0;
}

