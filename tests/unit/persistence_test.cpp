#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/persistence/aof.hpp"
#include "kvstore/persistence/snapshot.hpp"
#include "kvstore/persistence/write_event_queue.hpp"

namespace kvstore {
namespace {

class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() / "kvstore-persistence-test";
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_;
};

TEST_F(PersistenceTest, SnapshotRoundTripAndChecksumRejectsCorruption) {
  HashEngine source(16);
  ASSERT_TRUE(source.Create("a", "one", {}).ok());
  ASSERT_TRUE(source.Create("b", "two", {}).ok());
  ASSERT_TRUE(Snapshot::Save(directory_, source, 7).ok());
  HashEngine restored(16);
  auto offset = Snapshot::Load(directory_, restored);
  ASSERT_TRUE(offset.ok());
  EXPECT_EQ(offset.value(), 7U);
  EXPECT_EQ(restored.Export({}).value(), source.Export({}).value());

  const auto path = directory_ / "snapshot.kvs";
  auto permissions = std::filesystem::status(path).permissions();
  static_cast<void>(permissions);
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(-1, std::ios::end);
  char byte = 0;
  file.write(&byte, 1);
  file.close();
  HashEngine corrupted(16);
  EXPECT_EQ(Snapshot::Load(directory_, corrupted).status().code(), StatusCode::kCorruption);
}

TEST_F(PersistenceTest, AofReplayRestoresMutationsAndRejectsTruncation) {
  AofWriter writer(directory_, 100, 1024 * 1024, 60000, "everysec");
  ASSERT_TRUE(writer.Append({1, 1, WriteSource::kClient, CommandType::kSet, "a", "one"}).ok());
  ASSERT_TRUE(writer.Append({2, 2, WriteSource::kClient, CommandType::kMod, "a", "two"}).ok());
  ASSERT_TRUE(writer.Append({3, 3, WriteSource::kClient, CommandType::kDel, "a", ""}).ok());
  ASSERT_TRUE(writer.Append({4, 4, WriteSource::kClient, CommandType::kSet, "b", "v"}).ok());
  ASSERT_TRUE(writer.Flush(true).ok());
  HashEngine restored(16);
  const auto replayed = writer.Replay(restored);
  ASSERT_TRUE(replayed.ok()) << replayed.status().name() << ": " << replayed.status().message();
  EXPECT_EQ(restored.Get("b", {}).value(), "v");
  std::ofstream truncate(writer.path(), std::ios::binary | std::ios::trunc);
  truncate << "1 1 0 2 3\nxy";
  truncate.close();
  EXPECT_EQ(writer.Replay(restored).status().code(), StatusCode::kCorruption);
}

TEST_F(PersistenceTest, AofBatchIsAtomicAndIgnoresUncommittedTail) {
  AofWriter writer(directory_, 100, 1024 * 1024, 10, "no");
  ASSERT_TRUE(writer
                  .AppendBatch({{1, 1, WriteSource::kClient, CommandType::kSet, "a", "one"},
                                {2, 2, WriteSource::kClient, CommandType::kSet, "b", "two"}})
                  .ok());
  const auto committed_size = std::filesystem::file_size(writer.path());
  {
    std::ofstream tail(writer.path(), std::ios::binary | std::ios::app);
    tail.write("AOT1", 4);
  }
  HashEngine restored(16);
  const auto replayed = writer.Replay(restored);
  ASSERT_TRUE(replayed.ok()) << replayed.status().message();
  EXPECT_EQ(replayed.value(), 2U);
  EXPECT_EQ(restored.Get("a", {}).value(), "one");
  EXPECT_EQ(restored.Get("b", {}).value(), "two");
  EXPECT_EQ(std::filesystem::file_size(writer.path()), committed_size);
  ASSERT_TRUE(writer.Append({3, 3, WriteSource::kClient, CommandType::kSet, "c", "three"}).ok());
  HashEngine restarted(16);
  const auto restarted_offset = writer.Replay(restarted);
  ASSERT_TRUE(restarted_offset.ok()) << restarted_offset.status().message();
  EXPECT_EQ(restarted_offset.value(), 3U);
  EXPECT_EQ(restarted.Get("c", {}).value(), "three");
}

TEST_F(PersistenceTest, AofRejectsNonContiguousBatch) {
  AofWriter writer(directory_, 100, 1024 * 1024, 10, "always");
  EXPECT_EQ(writer
                .AppendBatch({{1, 1, WriteSource::kClient, CommandType::kSet, "a", "one"},
                              {3, 3, WriteSource::kClient, CommandType::kSet, "b", "two"}})
                .code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(std::filesystem::file_size(writer.path()), 0U);
}

TEST_F(PersistenceTest, AofRejectsSnapshotEventIdMismatch) {
  AofWriter writer(directory_, 100, 1024 * 1024, 10, "always");
  ASSERT_TRUE(writer.Append({1, 101, WriteSource::kClient, CommandType::kSet, "a", "one"}).ok());
  HashEngine restored(16);
  EXPECT_EQ(writer.Replay(restored, 1, 1).status().code(), StatusCode::kCorruption);
}

TEST_F(PersistenceTest, SnapshotCoversEmptyLargeMetadataVersionAndTruncation) {
  HashEngine source(8);
  ASSERT_TRUE(Snapshot::Save(directory_, source, 9, 13).ok());
  HashEngine restored(8);
  auto metadata = Snapshot::LoadWithMetadata(directory_, restored);
  ASSERT_TRUE(metadata.ok());
  EXPECT_EQ(metadata.value(), (RecoveryPoint{9, 13}));
  EXPECT_EQ(restored.Size(), 0U);

  ASSERT_TRUE(source.Create("large", std::string(1024 * 1024, 'v'), {}).ok());
  ASSERT_TRUE(Snapshot::Save(directory_, source, 10, 14).ok());
  metadata = Snapshot::LoadWithMetadata(directory_, restored);
  ASSERT_TRUE(metadata.ok());
  EXPECT_EQ(restored.Get("large", {}).value().size(), 1024U * 1024U);

  const auto path = directory_ / "snapshot.kvs";
  std::filesystem::resize_file(path, 12);
  EXPECT_EQ(Snapshot::LoadWithMetadata(directory_, restored).status().code(),
            StatusCode::kCorruption);
}

TEST_F(PersistenceTest, SnapshotIoUringUsesExplicitFallbackWhenUnavailable) {
  HashEngine source(8);
  ASSERT_TRUE(source.Create("key", "value", {}).ok());
  ASSERT_TRUE(Snapshot::Save(directory_, source, 1, 1, true, true).ok());
  HashEngine restored(8);
  ASSERT_TRUE(Snapshot::Load(directory_, restored).ok());
  EXPECT_EQ(restored.Get("key", {}).value(), "value");

  const auto direct_directory = directory_ / "direct";
  const auto direct = Snapshot::Save(direct_directory, source, 1, 1, true, false);
  if (direct.ok()) {
    HashEngine direct_restored(8);
    EXPECT_TRUE(Snapshot::Load(direct_directory, direct_restored).ok());
  } else {
    EXPECT_TRUE(direct.code() == StatusCode::kUnsupported || direct.code() == StatusCode::kIoError);
  }
}

TEST_F(PersistenceTest, AofPoliciesReplayIdempotentlyAndRejectBitFlip) {
  for (const std::string policy : {"always", "everysec", "no"}) {
    const auto policy_directory = directory_ / policy;
    AofWriter writer(policy_directory, 2, 256, 10, policy);
    ASSERT_TRUE(
        writer.Append({1, 1, WriteSource::kClient, CommandType::kSet, "n", "1", "node", 100}).ok());
    ASSERT_TRUE(
        writer.Append({2, 2, WriteSource::kClient, CommandType::kIncr, "n", "1", "node", 101})
            .ok());
    ASSERT_TRUE(writer.Flush(true).ok());
    HashEngine restored(8);
    auto replayed = writer.Replay(restored);
    ASSERT_TRUE(replayed.ok()) << replayed.status().message();
    EXPECT_EQ(restored.Get("n", {}).value(), "2");
    replayed = writer.Replay(restored, 2, 2);
    ASSERT_TRUE(replayed.ok()) << replayed.status().message();
    EXPECT_EQ(restored.Get("n", {}).value(), "2");
  }

  AofWriter corrupt(directory_ / "corrupt", 2, 256, 10, "always");
  ASSERT_TRUE(corrupt.Append({1, 1, WriteSource::kClient, CommandType::kSet, "key", "value"}).ok());
  {
    std::fstream file(corrupt.path(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(45);
    const char flipped = static_cast<char>(0xff);
    file.write(&flipped, 1);
  }
  HashEngine restored(8);
  EXPECT_EQ(corrupt.Replay(restored).status().code(), StatusCode::kCorruption);
}

TEST_F(PersistenceTest, WriteEventQueuePropagatesFailureRejectsOversizeAndDrains) {
  std::vector<std::uint64_t> consumed;
  WriteEventQueue queue(2, 256, [&consumed](const std::vector<WriteEvent>& events) {
    for (const auto& event : events) consumed.push_back(event.offset);
    return events.front().key == "fail" ? Status{StatusCode::kIoError, "consumer failed"}
                                        : Status::Ok();
  });
  EXPECT_TRUE(queue.Submit({1, 1, WriteSource::kClient, CommandType::kSet, "a", "1"}).ok());
  EXPECT_EQ(queue.Submit({2, 2, WriteSource::kClient, CommandType::kSet, "fail", "2"}).code(),
            StatusCode::kIoError);
  EXPECT_EQ(
      queue.Submit({3, 3, WriteSource::kClient, CommandType::kSet, "large", std::string(512, 'x')})
          .code(),
      StatusCode::kLimitExceeded);
  queue.Stop();
  EXPECT_EQ(consumed, (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(queue.Submit({4, 4, WriteSource::kClient, CommandType::kSet, "b", "3"}).code(),
            StatusCode::kCancelled);
}

TEST_F(PersistenceTest, AofEnforcesConfiguredFileCapacity) {
  AofWriter writer(directory_, 10, 1024, 10, "no", 64);
  EXPECT_EQ(writer.Append({1, 1, WriteSource::kClient, CommandType::kSet, "key", "value"}).code(),
            StatusCode::kLimitExceeded);
  EXPECT_EQ(std::filesystem::file_size(writer.path()), 0U);
}

TEST_F(PersistenceTest, AofReportsDeviceFullAndSnapshotFailedPublishKeepsPrevious) {
  const auto full_directory = directory_ / "full";
  std::filesystem::create_directories(full_directory);
  std::filesystem::create_symlink("/dev/full", full_directory / "appendonly.aof");
  AofWriter full(full_directory, 10, 1024, 10, "always");
  EXPECT_EQ(full.Append({1, 1, WriteSource::kClient, CommandType::kSet, "key", "value"}).code(),
            StatusCode::kIoError);

  HashEngine original(8);
  ASSERT_TRUE(original.Create("stable", "old", {}).ok());
  ASSERT_TRUE(Snapshot::Save(directory_, original, 1, 1).ok());
  std::filesystem::create_directory(directory_ / "snapshot.tmp");
  ASSERT_TRUE(original.Modify("stable", "new", {}).ok());
  EXPECT_EQ(Snapshot::Save(directory_, original, 2, 2).code(), StatusCode::kIoError);
  HashEngine restored(8);
  ASSERT_TRUE(Snapshot::Load(directory_, restored).ok());
  EXPECT_EQ(restored.Get("stable", {}).value(), "old");
}

TEST_F(PersistenceTest, ConcurrentDispatcherProducesStrictlyOrderedEventsAndQueueDrains) {
  std::vector<std::uint64_t> offsets;
  WriteEventQueue queue(512, 1024 * 1024, [&offsets](const std::vector<WriteEvent>& events) {
    for (const auto& event : events) offsets.push_back(event.offset);
    return Status::Ok();
  });
  Dispatcher dispatcher(
      std::make_unique<HashEngine>(512),
      [&queue](const WriteEvent& event) { return queue.Submit(event); }, {}, {}, 1, 1,
      "concurrent-node");
  dispatcher.SetBatchEventSink(
      [&queue](const std::vector<WriteEvent>& events) { return queue.SubmitBatch(events); });
  std::vector<std::jthread> writers;
  for (int thread = 0; thread < 4; ++thread) {
    writers.emplace_back([&dispatcher, thread] {
      for (int operation = 0; operation < 100; ++operation) {
        const auto response = dispatcher.Execute(
            {CommandType::kSet,
             {"key-" + std::to_string(thread) + "-" + std::to_string(operation), "value"}});
        EXPECT_TRUE(response.ok);
      }
    });
  }
  writers.clear();
  queue.Stop();
  ASSERT_EQ(offsets.size(), 400U);
  for (std::size_t index = 0; index < offsets.size(); ++index)
    EXPECT_EQ(offsets[index], index + 1U);
}

TEST_F(PersistenceTest, AofRecoversEveryUncommittedTransactionRegion) {
  const auto source_directory = directory_ / "source";
  {
    AofWriter writer(source_directory, 10, 1024, 10, "always");
    ASSERT_TRUE(
        writer.Append({1, 1, WriteSource::kClient, CommandType::kSet, "key", "value"}).ok());
  }
  std::ifstream source(source_directory / "appendonly.aof", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(source)),
                          std::istreambuf_iterator<char>());
  ASSERT_GT(bytes.size(), 52U);
  const std::array<std::size_t, 6> truncation_points{1, 3, 5, 43, 45, bytes.size() - 1U};
  for (const std::size_t length : truncation_points) {
    const auto truncated_directory = directory_ / ("tail-" + std::to_string(length));
    std::filesystem::create_directories(truncated_directory);
    {
      std::ofstream output(truncated_directory / "appendonly.aof", std::ios::binary);
      output.write(bytes.data(), static_cast<std::streamsize>(length));
    }
    AofWriter writer(truncated_directory, 10, 1024, 10, "always");
    HashEngine restored(8);
    const auto replayed = writer.Replay(restored);
    ASSERT_TRUE(replayed.ok()) << "length=" << length << ' ' << replayed.status().message();
    EXPECT_EQ(replayed.value(), 0U);
    EXPECT_EQ(std::filesystem::file_size(writer.path()), 0U);
  }
}

TEST_F(PersistenceTest, AofReplaysAtomicMultiDeleteEvent) {
  AofWriter writer(directory_, 10, 1024, 10, "always");
  Dispatcher dispatcher(std::make_unique<HashEngine>(8),
                        [&writer](const WriteEvent& event) { return writer.Append(event); });
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"a", "1"}, WriteSource::kClient, true}).ok);
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"b", "2"}, WriteSource::kClient, true}).ok);
  ASSERT_TRUE(dispatcher.Execute({CommandType::kDel, {"a", "b"}, WriteSource::kClient, true}).ok);
  HashEngine restored(8);
  ASSERT_TRUE(writer.Replay(restored).ok());
  EXPECT_FALSE(restored.Exists("a", {}).value());
  EXPECT_FALSE(restored.Exists("b", {}).value());
}

