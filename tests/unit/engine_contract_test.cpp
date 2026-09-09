#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/engine/array_engine.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/engine/rbtree_engine.hpp"
#include "kvstore/engine/skiplist_engine.hpp"

namespace kvstore {
namespace {

std::vector<std::unique_ptr<IEngine>> Engines() {
  std::vector<std::unique_ptr<IEngine>> engines;
  engines.push_back(std::make_unique<ArrayEngine>(8));
  engines.push_back(std::make_unique<RBTreeEngine>(8));
  engines.push_back(std::make_unique<HashEngine>(8, 4));
  engines.push_back(std::make_unique<SkipListEngine>(8, 8, 0.5, 7));
  return engines;
}

TEST(EngineContractTest, CRUDAndAtomicPrimitivesAgree) {
  for (auto& engine : Engines()) {
    EXPECT_EQ(engine->Create("a", "1"), Status::Ok());
    EXPECT_EQ(engine->Create("a", "2").code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(engine->Modify("missing", "x").code(), StatusCode::kNotFound);
    EXPECT_EQ(engine->Modify("a", "2"), Status::Ok());
    EXPECT_EQ(engine->Get("a").value(), "2");
    EXPECT_FALSE(engine->Upsert("a", "3").value());
    EXPECT_TRUE(engine->Upsert("b", "4").value());
    EXPECT_EQ(engine->Increment("counter", 2).value(), 2);
    EXPECT_EQ(engine->Increment("counter", -1).value(), 1);
    EXPECT_EQ(engine->Delete("a"), Status::Ok());
    EXPECT_EQ(engine->Get("a").status().code(), StatusCode::kNotFound);
  }
}

TEST(EngineContractTest, RejectsInvalidIntegerAndOverflow) {
  for (auto& engine : Engines()) {
    ASSERT_TRUE(
        engine->Create("counter", std::to_string(std::numeric_limits<std::int64_t>::max())).ok());
    EXPECT_EQ(engine->Increment("counter", 1).status().code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(engine->Modify("counter", "01").ok());
    EXPECT_EQ(engine->Increment("counter", 1).status().code(), StatusCode::kInvalidArgument);
  }
}

TEST(EngineContractTest, CancellationDoesNotMutate) {
  std::stop_source source;
  source.request_stop();
  for (auto& engine : Engines()) {
    EXPECT_EQ(engine->Create("a", "1", source.get_token()).code(), StatusCode::kCancelled);
    EXPECT_EQ(engine->Size(), 0U);
  }
}

TEST(EngineContractTest, ImportIsAtomicAndScanIsSorted) {
  for (auto& engine : Engines()) {
    ASSERT_TRUE(engine->Create("old", "value").ok());
    const std::vector<Entry> entries{{"z", "3"}, {"a", "1"}};
    ASSERT_TRUE(engine->Import(entries).ok());
    EXPECT_EQ(engine->Size(), 2U);
    auto scan = engine->Scan();
    ASSERT_TRUE(scan.ok());
    ASSERT_EQ(scan.value().size(), 2U);
    EXPECT_EQ(scan.value()[0].key, "a");
    EXPECT_EQ(scan.value()[1].key, "z");
  }
}

TEST(EngineContractTest, ImportFailuresPreserveOldState) {
  const std::vector<std::vector<Entry>> invalid{{{"a", "1"}, {"a", "2"}},
                                                {{"", "1"}},
                                                {{"a", "1"},
                                                 {"b", "2"},
                                                 {"c", "3"},
                                                 {"d", "4"},
                                                 {"e", "5"},
                                                 {"f", "6"},
                                                 {"g", "7"},
                                                 {"h", "8"},
                                                 {"i", "9"}}};
  for (auto& engine : Engines()) {
    ASSERT_TRUE(engine->Create("old", "value").ok());
    for (const auto& entries : invalid) {
      EXPECT_FALSE(engine->Import(entries).ok());
      const auto state = engine->Export();
      ASSERT_TRUE(state.ok());
      ASSERT_EQ(state.value(), (std::vector<Entry>{{"old", "value"}}));
      EXPECT_FALSE(engine->Exists("a").value());
      EXPECT_FALSE(engine->Exists("new").value());
    }
    std::stop_source source;
    source.request_stop();
    EXPECT_EQ(engine->Import({{"new", "value"}}, source.get_token()).code(),
              StatusCode::kCancelled);
    ASSERT_EQ(engine->Export().value(), (std::vector<Entry>{{"old", "value"}}));
    EXPECT_TRUE(engine->Exists("old").value());
  }
}

TEST(EngineContractTest, CapacityLargeValuesAndBinaryKeys) {
  const std::string binary_key("a\0b", 3);
  std::string large_value(1024 * 1024, 'x');
  large_value[5] = '\0';
  for (auto& engine : Engines()) {
    ASSERT_TRUE(engine->Create(binary_key, large_value).ok());
    EXPECT_EQ(engine->Get(binary_key).value(), large_value);
    for (int index = 1; index < 8; ++index) {
      ASSERT_TRUE(engine->Create("key" + std::to_string(index), "v").ok());
    }
    EXPECT_EQ(engine->Create("overflow", "v").code(), StatusCode::kLimitExceeded);
  }
}

TEST(EngineContractTest, ExportExistsEmptyKeysAndWriteCancellationAgree) {
  for (auto& engine : Engines()) {
    EXPECT_EQ(engine->Create("", "v").code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(engine->Exists("").status().code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(engine->Create("key", "value").ok());
    EXPECT_TRUE(engine->Exists("key").value());
    EXPECT_EQ(engine->Export().value(), engine->Scan().value());

    std::stop_source source;
    source.request_stop();
    EXPECT_EQ(engine->Modify("key", "changed", source.get_token()).code(), StatusCode::kCancelled);
    EXPECT_EQ(engine->Delete("key", source.get_token()).code(), StatusCode::kCancelled);
    EXPECT_EQ(engine->Upsert("key", "changed", source.get_token()).status().code(),
              StatusCode::kCancelled);
    EXPECT_EQ(engine->Increment("counter", 1, source.get_token()).status().code(),
              StatusCode::kCancelled);
    EXPECT_EQ(engine->Get("key").value(), "value");
    EXPECT_FALSE(engine->Exists("counter").value());
  }
}

TEST(EngineContractTest, IncrementIsAtomicAcrossThreads) {
  for (auto& engine : Engines()) {
    std::vector<std::jthread> threads;
    for (int thread = 0; thread < 8; ++thread) {
      threads.emplace_back([&engine] {
        for (int operation = 0; operation < 1000; ++operation) {
          EXPECT_TRUE(engine->Increment("counter", 1).ok());
        }
      });
    }
    threads.clear();
    ASSERT_TRUE(engine->Get("counter").ok());
    EXPECT_EQ(engine->Get("counter").value(), "8000");
  }
}

TEST(EngineContractTest, StructuralInvariantsSurviveInsertAndDelete) {
  RBTreeEngine tree(1000);
  SkipListEngine skiplist(1000, 16, 0.5, 123);
  for (int index = 0; index < 500; ++index) {
    const auto key = std::to_string(index);
    ASSERT_TRUE(tree.Create(key, "v", {}).ok());
    ASSERT_TRUE(skiplist.Create(key, "v", {}).ok());
    ASSERT_TRUE(tree.ValidateInvariants());
    ASSERT_TRUE(skiplist.ValidateInvariants());
  }
  for (int index = 499; index >= 0; --index) {
    const auto key = std::to_string(index);
    ASSERT_TRUE(tree.Delete(key, {}).ok());
    ASSERT_TRUE(skiplist.Delete(key, {}).ok());
    ASSERT_TRUE(tree.ValidateInvariants());
    ASSERT_TRUE(skiplist.ValidateInvariants());
  }
}

TEST(EngineContractTest, RBTreeHandlesOrderedReverseAndRandomSequences) {
  RBTreeEngine ascending(1000);
  RBTreeEngine descending(1000);
  std::vector<int> order(500);
  std::iota(order.begin(), order.end(), 0);
  for (const int index : order) {
    const auto key = "key-" + std::to_string(1000 + index);
    ASSERT_TRUE(ascending.Create(key, "v", {}).ok());
    ASSERT_TRUE(ascending.ValidateInvariants());
  }
  for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
    const auto key = "key-" + std::to_string(1000 + *iterator);
    ASSERT_TRUE(descending.Create(key, "v", {}).ok());
    ASSERT_TRUE(descending.ValidateInvariants());
  }
  std::mt19937 generator(42);
  std::shuffle(order.begin(), order.end(), generator);
  for (const int index : order) {
    const auto key = "key-" + std::to_string(1000 + index);
    ASSERT_TRUE(ascending.Delete(key, {}).ok());
    ASSERT_TRUE(ascending.ValidateInvariants());
  }
}

TEST(EngineContractTest, HashRehashPreservesEntries) {
  HashEngine hash(5000, 1, 0.5F);
  for (int index = 0; index < 5000; ++index) {
    ASSERT_TRUE(hash.Create("key" + std::to_string(index), "value", {}).ok());
  }
  for (int index = 0; index < 5000; ++index) {
    EXPECT_EQ(hash.Get("key" + std::to_string(index), {}).value(), "value");
  }
}

}  // namespace
}  // namespace kvstore
