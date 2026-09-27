#include "win_file.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace gufo::win {
namespace {

void MapLastError() noexcept {
  switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      errno = ENOENT;
      break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      errno = EACCES;
      break;
    case ERROR_INVALID_HANDLE:
      errno = EBADF;
      break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_NOT_SUPPORTED:
      errno = EINVAL;
      break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      errno = ENOMEM;
      break;
    default:
      errno = EIO;
      break;
  }
}

HANDLE HandleFromFd(int fd) noexcept {
  if (fd < 0) return INVALID_HANDLE_VALUE;
  const auto raw = _get_osfhandle(fd);
  return raw == -1 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(raw);
}

std::wstring PathFromFd(int fd) {
  HANDLE handle = HandleFromFd(fd);
  if (handle == INVALID_HANDLE_VALUE) return {};
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetFinalPathNameByHandleW(
      handle, buffer.data(), static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (size == 0 || size >= buffer.size()) return {};
  std::wstring path(buffer.data(), size);
  // Win32 CreateFile accepts \\?\ paths directly. Keep that prefix.
  return path;
}

int WrapHandle(HANDLE handle) noexcept {
  if (handle == INVALID_HANDLE_VALUE) return -1;
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                 _O_RDONLY | _O_BINARY);
  if (fd < 0) {
    CloseHandle(handle);
    return -1;
  }
  return fd;
}

}  // namespace

int OpenRead(const std::filesystem::path& path) {
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    MapLastError();
    return -1;
  }
  return WrapHandle(handle);
}

int DuplicateFd(int fd) {
  HANDLE source = HandleFromFd(fd);
  if (source == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    MapLastError();
    return -1;
  }
  return WrapHandle(duplicate);
}

int OpenDirectFromFd(int fd) {
  const auto path = PathFromFd(fd);
  if (path.empty()) {
    errno = ENOENT;
    return -1;
  }
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    MapLastError();
    return -1;
  }
  return WrapHandle(handle);
}

void Close(int fd) noexcept {
  if (fd >= 0) (void)_close(fd);
}

std::int64_t PRead(int fd, void* buffer, std::size_t bytes,
                   std::uint64_t offset) noexcept {
  if (bytes == 0) return 0;
  HANDLE handle = HandleFromFd(fd);
  if (handle == INVALID_HANDLE_VALUE || buffer == nullptr) {
    errno = EINVAL;
    return -1;
  }

  // ReadFile's DWORD count is 32-bit. Gufo's callers use at most 16 MiB
  // chunks, but keep this wrapper correct for larger requests too.
  std::size_t total = 0;
  auto* out = static_cast<std::uint8_t*>(buffer);
  while (total < bytes) {
    const DWORD count = static_cast<DWORD>(
        std::min<std::size_t>(bytes - total, 0x7ffff000ULL));
    struct EventHolder {
      HANDLE handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      ~EventHolder() { if (handle != nullptr) CloseHandle(handle); }
    };
    thread_local EventHolder event_holder;
    HANDLE event = event_holder.handle;
    if (event == nullptr) {
      MapLastError();
      return -1;
    }
    ResetEvent(event);
    OVERLAPPED ov{};
    const std::uint64_t absolute = offset + total;
    ov.Offset = static_cast<DWORD>(absolute & 0xffffffffULL);
    ov.OffsetHigh = static_cast<DWORD>(absolute >> 32U);
    ov.hEvent = event;
    DWORD got = 0;
    BOOL ok = ReadFile(handle, out + total, count, nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      ok = GetOverlappedResult(handle, &ov, &got, TRUE);
    } else if (ok) {
      ok = GetOverlappedResult(handle, &ov, &got, TRUE);
    }
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) {
      SetLastError(error);
      MapLastError();
      return total == 0 ? -1 : static_cast<std::int64_t>(total);
    }
    if (got == 0) break;
    total += got;
    if (got < count) break;
  }
  return static_cast<std::int64_t>(total);
}

}  // namespace gufo::win

#endif
