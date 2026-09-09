#include <gtest/gtest.h>

#include <cstdlib>

TEST(CliTest, VersionAndHelpAreAvailable) {
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --version > /dev/null"), 0);
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --help > /dev/null"), 0);
}

TEST(CliTest, DefaultConfigPassesValidation) {
  EXPECT_EQ(std::system(KVSTORE_SERVER_PATH " --config " KVSTORE_DEFAULT_CONFIG
                                            " --check-config > /dev/null"),
            0);
}
