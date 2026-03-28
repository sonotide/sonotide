#include "internal/win/com_scope.h"

#include <objbase.h>

namespace sonotide::detail::win {

com_scope::~com_scope() {
    if (should_uninitialize_) {
        CoUninitialize();
    }
}

result<void> com_scope::initialize_multithreaded() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        should_uninitialize_ = true;
        return result<void>::success();
    }

    if (hr == RPC_E_CHANGED_MODE) {
        return result<void>::success();
    }

    error failure;
    failure.category = error_category::initialization;
    failure.code = error_code::initialization_failed;
    failure.operation = "CoInitializeEx";
    failure.message = "Failed to initialize COM in multithreaded mode.";
    failure.native_code = static_cast<long>(hr);
    return result<void>::failure(std::move(failure));
}

}  // namespace sonotide::detail::win

