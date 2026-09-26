#pragma once
// big-endian readers/writers for the ITCH + MoldUDP64 wire formats.
// memcpy + bswap instead of casting the pointer: fields aren't aligned, so a
// cast is UB. the compiler turns this into one load + bswap anyway (rev on ARM)
#include <cstdint>
#include <cstring>

namespace fh {

inline uint16_t read_be16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline uint32_t read_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline uint64_t read_be64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

// ITCH timestamps are 6 bytes (ns since midnight), annoyingly not 8
inline uint64_t read_be48(const uint8_t* p) noexcept {
    return (uint64_t(read_be16(p)) << 32) | read_be32(p + 2);
}

inline void write_be16(uint8_t* p, uint16_t v) noexcept {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof v);
}

inline void write_be32(uint8_t* p, uint32_t v) noexcept {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof v);
}

inline void write_be64(uint8_t* p, uint64_t v) noexcept {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof v);
}

inline void write_be48(uint8_t* p, uint64_t v) noexcept {
    write_be16(p, uint16_t(v >> 32));
    write_be32(p + 2, uint32_t(v));
}

}  // namespace fh
