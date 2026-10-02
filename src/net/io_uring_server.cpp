#include "kvstore/net/io_uring_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

namespace kvstore {
namespace {

constexpr std::size_t kReadBytes = 8192;
constexpr std::uint64_t kAcceptTag = 1;
constexpr std::uint64_t kCancelTag = std::numeric_limits<std::uint64_t>::max();

Status ProbeFailure(const char* operation, int error) {
  if (error == EPERM || error == ENOSYS)
    return {StatusCode::kUnsupported,
            std::string(operation) + " unavailable: " + std::strerror(error)};
  return {StatusCode::kIoError, std::string(operation) + " failed: " + std::strerror(error)};
}

bool Enabled(const ProtocolConfig& config, std::string_view name) {
  return std::find(config.enabled.begin(), config.enabled.end(), name) != config.enabled.end();
}

class Ring final {
 public:
  ~Ring() { Close(); }

  [[nodiscard]] Status Open(std::size_t requested, bool sqpoll) {
#ifdef __linux__
    if (requested == 0 || requested > 4096)
      return {StatusCode::kInvalidArgument, "io_uring queue depth must be in [1, 4096]"};
    io_uring_params params{};
    if (sqpoll) params.flags |= IORING_SETUP_SQPOLL;
    fd_ =
        static_cast<int>(::syscall(__NR_io_uring_setup, static_cast<unsigned>(requested), &params));
    if (fd_ < 0) return ProbeFailure("io_uring_setup", errno);
    constexpr std::size_t kProbeCount = 256;
    std::vector<std::byte> probe_bytes(sizeof(io_uring_probe) +
                                       (kProbeCount - 1U) * sizeof(io_uring_probe_op));
    auto* probe = reinterpret_cast<io_uring_probe*>(probe_bytes.data());
    probe->ops_len = static_cast<__u8>(kProbeCount);
    if (::syscall(__NR_io_uring_register, fd_, IORING_REGISTER_PROBE, probe,
                  static_cast<unsigned>(kProbeCount)) < 0)
      return Fail("io_uring_register probe");
    const auto supported = [&](std::uint8_t operation) {
      for (std::uint8_t index = 0; index < probe->ops_len; ++index) {
        const auto& entry = probe->ops[index];
        if (entry.op == static_cast<std::uint8_t>(operation))
          return (entry.flags & IO_URING_OP_SUPPORTED) != 0;
      }
      return false;
    };
    if (!supported(IORING_OP_ACCEPT) || !supported(IORING_OP_RECV) || !supported(IORING_OP_SEND) ||
        !supported(IORING_OP_ASYNC_CANCEL))
      return Unsupported("required io_uring operations are unavailable");
    sq_entries_ = params.sq_entries;
    cq_entries_ = params.cq_entries;
    const auto sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
    const auto cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    const bool single = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
    const auto ring_size = single ? std::max(sq_ring_size, cq_ring_size) : sq_ring_size;
    sq_ring_size_ = ring_size;
    cq_ring_size_ = cq_ring_size;
    sq_ring_ = ::mmap(nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_,
                      IORING_OFF_SQ_RING);
    if (sq_ring_ == MAP_FAILED) return Fail("mmap SQ ring");
    if (single) {
      cq_ring_ = sq_ring_;
    } else {
      cq_ring_ = ::mmap(nullptr, cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                        fd_, IORING_OFF_CQ_RING);
      if (cq_ring_ == MAP_FAILED) return Fail("mmap CQ ring");
    }
    sqes_ = static_cast<io_uring_sqe*>(::mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe),
                                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                              fd_, IORING_OFF_SQES));
    if (sqes_ == MAP_FAILED) return Fail("mmap SQEs");
    sq_head_ = Ptr<std::uint32_t>(sq_ring_, params.sq_off.head);
    sq_tail_ = Ptr<std::uint32_t>(sq_ring_, params.sq_off.tail);
    sq_mask_ = Ptr<std::uint32_t>(sq_ring_, params.sq_off.ring_mask);
    sq_array_ = Ptr<std::uint32_t>(sq_ring_, params.sq_off.array);
    cq_head_ = Ptr<std::uint32_t>(cq_ring_, params.cq_off.head);
    cq_tail_ = Ptr<std::uint32_t>(cq_ring_, params.cq_off.tail);
    cq_mask_ = Ptr<std::uint32_t>(cq_ring_, params.cq_off.ring_mask);
    cq_overflow_ = Ptr<std::uint32_t>(cq_ring_, params.cq_off.overflow);
    sq_flags_ = Ptr<std::uint32_t>(sq_ring_, params.sq_off.flags);
    sqpoll_ = sqpoll;
    cqes_ = Ptr<io_uring_cqe>(cq_ring_, params.cq_off.cqes);
    return Status::Ok();
#else
    static_cast<void>(requested);
    static_cast<void>(sqpoll);
    return {StatusCode::kUnsupported, "io_uring requires Linux"};
#endif
  }

