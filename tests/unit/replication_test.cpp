#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/engine/array_engine.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/net/epoll_server.hpp"
#include "kvstore/replication/backlog.hpp"
#include "kvstore/replication/executor.hpp"
#include "kvstore/replication/frame.hpp"
#include "kvstore/replication/handshake.hpp"
#include "kvstore/replication/live_sync.hpp"
#include "kvstore/replication/sync.hpp"

namespace kvstore {
namespace {

WriteEvent Event(std::uint64_t offset, std::string key, std::string value) {
  return {offset,           offset,   WriteSource::kClient, CommandType::kSet, std::move(key),
          std::move(value), "primary"};
}

WriteEvent IncrementalEvent(std::uint64_t offset, std::string key, std::string value) {
  auto event = Event(offset, std::move(key), std::move(value));
  event.source = WriteSource::kIncrementalSync;
  event.checksum = event.ComputeChecksum();
  return event;
}

std::uint16_t ReserveReplicationPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ::close(fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    ::close(fd);
    return 0;
  }
  ::close(fd);
  return ntohs(address.sin_port);
}

bool WaitForValue(Dispatcher& dispatcher, std::string_view key, std::string_view expected,
                  int attempts = 300) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const auto value = dispatcher.engine().Get(key, {});
    if (value.ok() && value.value() == expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

TEST(ReplicationHandshakeTest, SupportsPartialFrameAndRejectsVersion) {
  const ReplicationHello hello{"primary-1", "primary", "snapshot,backlog"};
  const auto encoded = ReplicationHandshake::Encode(hello);
  ASSERT_TRUE(encoded.ok());
  std::string input = encoded.value().substr(0, 5);
  auto partial = ReplicationHandshake::Decode(input);
  ASSERT_TRUE(partial.ok());
  EXPECT_FALSE(partial.value().has_value());
  input.append(encoded.value().substr(5));
  auto decoded = ReplicationHandshake::Decode(input);
  ASSERT_TRUE(decoded.ok());
  ASSERT_TRUE(decoded.value().has_value());
  EXPECT_EQ(decoded.value()->node_id, hello.node_id);
  EXPECT_TRUE(input.empty());

  std::string incompatible = encoded.value();
  incompatible[4] = 0;
  incompatible[5] = 2;
  EXPECT_EQ(ReplicationHandshake::Decode(incompatible).status().code(), StatusCode::kUnsupported);
}

TEST(ReplicationHandshakeTest, TracksPeerStateAndTimeout) {
  const auto start = std::chrono::steady_clock::now();
  ReplicationConnection connection(start, std::chrono::milliseconds(10));
  const auto encoded = ReplicationHandshake::Encode(
      {"primary", "primary", "snapshot,backlog,ack,heartbeat,full-sync-request"});
  ASSERT_TRUE(encoded.ok());
  ASSERT_TRUE(connection.Receive(encoded.value().substr(0, 3), start).ok());
  ASSERT_TRUE(connection.Receive(encoded.value().substr(3), start).ok());
  EXPECT_EQ(connection.state(), ReplicationConnectionState::kHandshake);
  EXPECT_EQ(connection.peer()->node_id, "primary");
  EXPECT_TRUE(connection.BeginFullSync().ok());
  EXPECT_TRUE(connection.BeginCatchUp().ok());
  EXPECT_TRUE(connection.MarkOnline().ok());
  EXPECT_EQ(connection.state(), ReplicationConnectionState::kOnline);

  ReplicationConnection slow(start, std::chrono::milliseconds(10));
  EXPECT_EQ(slow.CheckTimeout(start + std::chrono::milliseconds(11)).code(),
            StatusCode::kDeadlineExceeded);
  ReplicationConnection invalid(start, std::chrono::milliseconds(10));
  EXPECT_EQ(invalid.BeginCatchUp().code(), StatusCode::kInvalidArgument);
}

TEST(ReplicationHandshakeTest, RejectsMissingCapabilityAndRoleMismatch) {
  const auto start = std::chrono::steady_clock::now();
  ReplicationConnection missing(start, std::chrono::milliseconds(100));
  const auto hello = ReplicationHandshake::Encode({"primary", "primary", "snapshot"});
  ASSERT_TRUE(hello.ok());
  EXPECT_EQ(missing.Receive(hello.value(), start).code(), StatusCode::kUnsupported);

  ReplicationConnection wrong_role(start, std::chrono::milliseconds(100));
  const auto replica = ReplicationHandshake::Encode(
      {"replica", "replica", "snapshot,backlog,ack,heartbeat,full-sync-request"});
  ASSERT_TRUE(replica.ok());
  EXPECT_EQ(wrong_role.Receive(replica.value(), start).code(), StatusCode::kInvalidArgument);
}

TEST(ReplicationBacklogTest, WrapsAndRequestsOlderThanWindowNeedFullSync) {
  ReplicationBacklog backlog;
  for (std::uint64_t offset = 1; offset <= ReplicationBacklog::kCapacity + 1U; ++offset)
    ASSERT_TRUE(backlog.Append({Event(offset, "k" + std::to_string(offset), "v")}).ok());
  EXPECT_EQ(backlog.size(), ReplicationBacklog::kCapacity);
  EXPECT_EQ(backlog.oldest_offset(), 2U);
  EXPECT_EQ(backlog.newest_offset(), ReplicationBacklog::kCapacity + 1U);
  EXPECT_EQ(backlog.ReadFrom(1).status().code(), StatusCode::kBusy);
  auto events = backlog.ReadFrom(2);
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events.value().size(), ReplicationBacklog::kCapacity);
  EXPECT_EQ(events.value().front().offset, 2U);
  EXPECT_EQ(events.value().back().offset, ReplicationBacklog::kCapacity + 1U);
}

TEST(ReplicationBacklogTest, RejectsInvalidMetadataAndOversizedBatchAtomically) {
  ReplicationBacklog backlog;
  std::vector<WriteEvent> oversized;
  oversized.reserve(ReplicationBacklog::kCapacity + 1U);
  for (std::uint64_t offset = 1; offset <= ReplicationBacklog::kCapacity + 1U; ++offset)
    oversized.push_back(Event(offset, std::to_string(offset), "v"));
  EXPECT_EQ(backlog.Append(oversized).code(), StatusCode::kLimitExceeded);
  EXPECT_EQ(backlog.size(), 0U);
  auto invalid = Event(1, "a", "v");
  invalid.source = WriteSource::kIncrementalSync;
  invalid.checksum = invalid.ComputeChecksum();
  EXPECT_EQ(backlog.Append({invalid}).code(), StatusCode::kInvalidArgument);
}

TEST(ReplicationFrameTest, SupportsFragmentedSnapshotAndEventsWithChecksum) {
  const std::vector<Entry> entries{{"a", "one"}, {"b", "two"}};
  auto snapshot = ReplicationFrameCodec::EncodeSnapshot({7, 9}, entries);
  ASSERT_TRUE(snapshot.ok());
  std::string input = snapshot.value().substr(0, 4);
  auto partial = ReplicationFrameCodec::Decode(input);
  ASSERT_TRUE(partial.ok());
  EXPECT_FALSE(partial.value().has_value());
  input.append(snapshot.value().substr(4));
  auto frame = ReplicationFrameCodec::Decode(input);
  ASSERT_TRUE(frame.ok());
  auto decoded = ReplicationFrameCodec::DecodeSnapshot(frame.value().value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().first, (RecoveryPoint{7, 9}));
  EXPECT_EQ(decoded.value().second, entries);

  auto events = ReplicationFrameCodec::EncodeEvents({IncrementalEvent(1, "a", "1")});
  ASSERT_TRUE(events.ok());
  auto event_input = events.value();
  auto event_frame = ReplicationFrameCodec::Decode(event_input);
  ASSERT_TRUE(event_frame.ok());
  auto decoded_events = ReplicationFrameCodec::DecodeEvents(event_frame.value().value());
  ASSERT_TRUE(decoded_events.ok());
  ASSERT_EQ(decoded_events.value().size(), 1U);
  EXPECT_EQ(decoded_events.value()[0].ComputeChecksum(), decoded_events.value()[0].checksum);
  auto request = ReplicationFrameCodec::EncodeFullSyncRequest(7);
  ASSERT_TRUE(request.ok());
  auto request_input = request.value();
  auto request_frame = ReplicationFrameCodec::Decode(request_input);
  ASSERT_TRUE(request_frame.ok());
  EXPECT_EQ(ReplicationFrameCodec::DecodeFullSyncRequest(request_frame.value().value()).value(),
            7U);
  auto snapshot_chunks = ReplicationChunkGenerator::Snapshot({7, 9}, entries, 43);
  ASSERT_TRUE(snapshot_chunks.ok());
  EXPECT_GT(snapshot_chunks.value().chunk_count(), 0U);
}

TEST(ReplicationFrameTest, ChunksSnapshotBeforeSingleFrameLimit) {
  const std::string value((64U << 20U) + 1024U, 'x');
  auto generator =
      ReplicationChunkGenerator::Snapshot({11, 12}, {{"large", value}}, 99, 64U * 1024U);
  ASSERT_TRUE(generator.ok());
  ASSERT_GT(generator.value().chunk_count(), 1024U);
  std::size_t frames = 0;
  for (;;) {
    auto encoded = generator.value().Next();
    ASSERT_TRUE(encoded.ok());
    if (!encoded.value()) break;
    EXPECT_LE(encoded.value()->size(), 64U * 1024U + 38U);
    ++frames;
  }
  EXPECT_EQ(frames, generator.value().chunk_count());
}

TEST(ReplicationFrameTest, PullGeneratorOwnsInputAndYieldsBoundedFrames) {
  std::vector<Entry> entries{{"key", std::string(9000, 'v')}};
  auto generator = ReplicationChunkGenerator::Snapshot({5, 8}, std::move(entries), 123, 1024);
  ASSERT_TRUE(generator.ok());
  EXPECT_GT(generator.value().chunk_count(), 1U);
  EXPECT_EQ(generator.value().total_size(), 9031U);
  std::vector<ReplicationFrame> decoded;
  for (;;) {
    auto next = generator.value().Next();
    ASSERT_TRUE(next.ok());
    if (!next.value()) break;
    EXPECT_LE(next.value()->size(), 13U + 25U + 1024U);
    auto input = *next.value();
    auto frame = ReplicationFrameCodec::Decode(input);
    ASSERT_TRUE(frame.ok());
    ASSERT_TRUE(frame.value());
    decoded.push_back(std::move(*frame.value()));
  }
  EXPECT_FALSE(generator.value().Next().value());
  auto logical = ReplicationFrameCodec::ReassembleChunks(decoded);
  ASSERT_TRUE(logical.ok());
  ReplicationFrame snapshot{ReplicationFrameType::kSnapshot, std::move(logical).value()};
  auto restored = ReplicationFrameCodec::DecodeSnapshot(snapshot);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(restored.value().second.front().value.size(), 9000U);

  auto bad = ReplicationChunkGenerator::Events({}, 1, 0);
  EXPECT_FALSE(bad.ok());

  auto event = IncrementalEvent(1, "event-key", "event-value");
  auto event_generator = ReplicationChunkGenerator::Events({event}, 124, 8);
  ASSERT_TRUE(event_generator.ok());
  std::vector<ReplicationFrame> event_chunks;
  for (;;) {
    auto next = event_generator.value().Next();
    ASSERT_TRUE(next.ok());
    if (!next.value()) break;
    auto input = *next.value();
    auto decoded_frame = ReplicationFrameCodec::Decode(input);
    ASSERT_TRUE(decoded_frame.ok());
    ASSERT_TRUE(decoded_frame.value());
    event_chunks.push_back(std::move(*decoded_frame.value()));
  }
  auto event_payload = ReplicationFrameCodec::ReassembleChunks(event_chunks);
  ASSERT_TRUE(event_payload.ok());
  ReplicationFrame event_frame{ReplicationFrameType::kEvents, std::move(event_payload).value()};
  auto decoded_events = ReplicationFrameCodec::DecodeEvents(event_frame);
  ASSERT_TRUE(decoded_events.ok());
  ASSERT_EQ(decoded_events.value().size(), 1U);
  EXPECT_EQ(decoded_events.value().front().key, event.key);
}

TEST(ReplicationSyncTest, FullSyncThenIncrementalConvergesAndRejectsFork) {
  ArrayEngine primary(16);
  ArrayEngine replica(16);
  ASSERT_TRUE(primary.Create("a", "one", {}).ok());
  ReplicationBacklog backlog;
  ASSERT_TRUE(backlog.Append({Event(1, "a", "one")}).ok());
  const auto directory = std::filesystem::temp_directory_path() /
                         ("kvstore-replication-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  PrimarySyncSession session(primary, backlog, directory);
  auto point = session.BeginFullSync();
  ASSERT_TRUE(point.ok());
  ArrayEngine snapshot_replica(16);
  ASSERT_TRUE(Snapshot::Load(directory, snapshot_replica).ok());
  auto entries = snapshot_replica.Export({});
  ASSERT_TRUE(entries.ok());
  ReplicaSyncApplier applier(replica);
  ASSERT_TRUE(applier.InstallFullSync(entries.value(), point.value()).ok());
  ASSERT_TRUE(replica.Get("a", {}).ok());

  auto published = Event(2, "b", "two");
  ASSERT_TRUE(backlog.Append({published}).ok());
  auto incremental = session.CatchUp(2);
  ASSERT_TRUE(incremental.ok());
  ASSERT_TRUE(applier.ApplyIncremental(incremental.value()).ok());
  EXPECT_EQ(replica.Get("b", {}).value(), "two");
  EXPECT_TRUE(applier.ApplyIncremental(incremental.value()).ok());
  auto fork = IncrementalEvent(2, "b", "different");
  fork.event_id = 99;
  fork.checksum = fork.ComputeChecksum();
  EXPECT_EQ(applier.ApplyIncremental({fork}).code(), StatusCode::kCorruption);
  EXPECT_TRUE(applier.Ack(applier.applied()).ok());
  EXPECT_EQ(session.Ack({2, 99}).code(), StatusCode::kInvalidArgument);
  std::filesystem::remove_all(directory);
}

TEST(ReplicationSyncTest, SnapshotViewRejectsOversizedDataBeforeExport) {
  auto engine = std::make_unique<ArrayEngine>(8);
  ASSERT_TRUE(engine->Create("key", std::string(4096, 'x'), {}).ok());
  Dispatcher dispatcher(std::move(engine));
  const auto rejected = dispatcher.SnapshotView(128);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.status().code(), StatusCode::kLimitExceeded);
  const auto accepted = dispatcher.SnapshotView(8192);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(accepted.value().entries.size(), 1U);
  EXPECT_EQ(accepted.value().entries.front().value.size(), 4096U);
}

TEST(ReplicationIoUringSocketTest, DrivesFullIncrementalAndReconnectConvergence) {
  ReplicationExecutorOptions executor_options;
  executor_options.queue_capacity = 8;
  auto executor_result = CreateReplicationExecutor("io_uring", std::move(executor_options));
  if (!executor_result.ok() && executor_result.status().code() == StatusCode::kUnsupported)
    GTEST_SKIP() << "io_uring unavailable: " << executor_result.status().message();
  ASSERT_TRUE(executor_result.ok()) << executor_result.status().message();
  auto executor = std::move(executor_result).value();

  const auto primary_port = ReserveReplicationPort();
  const auto replica_port = ReserveReplicationPort();
  ASSERT_NE(primary_port, 0U);
  ASSERT_NE(replica_port, 0U);
  const ServerConfig server_config{"127.0.0.1", primary_port, 1, 16, 8, 1000, 3000, 500,
                                   1U << 20U, 1U << 20U, 1U << 19U};
  const ServerConfig replica_config{"127.0.0.1", replica_port, 1, 16, 8, 1000, 3000, 500,
                                   1U << 20U, 1U << 20U, 1U << 19U};
  const ProtocolConfig protocol{{"resp2"}, 128, 1024, 1U << 20U, 16, 500};
  ReplicationBacklog backlog;
  Dispatcher primary_dispatcher(
      std::make_unique<HashEngine>(64),
      [&backlog](const WriteEvent& event) { return backlog.Append({event}); }, {}, {}, 1, 1,
      "io-uring-primary");
  Dispatcher replica_dispatcher(std::make_unique<HashEngine>(64));
  replica_dispatcher.SetReadOnly(true);
  ASSERT_TRUE(primary_dispatcher
                  .Execute({CommandType::kSet, {"snapshot", "ready"}, WriteSource::kClient,
                            true})
                  .ok);

  EpollServer primary(server_config, protocol, primary_dispatcher, "primary", "io-uring-primary",
                      {}, &backlog, 100, 128);
  EpollServer replica(replica_config, protocol, replica_dispatcher, "replica", "io-uring-replica",
                      "127.0.0.1:" + std::to_string(primary_port), nullptr, 100, 128,
                      executor.get());
  Status primary_status;
  Status replica_status;
  std::jthread primary_thread([&] { primary_status = primary.Run(); });
  for (int retry = 0; retry < 100 && !primary.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!primary.ready()) {
    primary.Stop();
    primary_thread.join();
    ADD_FAILURE() << "io_uring replication primary did not become ready";
    executor->Shutdown();
    return;
  }
  std::jthread replica_thread([&] { replica_status = replica.Run(); });
  for (int retry = 0; retry < 100 && !replica.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!replica.ready()) {
    replica.Stop();
    primary.Stop();
    replica_thread.join();
    primary_thread.join();
    ADD_FAILURE() << "io_uring replication replica did not become ready";
    executor->Shutdown();
    return;
  }

  EXPECT_TRUE(WaitForValue(replica_dispatcher, "snapshot", "ready"));
  EXPECT_TRUE(primary_dispatcher
                  .Execute({CommandType::kSet, {"incremental", "applied"},
                            WriteSource::kClient, true})
                  .ok);
  EXPECT_TRUE(WaitForValue(replica_dispatcher, "incremental", "applied"));

  primary.Stop();
  primary_thread.join();
  ASSERT_TRUE(primary_status.ok()) << primary_status.message();
  ASSERT_TRUE(primary_dispatcher
                  .Execute({CommandType::kSet, {"reconnect", "applied"}, WriteSource::kClient,
                            true})
                  .ok);

  EpollServer restarted_primary(server_config, protocol, primary_dispatcher, "primary",
                                "io-uring-primary", {}, &backlog, 100, 128);
  Status restarted_status;
  std::jthread restarted_thread([&] { restarted_status = restarted_primary.Run(); });
  for (int retry = 0; retry < 100 && !restarted_primary.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!restarted_primary.ready()) {
    restarted_primary.Stop();
    replica.Stop();
    restarted_thread.join();
    replica_thread.join();
    ADD_FAILURE() << "io_uring replication primary did not restart";
    executor->Shutdown();
    return;
  }
  EXPECT_TRUE(WaitForValue(replica_dispatcher, "reconnect", "applied"));

  replica.Stop();
  replica_thread.join();
  restarted_primary.Stop();
  restarted_thread.join();
  executor->Shutdown();
  EXPECT_TRUE(replica_status.ok()) << replica_status.message();
  EXPECT_TRUE(restarted_status.ok()) << restarted_status.message();
}

TEST(LiveSyncDeduplicatorTest, ForwardsMultiHopEventsOnceAndExpiresBoundedEntries) {
  using Clock = LiveSyncDeduplicator::Clock;
  const auto start = Clock::time_point{};
  LiveSyncDeduplicator node_a(2, std::chrono::milliseconds(10));
  LiveSyncDeduplicator node_b(2, std::chrono::milliseconds(10));
  LiveSyncDeduplicator node_c(2, std::chrono::milliseconds(10));

  auto original = Event(1, "key", "value");
  EXPECT_EQ(node_a.Observe(original, start), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_a.Observe(original, start), LiveSyncDeduplicator::Observation::kDuplicate);
  EXPECT_EQ(node_b.Observe(original, start), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_c.Observe(original, start), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_a.Observe(original, start), LiveSyncDeduplicator::Observation::kDuplicate);

  auto next = Event(2, "next", "value");
  next.origin_node = "origin:with:separator";
  EXPECT_EQ(node_b.Observe(next, start), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_b.Observe(next, start), LiveSyncDeduplicator::Observation::kDuplicate);
  EXPECT_EQ(node_b.size(), 2U);
  EXPECT_EQ(node_b.Observe(Event(3, "third", "value"), start),
            LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_b.size(), 2U);
  EXPECT_EQ(node_b.Observe(original, start + std::chrono::milliseconds(10)),
            LiveSyncDeduplicator::Observation::kNew);

  auto fork = original;
  fork.value = "fork";
  fork.checksum = fork.ComputeChecksum();
  EXPECT_EQ(node_b.Observe(fork, start + std::chrono::milliseconds(10)),
            LiveSyncDeduplicator::Observation::kConflict);

  LiveSyncDeduplicator disabled(0, std::chrono::milliseconds(10));
  EXPECT_EQ(disabled.Observe(original, start), LiveSyncDeduplicator::Observation::kConflict);
  EXPECT_EQ(disabled.Observe(Event(0, "bad", "value"), start),
            LiveSyncDeduplicator::Observation::kConflict);
}

TEST(LiveSyncDeduplicatorTest, SuppressesCyclicRelayAndRejectsReorderedConflicts) {
  using Clock = LiveSyncDeduplicator::Clock;
  const auto now = Clock::time_point{};
  LiveSyncDeduplicator node_a(16, std::chrono::minutes(1));
  LiveSyncDeduplicator node_b(16, std::chrono::minutes(1));
  auto event = Event(7, "cycle-key", "original");
  event.origin_node = "node-a";

  EXPECT_EQ(node_a.Observe(event, now), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_b.Observe(event, now), LiveSyncDeduplicator::Observation::kNew);
  EXPECT_EQ(node_a.Observe(event, now), LiveSyncDeduplicator::Observation::kDuplicate);

  auto reordered = event;
  reordered.offset = 3;
  reordered.checksum = reordered.ComputeChecksum();
  EXPECT_EQ(node_b.Observe(reordered, now), LiveSyncDeduplicator::Observation::kConflict);

  auto conflicting = event;
  conflicting.value = "tampered";
  conflicting.checksum = conflicting.ComputeChecksum();
  EXPECT_EQ(node_a.Observe(conflicting, now), LiveSyncDeduplicator::Observation::kConflict);
  EXPECT_EQ(node_a.size(), 1U);
  EXPECT_EQ(node_b.size(), 1U);
}

class ExecutorHarness {
 public:
  Result<std::unique_ptr<IReplicationExecutor>> Create(std::string_view backend) {
    ReplicationExecutorOptions options;
    options.queue_capacity = 2;
    options.reactor_post = [this](std::function<void()> work) {
      reactor_tasks.push_back(std::move(work));
      return Status::Ok();
    };
    options.proactor_submit = [this](std::function<void()> work,
                                     ProactorReplicationExecutor::Completion completion) {
      proactor_tasks.emplace_back(std::move(work), std::move(completion));
      return Status::Ok();
    };
    return CreateReplicationExecutor(backend, std::move(options));
  }

