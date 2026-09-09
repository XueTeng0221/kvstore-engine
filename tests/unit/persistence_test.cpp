#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/persistence/aof.hpp"
#include "kvstore/persistence/snapshot.hpp"

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
  ASSERT_TRUE(writer.Replay(restored).ok());
  EXPECT_EQ(restored.Get("b", {}).value(), "v");
  std::ofstream truncate(writer.path(), std::ios::binary | std::ios::trunc);
  truncate << "1 1 0 2 3\nxy";
  truncate.close();
  EXPECT_EQ(writer.Replay(restored).status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kvstore
