#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include "sonotide/error.h"

namespace sonotide::detail::win {

/// Переводит UTF-16 view в UTF-8 строку для публичных сообщений и логов.
[[nodiscard]] std::string utf8_from_utf16(std::wstring_view value);
/// Переводит UTF-8 view в UTF-16 строку для WinAPI / COM вызовов.
[[nodiscard]] std::wstring utf16_from_utf8(std::string_view value);

/// Нормализует `HRESULT` в доменный `sonotide::error`.
[[nodiscard]] error map_hresult(
    std::string_view operation,
    HRESULT hr,
    error_category category,
    error_code code,
    bool recoverable = false);

}  // namespace sonotide::detail::win
