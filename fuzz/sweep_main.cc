/*
 * Deterministic truncation + single-byte-mutation sweep driver.
 *
 * Links against any libFuzzer harness TU built with
 * -fsanitize=fuzzer-no-link and feeds it, for every input file named
 * on the command line: the file itself, every truncated prefix, and
 * every single-byte mutation (each byte XORed with each of the 255
 * non-zero values).
 *
 * This is just a dumb fuzzing harness that deterministically and
 * exhaustively applies mutations to input files.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace
{

std::vector<uint8_t>
read_file(const char *path)
{
    FILE *fp = std::fopen(path, "rb");
    if (fp == nullptr)
    {
        std::fprintf(stderr, "sweep: cannot open %s\n", path);
        std::exit(1);
    }

    std::vector<uint8_t> contents;
    uint8_t chunk[4096];

    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), fp)) > 0;)
    {
        contents.insert(contents.end(), chunk, chunk + got);
    }

    if (std::ferror(fp))
    {
        std::fprintf(stderr, "sweep: error reading %s\n", path);
        std::exit(1);
    }

    std::fclose(fp);
    return contents;
}

/* Returns the number of harness invocations for this input. */
size_t
sweep_one(const std::vector<uint8_t> &input)
{
    size_t execs = 0;

    for (size_t len = 0; len <= input.size(); len++)
    {
        LLVMFuzzerTestOneInput(input.data(), len);
        execs++;
    }

    std::vector<uint8_t> mutated = input;

    for (size_t i = 0; i < mutated.size(); i++)
    {
        for (unsigned delta = 1; delta < 256; delta++)
        {
            mutated[i] = input[i] ^ static_cast<uint8_t>(delta);
            LLVMFuzzerTestOneInput(mutated.data(), mutated.size());
            execs++;
        }

        mutated[i] = input[i];
    }

    return execs;
}

}  // namespace

int
main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s INPUT_FILE...\n", argv[0]);
        return 1;
    }

    size_t total_execs = 0;
    size_t total_bytes = 0;

    for (int arg = 1; arg < argc; arg++)
    {
        const std::vector<uint8_t> input = read_file(argv[arg]);

        total_execs += sweep_one(input);
        total_bytes += input.size();
    }

    std::printf("sweep: %d files (%zu bytes), %zu executions, all clean\n", argc - 1, total_bytes,
        total_execs);
    return 0;
}
