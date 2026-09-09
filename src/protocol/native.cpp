#include "kvstore/protocol/native.hpp"

#include <charconv>
#include <sstream>

#include "kvstore/protocol/resp.hpp"

namespace kvstore {
namespace {

Result<std::size_t> ParseSize(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return Status{StatusCode::kInvalidArgument, "invalid native length"};
  }
  return value;
}

}  // namespace

Status NativeParser::Feed(std::string_view bytes) {
  if (bytes.size() > max_frame_bytes_ - buffer_.size())
    return {StatusCode::kLimitExceeded, "native buffer limit"};
  buffer_.append(bytes);
  return Status::Ok();
}

Result<std::vector<Command>> NativeParser::ParseAvailable() {
  std::vector<Command> output;
  while (true) {
    const auto header_end = buffer_.find("\r\n");
    if (header_end == std::string::npos) break;
    std::istringstream header(buffer_.substr(0, header_end));
    std::string version;
    std::string name;
    std::string key_text;
    std::string value_text;
    if (!(header >> version >> name >> key_text >> value_text) || version != "KV/1") {
      return Status{StatusCode::kInvalidArgument, "invalid native header"};
    }
    auto key_size = ParseSize(key_text);
    auto value_size = ParseSize(value_text);
    if (!key_size.ok()) return key_size.status();
    if (!value_size.ok()) return value_size.status();
    if (key_size.value() == 0 || key_size.value() > max_key_bytes_ ||
        value_size.value() > max_value_bytes_) {
      return Status{StatusCode::kLimitExceeded, "native key/value limit"};
    }
    const auto payload_start = header_end + 2;
    if (payload_start > buffer_.size() || key_size.value() > buffer_.size() - payload_start ||
        value_size.value() > buffer_.size() - payload_start - key_size.value())
      break;
    const auto total = key_size.value() + value_size.value();
    if (total > max_frame_bytes_ - payload_start)
      return Status{StatusCode::kLimitExceeded, "native frame limit"};
    std::string key = buffer_.substr(payload_start, key_size.value());
    std::string value = buffer_.substr(payload_start + key_size.value(), value_size.value());
    CommandType type;
    if (name == "SET")
      type = CommandType::kSet;
    else if (name == "GET")
      type = CommandType::kGet;
    else if (name == "DEL")
      type = CommandType::kDel;
    else if (name == "MOD")
      type = CommandType::kMod;
    else if (name == "EXIST")
      type = CommandType::kExist;
    else if (name == "SAVE")
      type = CommandType::kSave;
    else if (name == "LOAD")
      type = CommandType::kLoad;
    else
      return Status{StatusCode::kUnsupported, "unknown native command"};
    const bool takes_value = type == CommandType::kSet || type == CommandType::kMod;
    if (!takes_value && value_size.value() != 0)
      return Status{StatusCode::kInvalidArgument, "unexpected native value"};
    output.push_back({type,
                      takes_value ? std::vector<std::string>{std::move(key), std::move(value)}
                                  : std::vector<std::string>{std::move(key)},
                      WriteSource::kClient});
    buffer_.erase(0, payload_start + total);
  }
  return output;
}

std::string EncodeNative(const CommandResponse& response) {
  const std::string payload = response.ok ? response.value : response.error;
  return "KV/1 " + std::string(response.ok ? "OK" : "ERROR") + " " +
         std::to_string(payload.size()) + " 0\r\n" + payload;
}

}  // namespace kvstore