  [[nodiscard]] io_uring_sqe* GetSqe() {
    const auto head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    const auto tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);
    if (tail - head >= sq_entries_) return nullptr;
    auto& sqe = sqes_[tail & *sq_mask_];
    std::memset(&sqe, 0, sizeof(sqe));
    sq_array_[tail & *sq_mask_] = tail & *sq_mask_;
    pending_tail_ = tail + 1U;
    return &sqe;
  }

  void Publish() noexcept {
    if (pending_tail_ != 0) {
      __atomic_store_n(sq_tail_, pending_tail_, __ATOMIC_RELEASE);
      pending_tail_ = 0;
    }
  }

  [[nodiscard]] Status Submit() {
    const auto head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    const auto tail = __atomic_load_n(sq_tail_, __ATOMIC_ACQUIRE);
    const auto pending = tail - head;
    if (pending == 0) return Status::Ok();
    std::uint32_t submitted = 0;
    while (submitted < pending) {
      unsigned enter_flags = IORING_ENTER_GETEVENTS;
      if (sqpoll_ && sq_flags_ != nullptr &&
          (__atomic_load_n(sq_flags_, __ATOMIC_ACQUIRE) & IORING_SQ_NEED_WAKEUP) != 0)
        enter_flags |= IORING_ENTER_SQ_WAKEUP;
      const auto result = static_cast<int>(
          ::syscall(__NR_io_uring_enter, fd_, pending - submitted, 0U, enter_flags, nullptr, 0));
      if (result < 0) {
        if (errno == EINTR) continue;
        return ProbeFailure("io_uring_enter", errno);
      }
      if (result == 0) {
        if (sqpoll_) return Status::Ok();
        return {StatusCode::kIoError, "io_uring_enter submitted no SQEs"};
      }
      submitted += static_cast<std::uint32_t>(result);
    }
    return Status::Ok();
  }

  template <typename F>
  void Drain(F&& callback) {
    auto head = __atomic_load_n(cq_head_, __ATOMIC_RELAXED);
    const auto tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    while (head != tail) {
      const auto cqe = cqes_[head & *cq_mask_];
      callback(cqe);
      ++head;
    }
    __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
  }

  [[nodiscard]] int fd() const noexcept { return fd_; }
  void CloseNow() noexcept { Close(); }
  [[nodiscard]] bool HasOverflow() const noexcept {
    return cq_overflow_ != nullptr && __atomic_load_n(cq_overflow_, __ATOMIC_ACQUIRE) != 0;
  }

  [[nodiscard]] Status Cancel(std::span<const std::uint64_t> ids) {
    for (const auto id : ids) {
      auto* sqe = GetSqe();
      if (sqe == nullptr) return {StatusCode::kBusy, "io_uring SQ full during cancellation"};
      sqe->opcode = IORING_OP_ASYNC_CANCEL;
      sqe->addr = id;
      sqe->user_data = kCancelTag;
      Publish();
      const auto status = Submit();
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }

 private:
  template <typename T>
  static T* Ptr(void* base, std::uint32_t offset) {
    return reinterpret_cast<T*>(static_cast<char*>(base) + offset);
  }

  Status Fail(const char* operation) {
    const auto error = errno;
    Close();
    return ProbeFailure(operation, error);
  }

  Status Unsupported(const char* message) {
    Close();
    return {StatusCode::kUnsupported, message};
  }

  void Close() noexcept {
    if (sqes_ != nullptr && sqes_ != MAP_FAILED) {
      ::munmap(sqes_, static_cast<std::size_t>(sq_entries_) * sizeof(io_uring_sqe));
      sqes_ = nullptr;
    }
    if (cq_ring_ != nullptr && cq_ring_ != MAP_FAILED && cq_ring_ != sq_ring_)
      ::munmap(cq_ring_, cq_ring_size_);
    if (sq_ring_ != nullptr && sq_ring_ != MAP_FAILED) ::munmap(sq_ring_, sq_ring_size_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    sq_ring_ = nullptr;
    cq_ring_ = nullptr;
  }

  int fd_{-1};
  std::uint32_t sq_entries_{};
  std::uint32_t cq_entries_{};
  std::size_t sq_ring_size_{};
  std::size_t cq_ring_size_{};
  void* sq_ring_{nullptr};
  void* cq_ring_{nullptr};
  io_uring_sqe* sqes_{nullptr};
  io_uring_cqe* cqes_{nullptr};
  std::uint32_t* sq_head_{nullptr};
  std::uint32_t* sq_flags_{nullptr};
  std::uint32_t* sq_tail_{nullptr};
  std::uint32_t* sq_mask_{nullptr};
  std::uint32_t* sq_array_{nullptr};
  std::uint32_t* cq_head_{nullptr};
  std::uint32_t* cq_tail_{nullptr};
  std::uint32_t* cq_mask_{nullptr};
  std::uint32_t* cq_overflow_{nullptr};
  std::uint32_t pending_tail_{0};
  bool sqpoll_{false};
};

struct Request {
  enum class Kind { kAccept, kRecv, kSend } kind;
  int fd{-1};
  std::uint64_t generation{0};
  std::string payload;
  std::size_t offset{0};
  std::array<char, kReadBytes> buffer{};
};

struct Client {
  enum class Mode { kUnknown, kResp, kNative, kBatch } mode{Mode::kUnknown};
  Client(const ProtocolConfig& config, std::uint64_t client_generation)
      : parser(config.max_frame_bytes),
        native(config.max_key_bytes, config.max_value_bytes, config.max_frame_bytes),
        batch(config.max_batch_records, config.max_frame_bytes, config.max_frame_bytes,
              config.max_key_bytes, config.max_value_bytes),
        generation(client_generation) {}
  RespParser parser;
  NativeParser native;
  BatchParser batch;
  std::string probe;
  std::string output;
  std::chrono::steady_clock::time_point last_activity{std::chrono::steady_clock::now()};
  std::optional<std::chrono::steady_clock::time_point> parse_started;
  bool recv_pending{false};
  bool send_pending{false};
  std::uint64_t generation{1};
};

}  // namespace