TEST_F(PersistenceTest, WriteEventQueueHonorsDeadlineAndCancellationWhenFull) {
  std::promise<void> release;
  auto gate = release.get_future().share();
  WriteEventQueue queue(1, 256, [&gate](const std::vector<WriteEvent>&) {
    gate.wait();
    return Status::Ok();
  });
  std::jthread first([&queue] {
    EXPECT_TRUE(queue.Submit({1, 1, WriteSource::kClient, CommandType::kSet, "a", "1"}).ok());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::jthread second([&queue] {
    EXPECT_TRUE(queue.Submit({2, 2, WriteSource::kClient, CommandType::kSet, "b", "2"}).ok());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(queue
                .Submit({3, 3, WriteSource::kClient, CommandType::kSet, "c", "3"},
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(20))
                .code(),
            StatusCode::kBusy);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_EQ(queue
                .Submit({3, 3, WriteSource::kClient, CommandType::kSet, "c", "3"},
                        std::chrono::steady_clock::time_point::max(), stop.get_token())
                .code(),
            StatusCode::kCancelled);
  release.set_value();
}

TEST_F(PersistenceTest, WriteEventQueueDeadlineCoversConsumerCompletion) {
  std::promise<void> release;
  auto gate = release.get_future().share();
  WriteEventQueue queue(1, 256, [&gate](const std::vector<WriteEvent>&) {
    gate.wait();
    return Status::Ok();
  });
  const auto status =
      queue.Submit({1, 1, WriteSource::kClient, CommandType::kSet, "a", "1"},
                   std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
  EXPECT_EQ(status.code(), StatusCode::kInternal);
  release.set_value();
  queue.Stop();
}

}  // namespace
}  // namespace kvstore
