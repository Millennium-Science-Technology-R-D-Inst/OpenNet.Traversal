#pragma once

#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace opennet
{
    class unique_socket
    {
    public:
        unique_socket() noexcept = default;
        explicit unique_socket(int handle) noexcept : handle_{handle} {}
        ~unique_socket() { reset(); }

        unique_socket(unique_socket const&) = delete;
        unique_socket& operator=(unique_socket const&) = delete;

        unique_socket(unique_socket&& other) noexcept : handle_{other.release()} {}
        unique_socket& operator=(unique_socket&& other) noexcept
        {
            if (this != &other)
                reset(other.release());
            return *this;
        }

        [[nodiscard]] int get() const noexcept { return handle_; }
        [[nodiscard]] explicit operator bool() const noexcept { return handle_ >= 0; }

        [[nodiscard]] int release() noexcept
        {
            int result = handle_;
            handle_ = -1;
            return result;
        }

        void reset(int replacement = -1) noexcept
        {
            if (handle_ >= 0)
                ::close(handle_);
            handle_ = replacement;
        }

    private:
        int handle_{-1};
    };

    inline void throw_socket_error(char const* operation)
    {
        throw std::system_error(errno, std::generic_category(), operation);
    }
}