IoUringServer::IoUringServer(const ServerConfig& config, const ProtocolConfig& protocol,
                             Dispatcher& dispatcher, std::string replication_role,
                             std::string replication_node_id, std::string replication_upstream,
                             ReplicationBacklog* replication_backlog,
                             std::uint64_t replication_heartbeat_ms, std::size_t max_events,
                             IReplicationExecutor* replication_executor, bool allow_peer_cycles,
                             std::size_t queue_depth, bool sqpoll, bool allow_fallback)
    : server_config_(config),
      protocol_config_(protocol),
      dispatcher_(dispatcher),
      replication_role_(std::move(replication_role)),
      replication_node_id_(std::move(replication_node_id)),
      replication_upstream_(std::move(replication_upstream)),
      replication_backlog_(replication_backlog),
      replication_heartbeat_ms_(replication_heartbeat_ms),
      max_events_(max_events),
      replication_executor_(replication_executor),
      allow_peer_cycles_(allow_peer_cycles),
      queue_depth_(queue_depth),
      sqpoll_(sqpoll),
      allow_fallback_(allow_fallback) {}

IoUringServer::~IoUringServer() { Stop(); }

Status IoUringServer::Probe(std::size_t queue_depth, bool sqpoll) noexcept {
  Ring ring;
  return ring.Open(queue_depth, sqpoll);
}

