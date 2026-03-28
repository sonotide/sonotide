#include "internal/win/com_scope.h"

#include <objbase.h>

namespace sonotide::detail::win {

/// Освобождает COM apartment только в том случае, если инициализацию выполнил этот объект.
com_scope::~com_scope() {
    if (should_uninitialize_) {
        CoUninitialize();
    }
}

/// Вызывает `CoInitializeEx` в multithreaded-режиме и переводит результат в `sonotide::result`.
result<void> com_scope::initialize_multithreaded() {
    // Backend ожидает многопоточный apartment, потому что рабочие потоки render/capture
    // работают из выделенных потоков, а не из STA UI loop.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        should_uninitialize_ = true;
        return result<void>::success();
    }

    // `RPC_E_CHANGED_MODE` означает, что поток уже инициализирован в другом режиме.
    // Этот случай допустим, потому что текущий COM apartment всё ещё можно безопасно использовать.
    if (hr == RPC_E_CHANGED_MODE) {
        return result<void>::success();
    }

    // Любой другой HRESULT означает жёсткий сбой инициализации для этого потока.
    error failure;
    failure.category = error_category::initialization;
    failure.code = error_code::initialization_failed;
    failure.operation = "CoInitializeEx";
    failure.message = "Failed to initialize COM in multithreaded mode.";
    failure.native_code = static_cast<long>(hr);
    return result<void>::failure(std::move(failure));
}

}  // namespace sonotide::detail::win
