#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace gufo::win {

int OpenRead(const std::filesystem::path& path);
int DuplicateFd(int fd);
int OpenDirectFromFd(int fd);
void Close(int fd) noexcept;
std::optional<std::uint64_t> FileSize(int fd) noexcept;
void* MapReadOnly(int fd, std::size_t bytes) noexcept;
void Unmap(void* address, std::size_t bytes = 0) noexcept;
void Prefetch(void* address, std::size_t bytes) noexcept;
std::int64_t PRead(int fd, void* buffer, std::size_t bytes,
                   std::uint64_t offset) noexcept;

}  // namespace gufo::win

#endif
