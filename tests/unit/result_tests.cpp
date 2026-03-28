#include <cassert>
#include <string>

#include "sonotide/result.h"

int main() {
    // Успешный result<int> должен хранить payload и сообщать о наличии значения.
    auto ok = sonotide::result<int>::success(42);
    assert(ok.has_value());
    assert(ok.value() == 42);

    // Failure result должен сохранять исходный объект ошибки без искажений.
    sonotide::error failure;
    failure.message = "boom";
    auto bad = sonotide::result<std::string>::failure(failure);
    assert(!bad.has_value());
    assert(bad.error().message == "boom");

    // Специализация void должна вести себя так же предсказуемо, как и типизированный result.
    auto ok_void = sonotide::result<void>::success();
    assert(ok_void.has_value());

    auto bad_void = sonotide::result<void>::failure(failure);
    assert(!bad_void.has_value());
    assert(bad_void.error().message == "boom");
    return 0;
}
