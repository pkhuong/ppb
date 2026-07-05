/*
 * Shared conformance I/O loop: 4-byte little-endian length-prefixed
 * ConformanceRequest/ConformanceResponse framing over stdin/stdout.
 */
#pragma once

#include "conformance/conformance.pb.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unistd.h>

namespace ppb_conformance
{

/*
 * Read exactly `len` bytes from `fd` into `buf`.  Returns false on EOF;
 * exits on a read error.
 */
inline bool
read_n(int fd, void *buf, size_t len)
{
    auto *p = static_cast<char *>(buf);

    while (len > 0)
    {
        ssize_t n = read(fd, p, len);

        if (n == 0)
        {
            return false; /* EOF */
        }

        if (n < 0)
        {
            _exit(1); /* unrecoverable read error */
        }

        p += n;
        len -= static_cast<size_t>(n);
    }

    return true;
}

/*
 * Write exactly `len` bytes from `buf` to `fd`.  Exits on error.
 */
inline void
write_n(int fd, const void *buf, size_t len)
{
    const auto *p = static_cast<const char *>(buf);

    while (len > 0)
    {
        ssize_t n = write(fd, p, len);

        if (n <= 0)
        {
            _exit(1); /* unrecoverable write error */
        }

        p += n;
        len -= static_cast<size_t>(n);
    }
}

using Handler = std::function<conformance::ConformanceResponse(const conformance::ConformanceRequest &)>;

inline int
run_loop(const Handler &handler)
{
    while (true)
    {
        /*
         * Read a 4-byte little-endian request length.  On a big-endian host
         * an explicit byte-swap (e.g. __builtin_bswap32) would be needed here;
         * that is deferred because the conformance suite currently runs only on
         * little-endian machines.
         */
        uint32_t in_len;

        if (!read_n(STDIN_FILENO, &in_len, sizeof(in_len)))
        {
            break; /* EOF: test runner is done */
        }

        std::string serialized_input;
        serialized_input.resize(in_len);

        if (!read_n(STDIN_FILENO, &serialized_input[0], in_len))
        {
            break;
        }

        conformance::ConformanceRequest request;

        if (!request.ParseFromString(serialized_input))
        {
            /* Malformed request framing: decode nothing, let the handler skip. */
            request.Clear();
        }

        conformance::ConformanceResponse response = handler(request);

        std::string serialized_output;

        if (!response.SerializeToString(&serialized_output))
        {
            _exit(1); /* a ConformanceResponse must always serialize */
        }

        uint32_t out_len = static_cast<uint32_t>(serialized_output.size());

        write_n(STDOUT_FILENO, &out_len, sizeof(out_len));
        write_n(STDOUT_FILENO, serialized_output.data(), serialized_output.size());
    }

    return 0;
}

}  // namespace ppb_conformance
