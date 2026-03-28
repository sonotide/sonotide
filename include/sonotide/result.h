#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "sonotide/error.h"

namespace sonotide {

/// Дискриминированное объединение для значений вида value-or-error.
template <typename value_type>
class result {
public:
    /// Создаёт успешный результат, владеющий значением.
    static result success(value_type value) {
        return result(std::in_place_index<0>, std::move(value));
    }

    /// Создаёт неуспешный результат, владеющий ошибкой.
    static result failure(error failure) {
        return result(std::in_place_index<1>, std::move(failure));
    }

    /// Возвращает `true`, когда значение присутствует.
    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    /// Удобная проверка на успех.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    /// Возвращает содержащееся значение как lvalue reference.
    [[nodiscard]] value_type& value() & {
        return std::get<0>(storage_);
    }

    /// Возвращает содержащееся значение как const lvalue reference.
    [[nodiscard]] const value_type& value() const& {
        return std::get<0>(storage_);
    }

    /// Возвращает содержащееся значение как rvalue reference.
    [[nodiscard]] value_type&& value() && {
        return std::get<0>(std::move(storage_));
    }

    /// Возвращает сохранённую ошибку как lvalue reference.
    [[nodiscard]] sonotide::error& error() & {
        return std::get<1>(storage_);
    }

    /// Возвращает сохранённую ошибку как const lvalue reference.
    [[nodiscard]] const sonotide::error& error() const& {
        return std::get<1>(storage_);
    }

private:
    template <typename... args>
    explicit result(std::in_place_index_t<0>, args&&... values)
        : storage_(std::in_place_index<0>, std::forward<args>(values)...) {}

    template <typename... args>
    explicit result(std::in_place_index_t<1>, args&&... values)
        : storage_(std::in_place_index<1>, std::forward<args>(values)...) {}

    std::variant<value_type, sonotide::error> storage_;
};

/// Специализация для операций, которые возвращают только success или error.
template <>
class result<void> {
public:
    /// Создаёт успешный void result.
    static result success() {
        return result();
    }

    /// Создаёт неуспешный void result.
    static result failure(sonotide::error failure) {
        return result(std::move(failure));
    }

    /// Возвращает `true`, когда результат успешен.
    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    /// Удобная проверка на успех.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    /// Возвращает сохранённую ошибку. Вызывать только когда `has_value()` равно `false`.
    [[nodiscard]] const sonotide::error& error() const& {
        return *error_;
    }

private:
    result() = default;

    explicit result(sonotide::error failure)
        : error_(std::move(failure)) {}

    std::optional<sonotide::error> error_;
};

}  // namespace sonotide
