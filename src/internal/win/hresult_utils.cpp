#include "internal/win/hresult_utils.h"

namespace sonotide::detail::win {

std::string utf8_from_utf16(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        utf8.data(),
        size,
        nullptr,
        nullptr);
    return utf8;
}

std::wstring utf16_from_utf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }

    std::wstring utf16(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        utf16.data(),
        size);
    return utf16;
}

error map_hresult(
    const std::string_view operation,
    const HRESULT hr,
    const error_category category,
    const error_code code,
    const bool recoverable) {
    error failure;
    failure.category = category;
    failure.code = code;
    failure.operation = std::string(operation);
    failure.native_code = static_cast<long>(hr);
    failure.recoverable = recoverable;
    failure.message = "Windows API call failed with HRESULT 0x" + std::to_string(
        static_cast<unsigned long>(hr)) + ".";
    return failure;
}

}  // namespace sonotide::detail::win

