#include "kvstore/protocol/resp.hpp"

#include <cctype>
#include <charconv>

namespace kvstore {
namespace {

Result<std::size_t> Number(std::string_view value) {
  std::size_t result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return Status{StatusCode::kInvalidArgument, "invalid RESP length"};
  }
  return result;
}

Result<std::optional<std::vector<std::string>>> ParseOne(std::string_view input) {
  if (input.empty()) return std::optional<std::vector<std::string>>{};
  if (input.front() != '*')
    return Status{StatusCode::kInvalidArgument, "RESP command must be array"};
  const auto line_end = input.find("\r\n");
  if (line_end == std::string_view::npos) return std::optional<std::vector<std::string>>{};
  auto count = Number(input.substr(1, line_end - 1));
  if (!count.ok()) return count.status();
  if (count.value() == 0 || count.value() > 1024)
    return Status{StatusCode::kLimitExceeded, "RESP array size"};
  std::size_t position = line_end + 2;
  std::vector<std::string> output;
  output.reserve(count.value());
  for (std::size_t index = 0; index < count.value(); ++index) {
    if (position >= input.size()) return std::optional<std::vector<std::string>>{};
    if (input[position] != '$')
      return Status{StatusCode::kInvalidArgument, "RESP item must be bulk string"};
    const auto length_end = input.find("\r\n", position);
    if (length_end == std::string_view::npos) return std::optional<std::vector<std::string>>{};
    auto length = Number(input.substr(position + 1, length_end - position - 1));
    if (!length.ok()) return length.status();
    position = length_end + 2;
    if (position > input.size() || input.size() - position < 2 ||
        length.value() > input.size() - position - 2)
      return std::optional<std::vector<std::string>>{};
    output.emplace_back(input.substr(position, length.value()));
    position += length.value();
    if (input.substr(position, 2) != "\r\n")
      return Status{StatusCode::kInvalidArgument, "RESP bulk terminator"};
    position += 2;
  }
  return std::optional<std::vector<std::string>>(std::move(output));
}

std::string Bulk(std::string_view value) {
  return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

}  // namespace

Status RespParser::Feed(std::string_view bytes) {
  if (bytes.size() > max_frame_bytes_ - buffer_.size())
    return {StatusCode::kLimitExceeded, "RESP buffer limit"};
  buffer_.append(bytes);
  return Status::Ok();
}

Result<std::vector<Command>> RespParser::ParseAvailable() {
  std::vector<Command> commands;
  while (!buffer_.empty()) {
    auto parsed = ParseOne(buffer_);
    if (!parsed.ok()) return parsed.status();
    if (!parsed.value().has_value()) break;
    auto parts = std::move(parsed.value().value());
    auto command = CommandFromResp(parts);
    if (!command.ok()) return command.status();
    std::size_t consumed = buffer_.find("\r\n") + 2;
    for (const auto& part : parts) consumed += part.size() + std::to_string(part.size()).size() + 5;
    buffer_.erase(0, consumed);
    commands.push_back(std::move(command).value());
  }
  return commands;
}

Result<Command> CommandFromResp(const std::vector<std::string>& parts) {
  if (parts.empty()) return Status{StatusCode::kInvalidArgument, "empty command"};
  std::string name = parts.front();
  for (char& character : name)
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  CommandType type;
  if (name == "SET")
    type = CommandType::kSet;
  else if (name == "GET")
    type = CommandType::kGet;
  else if (name == "DEL")
    type = CommandType::kDel;
  else if (name == "EXISTS")
    type = CommandType::kExist;
  else if (name == "MGET")
    type = CommandType::kMget;
  else if (name == "INCR")
    type = CommandType::kIncr;
  else if (name == "DECR")
    type = CommandType::kDecr;
  else if (name == "INCRBY")
    type = CommandType::kIncrBy;
  else if (name == "DECRBY")
    type = CommandType::kDecrBy;
  else if (name == "PING")
    type = CommandType::kPing;
  else if (name == "ECHO")
    type = CommandType::kEcho;
  else if (name == "CLIENT")
    type = CommandType::kClient;
  else if (name == "INFO")
    type = CommandType::kInfo;
  else if (name == "SAVE")
    type = CommandType::kSave;
  else
    type = CommandType::kUnknown;
  return Command{type, {parts.begin() + 1, parts.end()}, WriteSource::kClient, true};
}

std::string EncodeResp(const CommandResponse& response) {
  if (!response.ok) return "-ERR " + response.error + "\r\n";
  if (response.null_value) return "$-1\r\n";
  if (!response.array.empty()) {
    std::string output = "*" + std::to_string(response.array.size()) + "\r\n";
    for (const auto& item : response.array) output += EncodeResp(item);
    return output;
  }
  if (response.integer) return ":" + response.value + "\r\n";
  if (response.simple_string) return "+" + response.value + "\r\n";
  return Bulk(response.value);
}

}  // namespace kvstore
