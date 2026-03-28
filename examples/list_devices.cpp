#include <iostream>

#include "sonotide/runtime.h"

int main() {
    // Создаём среду выполнения, чтобы перечисление устройств шло через реальную реализацию Sonotide.
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }

    sonotide::runtime audio_runtime = std::move(runtime_result.value());
    // Для этого примера интересны именно выходные устройства рендеринга.
    auto devices_result = audio_runtime.enumerate_devices(sonotide::device_direction::render);
    if (!devices_result) {
        std::cerr << devices_result.error().message << '\n';
        return 1;
    }

    // Печатаем короткую строку на каждое устройство, чтобы пример был удобен как быстрая проверка.
    for (const auto& device : devices_result.value()) {
        std::cout << device.id << " | " << device.friendly_name;
        if (device.is_default_multimedia) {
            std::cout << " | default multimedia";
        }
        std::cout << '\n';
    }

    return 0;
}
