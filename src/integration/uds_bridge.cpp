#include "kvstore/integration/uds_bridge.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>

namespace kvstore::integration {
namespace {
bool ReadExact(int fd, void* data, std::size_t size) {
  auto* p = static_cast<std::byte*>(data);
  while (size != 0) {
    const auto n = ::read(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}
bool WriteAll(int fd, ByteView bytes) {
  while (!bytes.empty()) {
    const auto n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes = bytes.subspan(static_cast<std::size_t>(n));
  }
  return true;
}

int OpenSafeParent(const std::filesystem::path& parent) {
  if (!parent.is_absolute()) return -1;
  int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current < 0) return -1;
  for (const auto& component : parent) {
    if (component == "/" || component == ".") continue;
    if (component == "..") {
      ::close(current);
      return -1;
    }
    const int next =
        ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(current);
    if (next < 0) return -1;
    current = next;
    struct stat info {};
    if (::fstat(current, &info) < 0 || !S_ISDIR(info.st_mode) ||
        (info.st_uid != ::getuid() && info.st_uid != 0) ||
        ((info.st_mode & (S_IWGRP | S_IWOTH)) && !(info.st_mode & S_ISVTX))) {
      ::close(current);
      return -1;
    }
  }
  return current;
}

bool SameInode(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

void UnlinkOwnedAt(int parent_fd, const std::string& name, const struct stat& owned) noexcept {
  struct stat current {};
  if (parent_fd >= 0 && ::fstatat(parent_fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
      SameInode(current, owned)) {
    ::unlinkat(parent_fd, name.c_str(), 0);
  }
}
}  // namespace

UdsBridge::UdsBridge(kvcache::ChunkRegistry& r, kvcache::MatchIndex& i, std::string p,
                     std::string t, std::string m, std::filesystem::path disk_directory)
    : registry_(r), index_(i), path_(std::move(p)), tenant_(std::move(t)), model_(std::move(m)),
      disk_directory_(std::move(disk_directory)) {
  clients_.reserve(kMaxClients);
  client_threads_.reserve(kMaxClients);
}
UdsBridge::~UdsBridge() { Stop(); }
Status UdsBridge::Start() {
  std::lock_guard lifecycle(lifecycle_mutex_);
  if (thread_.joinable()) return {StatusCode::kAlreadyExists, "bridge already started"};
  try {
    if (path_.empty() || path_.size() >= sizeof(sockaddr_un::sun_path) ||
        path_.find('\0') != std::string::npos)
      return {StatusCode::kInvalidArgument, "invalid UDS path"};
    const auto parent = std::filesystem::path(path_).parent_path();
    const auto name = std::filesystem::path(path_).filename().string();
    if (parent.empty() || name.empty() || name == "." || name == "..")
      return {StatusCode::kInvalidArgument, "UDS path requires a safe parent directory"};
    int parent_fd = OpenSafeParent(parent);
    if (parent_fd < 0)
      return {StatusCode::kInvalidArgument, "UDS path requires a safe parent directory"};
    struct FdRollback {
      void operator()(int* p) const noexcept {
        if (p && *p >= 0) ::close(*p);
      }
    };
    std::unique_ptr<int, FdRollback> parent_rollback(&parent_fd);
    std::string prepared_name = name;
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {StatusCode::kIoError, std::strerror(errno)};
    std::unique_ptr<int, FdRollback> rollback(&fd);
    struct stat existing {};
    if (::fstatat(parent_fd, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
      return {StatusCode::kAlreadyExists, "bridge path exists"};
    if (errno != ENOENT) return {StatusCode::kIoError, std::strerror(errno)};
    const std::string anchored_path = "/proc/self/fd/" + std::to_string(parent_fd) + "/" + name;
    const std::string& bind_path =
        anchored_path.size() < sizeof(sockaddr_un::sun_path) ? anchored_path : path_;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, bind_path.data(), bind_path.size());
    const auto address_size =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + bind_path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), address_size) < 0)
      return {StatusCode::kIoError, std::strerror(errno)};
    struct stat socket_info {};
    if (::fstatat(parent_fd, name.c_str(), &socket_info, AT_SYMLINK_NOFOLLOW) < 0 ||
        !S_ISSOCK(socket_info.st_mode)) {
      // Never remove an inode that was not successfully observed as ours.
      return {StatusCode::kIoError, "bound pathname changed"};
    }
    owns_path_ = true;
    owned_dev_ = socket_info.st_dev;
    owned_ino_ = socket_info.st_ino;
    owned_parent_fd_ = parent_fd;
    owned_name_ = std::move(prepared_name);
    parent_rollback.release();
    if (::fchmodat(owned_parent_fd_, owned_name_.c_str(), S_IRUSR | S_IWUSR, 0) < 0) {
      UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
      ::close(owned_parent_fd_);
      owned_parent_fd_ = -1;
      owns_path_ = false;
      return {StatusCode::kIoError, std::strerror(errno)};
    }
    struct stat secured_info {};
    if (::fstatat(owned_parent_fd_, owned_name_.c_str(), &secured_info, AT_SYMLINK_NOFOLLOW) < 0 ||
        !SameInode(secured_info, socket_info) || (secured_info.st_mode & 0777) != 0600) {
      UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
      ::close(owned_parent_fd_);
      owned_parent_fd_ = -1;
      owns_path_ = false;
      return {StatusCode::kIoError, "failed to secure UDS pathname"};
    }
    if (::listen(fd, static_cast<int>(kMaxClients)) < 0) {
      UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
      ::close(owned_parent_fd_);
      owned_parent_fd_ = -1;
      owns_path_ = false;
      return {StatusCode::kIoError, std::strerror(errno)};
    }
    if (!disk_directory_.empty()) {
      kvcache::TieredStoreConfig config;
      config.directory = disk_directory_;
      config.resident = {.budget_bytes = 1ULL * 1024U * 1024U * 1024U,
                         .high_watermark_bytes = 1ULL * 1024U * 1024U * 1024U,
                         .low_watermark_bytes = 0};
      config.disk_budget_bytes = 1ULL * 1024U * 1024U * 1024U;
      auto opened = kvcache::TieredStore::Open(std::move(config));
      if (!opened.ok()) {
        ::close(fd);
        UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
        ::close(owned_parent_fd_);
        owned_parent_fd_ = -1;
        owns_path_ = false;
        return opened.status();
      }
      tiered_store_ = std::move(opened.value());
      auto manifests = tiered_store_->ListManifests();
      if (!manifests.ok()) {
        tiered_store_.reset();
        ::close(fd);
        UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
        ::close(owned_parent_fd_);
        owned_parent_fd_ = -1;
        owns_path_ = false;
        return manifests.status();
      }
      for (const auto& manifest : manifests.value()) {
        auto key = kvcache::CanonicalCacheKey(manifest);
        auto inserted = key.ok() ? index_.Insert(manifest, key.value()) : key.status();
        if (!inserted.ok()) {
          tiered_store_.reset();
          ::close(fd);
          UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
          ::close(owned_parent_fd_);
          owned_parent_fd_ = -1;
          owns_path_ = false;
          return {StatusCode::kCorruption,
                  std::string("failed to rebuild disk match index: ") +
                      std::string(inserted.message())};
        }
      }
    }
    listen_fd_ = fd;
    rollback.release();
    stopping_ = false;
    try {
      thread_ = std::thread(&UdsBridge::Run, this);
    } catch (...) {
      stopping_ = true;
      listen_fd_ = -1;
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
      UnlinkOwnedAt(owned_parent_fd_, owned_name_, socket_info);
      ::close(owned_parent_fd_);
      owned_parent_fd_ = -1;
      owns_path_ = false;
      return {StatusCode::kInternal, "failed to start UDS bridge thread"};
    }
    return Status::Ok();
  } catch (...) {
    return {StatusCode::kInternal, "failed to prepare UDS bridge"};
  }
}
void UdsBridge::Stop() noexcept {
  std::unique_lock lifecycle(lifecycle_mutex_);
  stopping_ = true;
  {
    std::lock_guard listener_lock(listener_mutex_);
    const int listener = listen_fd_.load();
    if (listener >= 0) ::shutdown(listener, SHUT_RDWR);
  }
  {
    std::lock_guard lock(clients_mutex_);
    for (const int client : clients_) ::shutdown(client, SHUT_RDWR);
  }
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard listener_lock(listener_mutex_);
    const int remaining_listener = listen_fd_.exchange(-1);
    if (remaining_listener >= 0) ::close(remaining_listener);
  }
  for (auto& worker : client_threads_)
    if (worker.thread.joinable()) worker.thread.join();
  client_threads_.clear();
  tiered_store_.reset();
  struct stat owned {};
  owned.st_dev = owned_dev_;
  owned.st_ino = owned_ino_;
  if (owns_path_) UnlinkOwnedAt(owned_parent_fd_, owned_name_, owned);
  if (owned_parent_fd_ >= 0) ::close(owned_parent_fd_);
  owned_parent_fd_ = -1;
  owned_name_.clear();
  owns_path_ = false;
}
void UdsBridge::Run() noexcept {
  try {
    while (!stopping_) {
      std::thread retired;
      {
        std::lock_guard lock(clients_mutex_);
        auto free = std::find_if(client_threads_.begin(), client_threads_.end(),
                                 [](const Worker& w) { return w.done && w.done->load(); });
        if (free != client_threads_.end()) {
          retired = std::move(free->thread);
          client_threads_.erase(free);
        }
      }
      if (retired.joinable()) retired.join();
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        if (stopping_) break;
        continue;
      }
      try {
        {
          std::lock_guard lock(clients_mutex_);
          if (clients_.size() >= kMaxClients || client_threads_.size() >= kMaxClients) {
            ::close(client);
            continue;
          }
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard lock(clients_mutex_);
        if (clients_.size() >= kMaxClients || client_threads_.size() >= kMaxClients) {
          ::close(client);
          continue;
        }
        clients_.push_back(client);
        client_threads_.emplace_back();
        auto& record = client_threads_.back();
        try {
          record.done = done;
          record.thread = std::thread([this, client, done] {
            HandleClient(client);
            done->store(true);
          });
        } catch (...) {
          client_threads_.pop_back();
          clients_.pop_back();
          ::close(client);
          stopping_ = true;
          continue;
        }
      } catch (...) {
        // This also covers allocation failure before the client is registered.
        ::close(client);
        if (!stopping_) stopping_ = true;
      }
    }
  } catch (...) {
    stopping_ = true;
  }
  {
    std::lock_guard listener_lock(listener_mutex_);
    const int listener = listen_fd_.exchange(-1);
    if (listener >= 0) {
      ::shutdown(listener, SHUT_RDWR);
      ::close(listener);
    }
  }
  std::lock_guard lock(clients_mutex_);
  for (const int client : clients_) ::shutdown(client, SHUT_RDWR);
}
void UdsBridge::HandleClient(int client) noexcept {
  try {
    // The bridge trusts same-UID local framework processes; this is the
    // explicit authentication boundary for the 0600 socket.
    struct ucred peer {};
    socklen_t peer_size = sizeof(peer);
    if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 ||
        peer.uid != ::getuid()) {
      ::shutdown(client, SHUT_RDWR);
      {
        std::lock_guard lock(clients_mutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
      }
      ::close(client);
      return;
    }
    Session session(registry_, index_, tenant_, model_, tiered_store_.get());
    while (!stopping_) {
      std::array<std::byte, 4> size_bytes{};
      if (!ReadExact(client, size_bytes.data(), size_bytes.size())) break;
      const std::uint32_t size = std::to_integer<std::uint32_t>(size_bytes[0]) |
                                 (std::to_integer<std::uint32_t>(size_bytes[1]) << 8U) |
                                 (std::to_integer<std::uint32_t>(size_bytes[2]) << 16U) |
                                 (std::to_integer<std::uint32_t>(size_bytes[3]) << 24U);
      if (size == 0 || size > Codec::kMaxFrame) break;
      Bytes request(size);
      if (!ReadExact(client, request.data(), request.size())) break;
      auto response = session.Exchange(request);
      if (!response.ok() || response.value().size() > Codec::kMaxFrame) break;
      const auto response_size = static_cast<std::uint32_t>(response.value().size());
      std::array<std::byte, sizeof(response_size)> response_size_bytes{};
      for (std::size_t i = 0; i < size_bytes.size(); ++i)
        response_size_bytes[i] = std::byte((response_size >> (8U * i)) & 0xffU);
      if (!WriteAll(client, response_size_bytes) || !WriteAll(client, response.value())) break;
    }
    session.Disconnect();
  } catch (...) {
  }
  {
    std::lock_guard lock(clients_mutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
  }
  ::shutdown(client, SHUT_RDWR);
  ::close(client);
}
}  // namespace kvstore::integration
