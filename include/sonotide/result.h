#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "sonotide/error.h"

namespace sonotide {

template <typename value_type>
class result {
public:
    static result success(value_type value) {
        return result(std::in_place_index<0>, std::move(value));
    }

    static result failure(error failure) {
        return result(std::in_place_index<1>, std::move(failure));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] value_type& value() & {
        return std::get<0>(storage_);
    }

    [[nodiscard]] const value_type& value() const& {
        return std::get<0>(storage_);
    }

    [[nodiscard]] value_type&& value() && {
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] sonotide::error& error() & {
        return std::get<1>(storage_);
    }

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

template <>
class result<void> {
public:
    static result success() {
        return result();
    }

    static result failure(sonotide::error failure) {
        return result(std::move(failure));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

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

