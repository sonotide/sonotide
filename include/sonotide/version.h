#pragma once

#include <string_view>

namespace sonotide {

/// Мажорная версия пакета Sonotide.
inline constexpr int version_major = 0;
/// Минорная версия пакета Sonotide.
inline constexpr int version_minor = 3;
/// Патч-версия пакета Sonotide.
inline constexpr int version_patch = 0;
/// Человекочитаемая semantic version строка.
inline constexpr std::string_view version_string = "0.3.0";

}  // namespace sonotide
