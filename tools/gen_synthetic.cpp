// writes a fake ITCH file so you can try `replay` without the real data
// usage: gen_synthetic <out.itch> [n_messages=5000000] [n_symbols=500]
#include <cstdio>
#include <cstdlib>

#include "fh/synthetic.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.itch> [n_messages] [n_symbols]\n", argv[0]);
        return 1;
    }
    const size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
    const int syms = argc > 3 ? std::atoi(argv[3]) : 500;
    auto data = fh::generate_synthetic(n, syms);
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror("fopen"); return 1; }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    std::printf("wrote %zu messages (%.1f MB) to %s\n", n, data.size() / 1e6, argv[1]);
    return 0;
}