  void RunOne(std::string_view backend) {
    if (backend == "reactor") {
      auto task = std::move(reactor_tasks.front());
      reactor_tasks.pop_front();
      task();
    } else if (backend == "proactor") {
      auto item = std::move(proactor_tasks.front());
      proactor_tasks.pop_front();
      item.first();
      item.second();
    }
  }

  bool HasPending(std::string_view backend) const {
    return backend == "reactor"    ? !reactor_tasks.empty()
           : backend == "proactor" ? !proactor_tasks.empty()
                                   : false;
  }

 private:
  std::deque<std::function<void()>> reactor_tasks;
  std::deque<std::pair<std::function<void()>, ProactorReplicationExecutor::Completion>>
      proactor_tasks;
};

class ReplicationExecutorConformanceTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ReplicationExecutorConformanceTest, ExecutesWorkAndIsolatesExceptions) {
  const auto backend = GetParam();
  ExecutorHarness harness;
  auto result = harness.Create(backend);
  ASSERT_TRUE(result.ok());
  auto executor = std::move(result).value();
  std::mutex mutex;
  std::condition_variable changed;
  int completed = 0;
  ASSERT_TRUE(executor->Submit([] { throw std::runtime_error("isolated"); }).ok());
  if (harness.HasPending(backend)) harness.RunOne(backend);
  ASSERT_TRUE(executor
                  ->Submit([&] {
                    {
                      std::scoped_lock lock(mutex);
                      ++completed;
                    }
                    changed.notify_all();
                  })
                  .ok());
  if (harness.HasPending(backend)) harness.RunOne(backend);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return completed == 1; }));
  }
  executor->Shutdown();
  EXPECT_EQ(executor->Submit([] {}).code(), StatusCode::kCancelled);
}

