#pragma once

#include <memory>

#include "sonotide/result.h"
#include "sonotide/stream_status.h"

namespace sonotide {
namespace detail {
class stream_handle;
}

class capture_stream {
public:
    capture_stream() = default;
    ~capture_stream() = default;

    capture_stream(capture_stream&&) noexcept = default;
    capture_stream& operator=(capture_stream&&) noexcept = default;

    capture_stream(const capture_stream&) = delete;
    capture_stream& operator=(const capture_stream&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    result<void> start();
    result<void> stop();
    result<void> reset();
    result<void> close();
    [[nodiscard]] stream_status status() const;

private:
    explicit capture_stream(std::shared_ptr<detail::stream_handle> handle) noexcept;

    std::shared_ptr<detail::stream_handle> handle_;

    friend class runtime;
};

}  // namespace sonotide

