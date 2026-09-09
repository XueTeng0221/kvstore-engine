#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvstore/engine/array_engine.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/engine/rbtree_engine.hpp"
#include "kvstore/engine/skiplist_engine.hpp"

namespace kvstore {
namespace {

std::unique_ptr<IEngine> NewEngine(std::size_t index) {
  if (index == 0) return std::make_unique<ArrayEngine>(128);
  if (index == 1) return std::make_unique<RBTreeEngine>(128);
  if (index == 2) return std::make_unique<HashEngine>(128, 8);
  return std::make_unique<SkipListEngine>(128, 12, 0.5, 99);
}

TEST(EngineDifferentialTest, DeterministicRandomWorkload) {
  std::vector<std::unique_ptr<IEngine>> engines;
  for (std::size_t index = 0; index < 4; ++index) engines.push_back(NewEngine(index));
  std::unordered_map<std::string, std::string> model;
  std::mt19937 generator(12345);
  constexpr int kOperations = 1000000;
  for (int operation = 0; operation < kOperations; ++operation) {
    const std::string key = "key" + std::to_string(generator() % 64);
    const std::string value = "value" + std::to_string(generator() % 1000);
    const int action = static_cast<int>(generator() % 4);
    Status expected = Status::Ok();
    if (action == 0) {
      const auto [iterator, inserted] = model.emplace(key, value);
      expected = inserted ? Status::Ok() : Status{StatusCode::kAlreadyExists, ""};
      for (auto& engine : engines) EXPECT_EQ(engine->Create(key, value).code(), expected.code());
      if (!inserted) static_cast<void>(iterator);
    } else if (action == 1) {
      const auto iterator = model.find(key);
      expected = iterator == model.end() ? Status{StatusCode::kNotFound, ""} : Status::Ok();
      if (iterator != model.end()) iterator->second = value;
      for (auto& engine : engines) EXPECT_EQ(engine->Modify(key, value).code(), expected.code());
    } else if (action == 2) {
      expected = model.erase(key) == 0 ? Status{StatusCode::kNotFound, ""} : Status::Ok();
      for (auto& engine : engines) EXPECT_EQ(engine->Delete(key).code(), expected.code());
    } else {
      const bool inserted = !model.contains(key);
      model[key] = value;
      for (auto& engine : engines) EXPECT_EQ(engine->Upsert(key, value).value(), inserted);
    }
    if (operation % 1000 != 0 && operation + 1 != kOperations) continue;
    for (auto& engine : engines) {
      const auto scan = engine->Scan();
      ASSERT_TRUE(scan.ok());
      std::vector<Entry> expected_entries;
      for (const auto& [model_key, model_value] : model)
        expected_entries.push_back({model_key, model_value});
      std::sort(expected_entries.begin(), expected_entries.end(),
                [](const Entry& left, const Entry& right) { return left.key < right.key; });
      EXPECT_EQ(scan.value(), expected_entries);
      if (const auto* tree = dynamic_cast<RBTreeEngine*>(engine.get()); tree != nullptr) {
        EXPECT_TRUE(tree->ValidateInvariants());
      }
      if (const auto* skiplist = dynamic_cast<SkipListEngine*>(engine.get()); skiplist != nullptr) {
        EXPECT_TRUE(skiplist->ValidateInvariants());
      }
    }
  }
}

}  // namespace
}  // namespace kvstore
