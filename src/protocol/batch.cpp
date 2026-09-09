#include "kvstore/protocol/batch.hpp"

#include <cstdint>

namespace kvstore {
namespace {

template <typename T>
T Read(std::string_view input, std::size_t position) {
  T value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value = static_cast<T>((value << 8U) | static_cast<unsigned char>(input[position + index]));
  }
  return value;
}

template <typename T>
void Append(std::string& output, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<char>(value >> ((sizeof(T) - index - 1) * 8U)));
  }
}

}  // namespace

Status BatchParser::Feed(std::string_view bytes) {
  if (bytes.size() > max_frame_ - buffer_.size())
    return {StatusCode::kLimitExceeded, "batch buffer limit"};
  buffer_.append(bytes);
  return Status::Ok();
}

Result<std::vector<Command>> BatchParser::ParseAvailable() {
  std::vector<Command> output;
  while (true) {
    if (buffer_.size() < 20) break;
    if (buffer_.substr(0, 4) != "KVB1") return Status{StatusCode::kInvalidArgument, "batch magic"};
    const auto version = Read<std::uint16_t>(buffer_, 4);
    const auto records = Read<std::uint32_t>(buffer_, 8);
    const auto payload_size = Read<std::uint64_t>(buffer_, 12);
    if (version != 1 || records == 0 || records > max_records_ || payload_size > max_frame_ - 20)
      return Status{StatusCode::kInvalidArgument, "batch header"};
    if (payload_size > buffer_.size() - 20) break;
    std::size_t position = 20;
    const auto end = position + static_cast<std::size_t>(payload_size);
    for (std::uint32_t index = 0; index < records; ++index) {
      if (position >= end) return Status{StatusCode::kCorruption, "batch record bounds"};
      const auto type = static_cast<CommandType>(static_cast<unsigned char>(buffer_[position++]));
      if (type != CommandType::kSet && type != CommandType::kGet && type != CommandType::kDel &&
          type != CommandType::kMod && type != CommandType::kExist) {
        return Status{StatusCode::kUnsupported, "batch command"};
      }
      if (position > end || end - position < 12)
        return Status{StatusCode::kCorruption, "batch lengths"};
      const auto key_size = Read<std::uint32_t>(buffer_, position);
      const auto value_size = Read<std::uint64_t>(buffer_, position + 4);
      position += 12;
      if (key_size == 0 || value_size > end - position - key_size || key_size > end - position) {
        return Status{StatusCode::kCorruption, "batch record payload"};
      }
      std::string key = buffer_.substr(position, key_size);
      position += key_size;
      std::string value = buffer_.substr(position, static_cast<std::size_t>(value_size));
      position += static_cast<std::size_t>(value_size);
      const bool takes_value = type == CommandType::kSet || type == CommandType::kMod;
      if (!takes_value && !value.empty())
        return Status{StatusCode::kInvalidArgument, "batch unexpected value"};
      output.push_back({type,
                        takes_value ? std::vector<std::string>{std::move(key), std::move(value)}
                                    : std::vector<std::string>{std::move(key)},
                        WriteSource::kClient});
    }
    if (position != end) return Status{StatusCode::kCorruption, "batch trailing record bytes"};
    buffer_.erase(0, 20 + static_cast<std::size_t>(payload_size));
  }
  return output;
}

std::string EncodeBatch(const std::vector<CommandResponse>& responses) {
  std::string payload;
  for (const auto& response : responses) {
    const auto value = response.ok ? response.value : response.error;
    Append<std::uint16_t>(payload, response.ok ? 0 : 11);
    Append<std::uint64_t>(payload, value.size());
    payload.append(value);
  }
  std::string output("KVB1", 4);
  Append<std::uint16_t>(output, 1);
  Append<std::uint16_t>(output, 0);
  Append<std::uint32_t>(output, static_cast<std::uint32_t>(responses.size()));
  Append<std::uint64_t>(output, payload.size());
  output += payload;
  return output;
}

}  // namespace kvstore
