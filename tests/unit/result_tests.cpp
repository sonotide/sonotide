#include <cassert>
#include <string>

#include "sonotide/result.h"

int main() {
    auto ok = sonotide::result<int>::success(42);
    assert(ok.has_value());
    assert(ok.value() == 42);

    sonotide::error failure;
    failure.message = "boom";
    auto bad = sonotide::result<std::string>::failure(failure);
    assert(!bad.has_value());
    assert(bad.error().message == "boom");

    auto ok_void = sonotide::result<void>::success();
    assert(ok_void.has_value());

    auto bad_void = sonotide::result<void>::failure(failure);
    assert(!bad_void.has_value());
    assert(bad_void.error().message == "boom");
    return 0;
}

