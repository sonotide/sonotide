#include "internal/win/hresult_utils.h"

#include <limits>

namespace sonotide::detail::win {

/// Преобразует UTF-16 view в UTF-8 для человекочитаемых сообщений и публичных API.
std::string utf8_from_utf16(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int input_size = static_cast<int>(value.size());

    // Сначала запрашиваем у Windows нужный размер буфера, затем выделяем память один раз.
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size), '\0');
    const int converted_size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        utf8.data(),
        size,
        nullptr,
        nullptr);
    if (converted_size != size) {
        return {};
    }
    return utf8;
}

/// Преобразует UTF-8 view в UTF-16 для вызовов WinAPI, которые ожидают wide strings.
std::wstring utf16_from_utf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int input_size = static_cast<int>(value.size());

    // Зеркалирование пути UTF-16 -> UTF-8 делает конвертацию предсказуемой и локализованной.
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }

    std::wstring utf16(static_cast<std::size_t>(size), L'\0');
    const int converted_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        utf16.data(),
        size);
    if (converted_size != size) {
        return {};
    }
    return utf16;
}

/// Строит объект ошибки Sonotide на основе нативного Windows HRESULT.
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
    // Сообщение сохраняет HRESULT видимым даже тогда, когда верхние уровни не смотрят на native_code.
    failure.message = "Windows API call failed with HRESULT 0x" + std::to_string(
        static_cast<unsigned long>(hr)) + ".";
    return failure;
}

}  // namespace sonotide::detail::win