TEST_P(ReplicationExecutorConformanceTest, BoundedAdmissionAndShutdownDrain) {
  const auto backend = GetParam();
  ExecutorHarness harness;
  ReplicationExecutorOptions options;
  options.queue_capacity = 0;
  options.reactor_post = [](std::function<void()>) { return Status::Ok(); };
  options.proactor_submit = [](std::function<void()>, ProactorReplicationExecutor::Completion) {
    return Status::Ok();
  };
  auto rejected_executor = CreateReplicationExecutor(backend, std::move(options));
  ASSERT_TRUE(rejected_executor.ok());
  EXPECT_EQ(rejected_executor.value()->Submit([] {}).code(), StatusCode::kBusy);
  rejected_executor.value()->Shutdown();

  auto result = harness.Create(backend);
  ASSERT_TRUE(result.ok());
  auto executor = std::move(result).value();
  std::atomic<int> drained{0};
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool release = false;
  ASSERT_TRUE(executor
                  ->Submit([&] {
                    std::unique_lock lock(gate_mutex);
                    gate_changed.wait(lock, [&] { return release; });
                    ++drained;
                  })
                  .ok());
  ASSERT_TRUE(executor->Submit([&] { ++drained; }).ok());
  EXPECT_EQ(executor->Submit([] {}).code(), StatusCode::kBusy);
  {
    std::scoped_lock lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  auto shutdown = std::async(std::launch::async, [&] { executor->Shutdown(); });
  while (harness.HasPending(backend)) harness.RunOne(backend);
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(drained.load(), 2);
  EXPECT_EQ(executor->Submit([] {}).code(), StatusCode::kCancelled);
}

TEST_P(ReplicationExecutorConformanceTest, RunsIdenticalReplicaStateMachine) {
  const auto backend = GetParam();
  ExecutorHarness harness;
  auto result = harness.Create(backend);
  ASSERT_TRUE(result.ok());
  auto executor = std::move(result).value();
  ArrayEngine replica(16);
  ReplicaSyncApplier applier(replica);
  std::promise<Status> completed;
  ASSERT_TRUE(executor
                  ->Submit([&] {
                    auto status = applier.InstallFullSync({{"seed", "one"}}, {1, 1});
                    if (status.ok())
                      status = applier.ApplyIncremental({IncrementalEvent(2, "k", "v")});
                    completed.set_value(status);
                  })
                  .ok());
  if (harness.HasPending(backend)) harness.RunOne(backend);
  auto future = completed.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  EXPECT_EQ(replica.Get("seed", {}).value(), "one");
  EXPECT_EQ(replica.Get("k", {}).value(), "v");
  EXPECT_EQ(applier.applied().offset, 2U);
  executor->Shutdown();
}

INSTANTIATE_TEST_SUITE_P(AllBackends, ReplicationExecutorConformanceTest,
                         ::testing::Values("pthread", "reactor", "proactor", "ntyco"));

TEST(ReplicationExecutorFactoryTest, RejectsUnknownBackendAndMissingAdapters) {
  EXPECT_FALSE(CreateReplicationExecutor("unknown").ok());
  EXPECT_FALSE(CreateReplicationExecutor("reactor").ok());
  EXPECT_FALSE(CreateReplicationExecutor("proactor").ok());
}

TEST(ReplicationExecutorIoUringTest, RunsCompletionAndDrainsOnShutdown) {
  ReplicationExecutorOptions options;
  options.queue_capacity = 4;
  auto result = CreateReplicationExecutor("io_uring", std::move(options));
  if (!result.ok() && result.status().code() == StatusCode::kUnsupported) GTEST_SKIP();
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto executor = std::move(result).value();
  std::promise<void> completed;
  auto future = completed.get_future();
  ASSERT_TRUE(executor->Submit([&completed] { completed.set_value(); }).ok());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  executor->Shutdown();
}

}  // namespace
}  // namespace kvstore