Status IoUringServer::Run() {
  state_.store(ServerState::kStarting, std::memory_order_release);
  std::unordered_map<int, Client> clients;
  std::unordered_map<std::uint64_t, std::unique_ptr<Request>> requests;
  Ring ring;
  const auto probe = ring.Open(queue_depth_, sqpoll_);
  if (!probe.ok()) {
    if (!allow_fallback_) {
      state_.store(ServerState::kFailed, std::memory_order_release);
      return probe;
    }
    auto fallback = std::make_shared<EpollServer>(
        server_config_, protocol_config_, dispatcher_, replication_role_, replication_node_id_,
        replication_upstream_, replication_backlog_, replication_heartbeat_ms_, max_events_,
        replication_executor_, allow_peer_cycles_);
    {
      std::lock_guard lock(fallback_mutex_);
      fallback_ = fallback;
    }
    if (stopping_.load(std::memory_order_acquire)) fallback->Stop();
    const auto status = fallback->Run();
    state_.store(fallback->state(), std::memory_order_release);
    return status;
  }

  const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "socket failed"};
  }
  int reuse = 1;
  static_cast<void>(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config_.port);
  if (::inet_pton(AF_INET, server_config_.listen_address.c_str(), &address.sin_addr) != 1 ||
      ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(listener, 256) < 0) {
    ::close(listener);
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "bind/listen failed"};
  }

  std::uint64_t next_request = 2;
  std::uint64_t next_generation = 1;
  auto submit_accept = [&]() -> Status {
    auto* sqe = ring.GetSqe();
    if (sqe == nullptr) return {StatusCode::kBusy, "io_uring SQ full"};
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = listener;
    sqe->accept_flags = SOCK_CLOEXEC;
    sqe->user_data = kAcceptTag;
    ring.Publish();
    return ring.Submit();
  };
  const auto initial_accept = submit_accept();
  if (!initial_accept.ok()) {
    ::close(listener);
    state_.store(ServerState::kFailed, std::memory_order_release);
    return initial_accept;
  }
  state_.store(ServerState::kReady, std::memory_order_release);

  auto close_client = [&](int fd) {
    ::close(fd);
    clients.erase(fd);
  };
  auto submit_recv = [&](int fd) -> Status {
    auto& client = clients.at(fd);
    if (client.recv_pending) return Status::Ok();
    auto request = std::make_unique<Request>(Request{
        Request::Kind::kRecv, fd, client.generation, {}, 0, std::array<char, kReadBytes>{}});
    const auto id = next_request++;
    auto* sqe = ring.GetSqe();
    if (sqe == nullptr) return {StatusCode::kBusy, "io_uring SQ full"};
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<std::uint64_t>(request->buffer.data());
    sqe->len = static_cast<unsigned>(request->buffer.size());
    sqe->user_data = id;
    client.recv_pending = true;
    requests.emplace(id, std::move(request));
    ring.Publish();
    return ring.Submit();
  };
  auto submit_send = [&](int fd) -> Status {
    auto& client = clients.at(fd);
    if (client.send_pending || client.output.empty()) return Status::Ok();
    auto request =
        std::make_unique<Request>(Request{Request::Kind::kSend, fd, client.generation,
                                          client.output, 0, std::array<char, kReadBytes>{}});
    const auto id = next_request++;
    auto* sqe = ring.GetSqe();
    if (sqe == nullptr) return {StatusCode::kBusy, "io_uring SQ full"};
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<std::uint64_t>(request->payload.data());
    sqe->len = static_cast<unsigned>(request->payload.size());
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = id;
    client.send_pending = true;
    requests.emplace(id, std::move(request));
    ring.Publish();
    return ring.Submit();
  };

  Status loop_error = Status::Ok();
  while (!stopping_.load(std::memory_order_acquire)) {
    struct pollfd descriptor {
      ring.fd(), POLLIN, 0
    };
    static_cast<void>(::poll(&descriptor, 1, 100));
    ring.Drain([&](const io_uring_cqe& cqe) {
      if (cqe.user_data == kAcceptTag) {
        if (cqe.res >= 0) {
          if (clients.size() >= server_config_.max_connections) {
            ::close(cqe.res);
          } else {
            clients.emplace(cqe.res, Client(protocol_config_, next_generation++));
            const auto status = submit_recv(cqe.res);
            if (!status.ok()) loop_error = status;
          }
        }
        const auto status = submit_accept();
        if (!status.ok()) loop_error = status;
        return;
      }
      auto iterator = requests.find(cqe.user_data);
      if (iterator == requests.end()) return;
      auto request = std::move(iterator->second);
      requests.erase(iterator);
      auto client = clients.find(request->fd);
      if (client == clients.end() || client->second.generation != request->generation) return;
      if (request->kind == Request::Kind::kRecv) {
        client->second.recv_pending = false;
        if (cqe.res <= 0 || static_cast<std::size_t>(cqe.res) > request->buffer.size()) {
          close_client(request->fd);
          return;
        }
        client->second.last_activity = std::chrono::steady_clock::now();
        const std::string_view bytes(request->buffer.data(), static_cast<std::size_t>(cqe.res));
        if (client->second.probe.size() > server_config_.max_input_buffer_bytes ||
            bytes.size() > server_config_.max_input_buffer_bytes - client->second.probe.size()) {
          close_client(request->fd);
          return;
        }
        client->second.probe.append(bytes);
        if (client->second.mode == Client::Mode::kUnknown) {
          if (client->second.probe.front() == '*') {
            if (!Enabled(protocol_config_, "resp2")) {
              close_client(request->fd);
              return;
            }
            client->second.mode = Client::Mode::kResp;
          } else if (client->second.probe.starts_with("KVB1")) {
            if (!Enabled(protocol_config_, "batch-v1")) {
              close_client(request->fd);
              return;
            }
            client->second.mode = Client::Mode::kBatch;
          } else if (client->second.probe.size() < 4 && client->second.probe.front() == 'K') {
            if (!client->second.parse_started.has_value())
              client->second.parse_started = std::chrono::steady_clock::now();
            const auto recv_status = submit_recv(request->fd);
            if (!recv_status.ok()) loop_error = recv_status;
            return;
          } else {
            if (!Enabled(protocol_config_, "text-kv")) {
              close_client(request->fd);
              return;
            }
            client->second.mode = Client::Mode::kNative;
          }
        }
        const std::string received = std::exchange(client->second.probe, {});
        Status fed = Status::Ok();
        Result<std::vector<Command>> commands{std::vector<Command>{}};
        std::vector<std::vector<Command>> batches;
        if (client->second.mode == Client::Mode::kResp) {
          fed = client->second.parser.Feed(received);
          if (fed.ok()) commands = client->second.parser.ParseAvailable();
        } else if (client->second.mode == Client::Mode::kNative) {
          fed = client->second.native.Feed(received);
          if (fed.ok()) commands = client->second.native.ParseAvailable();
        } else {
          fed = client->second.batch.Feed(received);
          if (fed.ok()) {
            auto parsed = client->second.batch.ParseAvailableFrames();
            if (!parsed.ok())
              commands = parsed.status();
            else
              batches = std::move(parsed).value();
          }
        }
        if (!fed.ok()) {
          close_client(request->fd);
          return;
        }
        if (!commands.ok()) {
          close_client(request->fd);
          return;
        }
        auto append_response = [&](std::string encoded) {
          if (encoded.size() >
              server_config_.max_output_buffer_bytes -
                  std::min(server_config_.max_output_buffer_bytes, client->second.output.size()))
            return false;
          client->second.output += std::move(encoded);
          return true;
        };
        if (client->second.mode == Client::Mode::kBatch) {
          std::size_t total_commands = 0;
          for (const auto& batch : batches) {
            if (batch.size() > server_config_.max_inflight_requests ||
                total_commands > server_config_.max_inflight_requests - batch.size()) {
              close_client(request->fd);
              return;
            }
            total_commands += batch.size();
          }
          for (const auto& batch : batches) {
            std::vector<CommandResponse> responses;
            responses.reserve(batch.size());
            for (const auto& command : batch) {
              responses.push_back(dispatcher_.Execute(command));
            }
            if (!append_response(EncodeBatch(responses))) {
              close_client(request->fd);
              return;
            }
          }
        } else {
          if (commands.value().size() > server_config_.max_inflight_requests) {
            close_client(request->fd);
            return;
          }
          for (const auto& command : commands.value()) {
            const auto encoded = client->second.mode == Client::Mode::kResp
                                     ? EncodeResp(dispatcher_.Execute(command))
                                     : EncodeNative(dispatcher_.Execute(command));
            if (!append_response(encoded)) {
              close_client(request->fd);
              return;
            }
          }
        }
        const bool buffered = client->second.mode == Client::Mode::kResp
                                  ? client->second.parser.has_buffered_data()
                              : client->second.mode == Client::Mode::kNative
                                  ? client->second.native.has_buffered_data()
                                  : client->second.batch.has_buffered_data();
        const auto buffered_bytes =
            client->second.probe.size() + (client->second.mode == Client::Mode::kResp
                                               ? client->second.parser.buffered_bytes()
                                           : client->second.mode == Client::Mode::kNative
                                               ? client->second.native.buffered_bytes()
                                               : client->second.batch.buffered_bytes());
        if (buffered_bytes > server_config_.max_input_buffer_bytes) {
          close_client(request->fd);
          return;
        }
        if (buffered) {
          if (!client->second.parse_started.has_value())
            client->second.parse_started = std::chrono::steady_clock::now();
          if (std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - *client->second.parse_started)
                  .count() > static_cast<std::int64_t>(protocol_config_.parse_timeout_ms)) {
            close_client(request->fd);
            return;
          }
        } else {
          client->second.parse_started.reset();
        }
        const auto send_status = submit_send(request->fd);
        const auto recv_status =
            client->second.output.size() < server_config_.output_high_watermark_bytes
                ? submit_recv(request->fd)
                : Status::Ok();
        if (!send_status.ok()) loop_error = send_status;
        if (!recv_status.ok()) loop_error = recv_status;
      } else if (request->kind == Request::Kind::kSend) {
        client->second.send_pending = false;
        if (cqe.res <= 0 || static_cast<std::size_t>(cqe.res) > request->payload.size()) {
          close_client(request->fd);
          return;
        }
        client->second.output.erase(0, static_cast<std::size_t>(cqe.res));
        const auto status = submit_send(request->fd);
        if (!status.ok()) loop_error = status;
        if (client->second.output.size() < server_config_.output_high_watermark_bytes) {
          const auto recv_status = submit_recv(request->fd);
          if (!recv_status.ok()) loop_error = recv_status;
        }
      }
    });
    if (ring.HasOverflow() || !loop_error.ok()) {
      state_.store(ServerState::kFailed, std::memory_order_release);
      stopping_.store(true, std::memory_order_release);
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired;
    for (const auto& [fd, client] : clients) {
      const auto idle_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - client.last_activity).count();
      const auto parse_ms =
          client.parse_started.has_value()
              ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *client.parse_started)
                    .count()
              : 0;
      if (idle_ms > static_cast<std::int64_t>(server_config_.idle_timeout_ms) ||
          (client.parse_started.has_value() &&
           parse_ms > static_cast<std::int64_t>(protocol_config_.parse_timeout_ms)))
        expired.push_back(fd);
    }
    for (const int fd : expired) close_client(fd);
  }

  const auto terminal_error =
      ring.HasOverflow() ? Status(StatusCode::kIoError, "io_uring completion queue overflow")
                         : loop_error;

  std::vector<std::uint64_t> pending_ids;
  pending_ids.reserve(requests.size() + 1U);
  pending_ids.push_back(kAcceptTag);
  for (const auto& [id, request] : requests) {
    static_cast<void>(request);
    pending_ids.push_back(id);
  }
  const auto cancel_status = ring.Cancel(pending_ids);
  const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!requests.empty() && std::chrono::steady_clock::now() < cancel_deadline) {
    struct pollfd descriptor {
      ring.fd(), POLLIN, 0
    };
    static_cast<void>(::poll(&descriptor, 1, 10));
    ring.Drain([&](const io_uring_cqe& cqe) {
      if (cqe.user_data != kCancelTag) requests.erase(cqe.user_data);
    });
  }
  const bool drained = requests.empty();
  // Keep request-owned buffers alive until Ring is destroyed at scope exit.
  for (const auto& [fd, client] : clients) {
    static_cast<void>(client);
    ::close(fd);
  }
  ::close(listener);
  if (!cancel_status.ok()) return cancel_status;
  if (!drained) return {StatusCode::kDeadlineExceeded, "io_uring cancellation drain timed out"};
  if (!terminal_error.ok()) return terminal_error;
  state_.store(ServerState::kStopped, std::memory_order_release);
  return Status::Ok();
}

void IoUringServer::Stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::shared_ptr<EpollServer> fallback;
  {
    std::lock_guard lock(fallback_mutex_);
    fallback = fallback_;
  }
  if (fallback != nullptr) {
    fallback->Stop();
  } else if (state_.load(std::memory_order_acquire) == ServerState::kReady) {
    state_.store(ServerState::kDraining, std::memory_order_release);
  }
}

}  // namespace kvstore
