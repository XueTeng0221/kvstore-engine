#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <thread>

namespace kvstore {

using OwnedThread = std::jthread;

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { Reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) Reset(other.Release());
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int Release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void Reset(int fd = -1) noexcept {
    if (fd_ >= 0) static_cast<void>(::close(fd_));
    fd_ = fd;
  }

 private:
  int fd_{-1};
};

class MappedRegion {
 public:
  MappedRegion() = default;
  MappedRegion(void* address, std::size_t size) noexcept : address_(address), size_(size) {}
  ~MappedRegion() { Reset(); }
  MappedRegion(const MappedRegion&) = delete;
  MappedRegion& operator=(const MappedRegion&) = delete;
  MappedRegion(MappedRegion&& other) noexcept : address_(other.address_), size_(other.size_) {
    other.address_ = MAP_FAILED;
    other.size_ = 0;
  }
  MappedRegion& operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
      Reset();
      address_ = other.address_;
      size_ = other.size_;
      other.address_ = MAP_FAILED;
      other.size_ = 0;
    }
    return *this;
  }
  [[nodiscard]] void* data() const noexcept { return address_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return address_ != MAP_FAILED && address_ != nullptr;
  }
  void Reset() noexcept {
    if (*this) static_cast<void>(::munmap(address_, size_));
    address_ = MAP_FAILED;
    size_ = 0;
  }

 private:
  void* address_{MAP_FAILED};
  std::size_t size_{0};
};

}  // namespace kvstore
