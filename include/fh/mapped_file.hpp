#pragma once
// read-only mmap of a file. lets us "read" a multi-GB ITCH file without
// copying it into our own memory, the OS just pages it in as we go
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fh {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("cannot open " + path);
        struct stat st {};
        if (::fstat(fd_, &st) != 0) throw std::runtime_error("cannot stat " + path);
        size_ = size_t(st.st_size);
        if (size_ == 0) return;
        int flags = MAP_PRIVATE;
#if defined(__linux__)
        flags |= MAP_POPULATE;  // fault everything in now so it doesn't pollute the timings
#endif
        void* p = ::mmap(nullptr, size_, PROT_READ, flags, fd_, 0);
        if (p == MAP_FAILED) throw std::runtime_error("mmap failed for " + path);
        data_ = static_cast<const uint8_t*>(p);
        ::madvise(const_cast<uint8_t*>(data_), size_, MADV_SEQUENTIAL);
    }

    ~MappedFile() {
        if (data_) ::munmap(const_cast<uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    int fd_ = -1;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace fh
