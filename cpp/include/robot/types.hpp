#pragma once

#include <expected>
#include <string>

namespace robot {

struct Error {
    enum class Code {
        invalid_argument,
        io,
        timeout,
        disconnected,
        protocol,
        resource_exhausted,
    };

    Code code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

} // namespace robot
