/**
 * @file tests/unit/app/file_save_activity_test.cpp
 * @brief Tests save activity lifetime across simultaneous file writers.
 */
// test header include
#include "src/app/file_save_activity.h"

// standard includes
#include <future>
#include <thread>

// lib includes
#include <lizardbyte/common/testing.h>

/**
 * @brief Verify an early return clears the only tracked save.
 */
TEST(FileSaveActivityTest, ClearsOnEarlyReturn) {
  const auto before = app::file_save_snapshot();
  EXPECT_FALSE(before.active);
  const auto failedWrite = [] {
    const app::FileSaveActivity saving;
    EXPECT_TRUE(app::file_save_snapshot().active);
    return false;
  };
  EXPECT_FALSE(failedWrite());
  const auto after = app::file_save_snapshot();
  EXPECT_FALSE(after.active);
  EXPECT_EQ(after.generation, before.generation + 1);
}

/**
 * @brief Verify the indicator stays active until both the shell and its background writer finish.
 */
TEST(FileSaveActivityTest, RemainsActiveUntilTheLastWriterFinishes) {
  const auto before = app::file_save_snapshot();
  std::promise<void> entered;
  std::promise<void> finish;
  auto enteredFuture = entered.get_future();
  auto finishFuture = finish.get_future();
  std::thread worker([&entered, &finishFuture] {
    const app::FileSaveActivity saving;
    entered.set_value();
    finishFuture.wait();
  });
  enteredFuture.wait();
  {
    const app::FileSaveActivity saving;
    EXPECT_TRUE(app::file_save_snapshot().active);
    EXPECT_EQ(app::file_save_snapshot().generation, before.generation + 1);
  }
  EXPECT_TRUE(app::file_save_snapshot().active);
  finish.set_value();
  worker.join();
  EXPECT_FALSE(app::file_save_snapshot().active);
  EXPECT_EQ(app::file_save_snapshot().generation, before.generation + 1);
}
