#include "kvstore/engine/engine_factory.hpp"

#include "kvstore/engine/array_engine.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/engine/rbtree_engine.hpp"
#include "kvstore/engine/skiplist_engine.hpp"

namespace kvstore {

Result<std::unique_ptr<IEngine>> CreateEngine(const EngineConfig& config) {
  if (config.capacity == 0) {
    return Status{StatusCode::kInvalidArgument, "engine capacity must be positive"};
  }
  if (config.type == "array") {
    std::unique_ptr<IEngine> engine =
        std::make_unique<ArrayEngine>(config.capacity, config.array_reserve);
    return engine;
  }
  if (config.type == "rbtree") {
    std::unique_ptr<IEngine> engine = std::make_unique<RBTreeEngine>(config.capacity);
    return engine;
  }
  if (config.type == "hash") {
    std::unique_ptr<IEngine> engine =
        std::make_unique<HashEngine>(config.capacity, config.hash_initial_buckets,
                                     static_cast<float>(config.hash_max_load_factor));
    return engine;
  }
  if (config.type == "skiplist") {
    std::unique_ptr<IEngine> engine =
        std::make_unique<SkipListEngine>(config.capacity, config.skiplist_max_level,
                                         config.skiplist_probability, config.skiplist_seed);
    return engine;
  }
  return Status{StatusCode::kInvalidArgument, "unknown engine type"};
}

Result<std::unique_ptr<IEngine>> CreateEngine(std::string_view type, std::size_t capacity) {
  EngineConfig config{.type = std::string(type),
                      .capacity = capacity,
                      .array_reserve = 16,
                      .hash_initial_buckets = 16,
                      .hash_max_load_factor = 0.75,
                      .skiplist_max_level = 20,
                      .skiplist_probability = 0.5,
                      .skiplist_seed = 0xc0ffee};
  return CreateEngine(config);
}

}  // namespace kvstore
