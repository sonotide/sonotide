#pragma once

#include <windows.h>

#include "sonotide/result.h"

namespace sonotide::detail::win {

class com_scope {
public:
    com_scope() = default;
    ~com_scope();

    com_scope(const com_scope&) = delete;
    com_scope& operator=(const com_scope&) = delete;

    [[nodiscard]] result<void> initialize_multithreaded();

private:
    bool should_uninitialize_ = false;
};

}  // namespace sonotide::detail::win

