#include "sonotide/error.h"
#include "sonotide/version.h"

int main() {
    if (sonotide::version_string.empty()) {
        return 1;
    }

    // Calling an out-of-line implementation function verifies that the imported
    // target resolves the installed library without initializing Windows audio.
    return sonotide::to_string(sonotide::error_category::initialization).empty() ? 1 : 0;
}
