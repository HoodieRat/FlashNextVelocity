#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace gufo::win {

int OpenRead(const std::filesystem::path& path);
int DuplicateFd(int fd);
int OpenDirectFromFd(int fd);
void Close(int fd) noexcept;
std::int64_t PRead(int fd, void* buffer, std::size_t bytes,
                   std::uint64_t offset) noexcept;

}  // namespace gufo::win

#endif
