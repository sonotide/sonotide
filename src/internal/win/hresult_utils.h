#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include "sonotide/error.h"

namespace sonotide::detail::win {

[[nodiscard]] std::string utf8_from_utf16(std::wstring_view value);
[[nodiscard]] std::wstring utf16_from_utf8(std::string_view value);

[[nodiscard]] error map_hresult(
    std::string_view operation,
    HRESULT hr,
    error_category category,
    error_code code,
    bool recoverable = false);

}  // namespace sonotide::detail::win

