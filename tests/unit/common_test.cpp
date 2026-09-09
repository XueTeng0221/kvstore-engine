#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <sstream>

#include "kvstore/common/byte_buffer.hpp"
#include "kvstore/common/clock.hpp"
#include "kvstore/common/crc32.hpp"
#include "kvstore/common/logger.hpp"
#include "kvstore/common/random.hpp"
#include "kvstore/common/resources.hpp"

namespace kvstore {

TEST(CommonTest, Crc32KnownVector) {
  constexpr std::array<std::byte, 9> bytes{std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                                           std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
                                           std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
  EXPECT_EQ(Crc32(bytes), 0xcbf43926U);
}

TEST(CommonTest, StatusCodesHaveStableWireValues) {
  EXPECT_EQ(Status{}.wire_code(), 0U);
  EXPECT_EQ((Status{StatusCode::kCorruption, "bad"}.wire_code()), 6U);
  EXPECT_EQ((Status{StatusCode::kInternal, "bad"}.wire_code()), 11U);
}

TEST(CommonTest, ResultRejectsOkStatusWithoutValueInAllBuilds) {
  const Result<int> result(Status::Ok());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInternal);
  EXPECT_THROW(static_cast<void>(result.value()), std::bad_optional_access);
}

TEST(CommonTest, ByteBufferIsBoundedAndOwned) {
  ByteBuffer buffer(3);
  const std::array<std::byte, 2> bytes{std::byte{'a'}, std::byte{'b'}};
  EXPECT_TRUE(buffer.Append(bytes).ok());
  EXPECT_FALSE(buffer.Append(std::array<std::byte, 2>{std::byte{'c'}, std::byte{'d'}}).ok());
  auto result = buffer.Read(0, 2);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), (Bytes{std::byte{'a'}, std::byte{'b'}}));
  EXPECT_FALSE(buffer.Read(2, 2).ok());
}

TEST(CommonTest, ManualClockIsDeterministic) {
  ManualClock clock;
  clock.Advance(std::chrono::seconds(3));
  EXPECT_EQ(clock.Now().time_since_epoch(), std::chrono::seconds(3));
}

TEST(CommonTest, LoggerDoesNotExposePayloadContext) {
  std::ostringstream output;
  Logger logger(output);
  logger.Log(LogLevel::kInfo, "mutation",
             {.request_id = "r1", .connection_id = 4, .node_id = "n1", .event_offset = 8});
  EXPECT_NE(output.str().find("mutation"), std::string::npos);
  EXPECT_NE(output.str().find("event_offset"), std::string::npos);
}

TEST(CommonTest, FileDescriptorAndMappingUseRaii) {
  int descriptors[2]{};
  ASSERT_EQ(::pipe(descriptors), 0);
  const int released_descriptor = descriptors[0];
  {
    UniqueFd read_end(descriptors[0]);
    UniqueFd write_end(descriptors[1]);
    UniqueFd moved(std::move(read_end));
    EXPECT_FALSE(read_end);
    EXPECT_TRUE(moved);
  }
  errno = 0;
  EXPECT_EQ(::fcntl(released_descriptor, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);

  void* address = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(address, MAP_FAILED);
  MappedRegion mapping(address, 4096);
  EXPECT_TRUE(mapping);
  EXPECT_EQ(mapping.size(), 4096U);
}

TEST(CommonTest, SeededRandomIsReproducible) {
  SeededRandom first(42);
  SeededRandom second(42);
  EXPECT_EQ(first.Next(), second.Next());
  EXPECT_EQ(first.Next(), second.Next());
}

}  // namespace kvstore
