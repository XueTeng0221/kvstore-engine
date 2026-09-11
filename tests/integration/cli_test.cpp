#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

TEST(CliTest, VersionAndHelpAreAvailable) {
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --version > /dev/null"), 0);
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --help > /dev/null"), 0);
}

TEST(CliTest, DefaultConfigPassesValidation) {
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --config " KVSTORE_DEFAULT_CONFIG
                                            " --check-config > /dev/null"),
            0);
}

namespace {

std::uint16_t ReservePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  EXPECT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  ::close(fd);
  return ntohs(address.sin_port);
}

pid_t Spawn(const std::filesystem::path& config) {
  const auto pid = ::fork();
  if (pid == 0) {
    ::execl(KVSTORE_SERVER_PATH, KVSTORE_SERVER_PATH, "--config", config.c_str(), nullptr);
    _exit(127);
  }
  return pid;
}

int Connect(std::uint16_t port) {
  for (int retry = 0; retry < 100; ++retry) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

std::string Request(int fd, std::string_view request) {
  EXPECT_EQ(::send(fd, request.data(), request.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(request.size()));
  char buffer[256];
  const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
  EXPECT_GT(count, 0);
  return count > 0 ? std::string(buffer, static_cast<std::size_t>(count)) : std::string{};
}

}  // namespace

TEST(CliTest, ProcessCrudGracefulStopAndRestartRecovery) {
  const auto directory =
      std::filesystem::temp_directory_path() / ("kvstore-restart-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto config_path = directory / "config.json";
  std::ifstream source(KVSTORE_DEFAULT_CONFIG);
  auto config = nlohmann::json::parse(source);
  const auto port = ReservePort();
  config["server"]["port"] = port;
  config["persistence"]["directory"] = directory.string();
  config["persistence"]["flush_interval_ms"] = 20;
  config["observability"]["metrics_enabled"] = false;
  std::ofstream output(config_path);
  output << config;
  output.close();

  auto pid = Spawn(config_path);
  ASSERT_GT(pid, 0);
  int fd = Connect(port);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(Request(fd, "*3\r\n$3\r\nSET\r\n$7\r\nrestart\r\n$5\r\nvalue\r\n"), "+OK\r\n");
  ::close(fd);
  ASSERT_EQ(::kill(pid, SIGTERM), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  pid = Spawn(config_path);
  ASSERT_GT(pid, 0);
  fd = Connect(port);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(Request(fd, "*2\r\n$3\r\nGET\r\n$7\r\nrestart\r\n"), "$5\r\nvalue\r\n");
  ::close(fd);
  ASSERT_EQ(::kill(pid, SIGTERM), 0);
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  std::filesystem::remove_all(directory);
}
