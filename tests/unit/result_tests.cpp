#include <string>
#include <string_view>

#include "test_support/test_harness.h"
#include "sonotide/result.h"

int main() {
    // Успешный result<int> должен хранить payload и сообщать о наличии значения.
    auto ok = sonotide::result<int>::success(42);
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 42);

    // Failure result должен сохранять исходный объект ошибки без искажений.
    sonotide::error failure;
    failure.category = sonotide::error_category::stream;
    failure.code = sonotide::error_code::stream_open_failed;
    failure.message = "boom";
    failure.operation = "unit::result_tests";
    failure.recoverable = true;
    auto bad = sonotide::result<std::string>::failure(failure);
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().message == "boom");
    REQUIRE(bad.error().recoverable);
    REQUIRE(bad.error().operation == "unit::result_tests");

    // Специализация void должна вести себя так же предсказуемо, как и типизированный result.
    auto ok_void = sonotide::result<void>::success();
    REQUIRE(ok_void.has_value());

    auto bad_void = sonotide::result<void>::failure(failure);
    REQUIRE(!bad_void.has_value());
    REQUIRE(bad_void.error().message == "boom");

    // Полный объект ошибки должен сохраняться без потерь.
    sonotide::error rich_failure;
    rich_failure.category = sonotide::error_category::callback;
    rich_failure.code = sonotide::error_code::callback_failed;
    rich_failure.message = "callback exploded";
    rich_failure.operation = "unit::result_tests::rich";
    rich_failure.recoverable = false;
    rich_failure.native_code = 42;
    auto rich_result = sonotide::result<int>::failure(rich_failure);
    REQUIRE(!rich_result.has_value());
    REQUIRE(rich_result.error().category == sonotide::error_category::callback);
    REQUIRE(rich_result.error().code == sonotide::error_code::callback_failed);
    REQUIRE(rich_result.error().native_code.has_value());
    REQUIRE(*rich_result.error().native_code == 42);

    // Строковые представления ошибок должны быть стабильными.
    REQUIRE(sonotide::to_string(sonotide::error_category::stream) == std::string_view("stream"));
    REQUIRE(sonotide::to_string(sonotide::error_code::invalid_state) == std::string_view("invalid_state"));
    return 0;
}
