/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/dwio/common/ParallelFor.h"
#include "folly/Executor.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "folly/executors/InlineExecutor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "velox/common/base/VeloxException.h"

using namespace ::testing;
using namespace ::facebook::velox::dwio::common;

namespace {

class CountingExecutor : public folly::Executor {
 public:
  explicit CountingExecutor(folly::Executor& executor)
      : executor_(executor), count_(0) {}

  void add(folly::Func f) override {
    executor_.add(std::move(f));
    ++count_;
  }

  size_t getCount() const {
    return count_;
  }

 private:
  folly::Executor& executor_;
  size_t count_;
};

enum class MODE { INDEX, RANGE };

void testParallelFor(
    folly::Executor* executor,
    size_t from,
    size_t to,
    size_t parallelismFactor,
    MODE mode) {
  std::optional<CountingExecutor> countedExecutor;
  std::ostringstream oss;
  oss << "ParallelFor(executor: " << executor << ", from: " << from
      << ", to: " << to << ", parallelismFactor: " << parallelismFactor << ")";
  SCOPED_TRACE(oss.str());
  if (executor) {
    countedExecutor.emplace(*executor);
    executor = &countedExecutor.value();
  }

  std::unordered_map<size_t, std::atomic<size_t>> indexInvoked;
  for (size_t i = from; i < to; ++i) {
    indexInvoked[i] = 0UL;
  }

  switch (mode) {
    case MODE::INDEX:
      ParallelFor(executor, from, to, parallelismFactor)
          .execute([&indexInvoked](size_t i) {
            auto it = indexInvoked.find(i);
            ASSERT_NE(it, indexInvoked.end());
            ++it->second;
          });
      break;
    case MODE::RANGE:
      ParallelFor(executor, from, to, parallelismFactor)
          .execute([&indexInvoked](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
              auto it = indexInvoked.find(i);
              ASSERT_NE(it, indexInvoked.end());
              ++it->second;
            }
          });
      break;
  }

  // Parallel For should have thrown otherwise
  ASSERT_LE(from, to);

  // The method was called for each index just once, and didn't call out of
  // bounds indices.
  EXPECT_EQ(indexInvoked.size(), (to - from));
  for (auto& [i, count] : indexInvoked) {
    if (i < from || i >= to) {
      EXPECT_EQ(indexInvoked[i], 0);
    } else {
      EXPECT_EQ(indexInvoked[i], 1);
    }
  }

  if (countedExecutor) {
    const auto extraThreadsUsed = countedExecutor->getCount();
    const auto numTasks = to - from;
    const auto expectedExtraThreads = std::min(parallelismFactor, numTasks);
    EXPECT_EQ(
        extraThreadsUsed,
        (expectedExtraThreads > 1 ? expectedExtraThreads : 0));
  }
}

class ParallelForTest : public ::testing::TestWithParam<MODE> {};

} // namespace

TEST_P(ParallelForTest, E2E) {
  auto inlineExecutor = folly::InlineExecutor::instance();
  for (size_t parallelism = 0; parallelism < 25; ++parallelism) {
    for (size_t begin = 0; begin < 25; ++begin) {
      for (size_t end = 0; end < 25; ++end) {
        if (begin <= end) {
          testParallelFor(&inlineExecutor, begin, end, parallelism, GetParam());
        } else {
          EXPECT_THROW(
              testParallelFor(
                  &inlineExecutor, begin, end, parallelism, GetParam()),
              facebook::velox::VeloxRuntimeError);
        }
      }
    }
  }
}

TEST_P(ParallelForTest, E2EParallel) {
  for (size_t parallelism = 1; parallelism < 2; ++parallelism) {
    folly::CPUThreadPoolExecutor executor(parallelism);
    for (size_t begin = 0; begin < 25; ++begin) {
      for (size_t end = 0; end < 25; ++end) {
        if (begin <= end) {
          testParallelFor(&executor, begin, end, parallelism, GetParam());
        } else {
          EXPECT_THROW(
              testParallelFor(&executor, begin, end, parallelism, GetParam()),
              facebook::velox::VeloxRuntimeError);
        }
      }
    }
  }
}

TEST_P(ParallelForTest, NoWait) {
  bool wait = true;
  std::mutex mutex;
  std::condition_variable cv;
  size_t added = 0;
  size_t executed = 0;
  folly::CPUThreadPoolExecutor executor(2);
  ParallelFor pf(&executor, 0, 2, 2);
  switch (GetParam()) {
    case MODE::INDEX:
      pf.execute(
          [&](size_t /* unused */) {
            std::unique_lock lock(mutex);
            ++added;
            cv.notify_all();
            cv.wait(lock, [&] { return !wait; });
            ++executed;
            cv.notify_all();
          },
          false);
      break;
    case MODE::RANGE:
      pf.execute(
          [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
              std::unique_lock lock(mutex);
              ++added;
              cv.notify_all();
              cv.wait(lock, [&] { return !wait; });
              ++executed;
              cv.notify_all();
            }
          },
          false);
      break;
  }
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return added == 2; });
  }
  ASSERT_EQ(added, 2);
  // Parallel for didn't wait for tasks to finish
  ASSERT_EQ(executed, 0);
  {
    std::unique_lock lock(mutex);
    wait = false;
    cv.notify_all();
  }
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return executed == 2; });
  }
  EXPECT_EQ(executed, 2);
}

TEST_P(ParallelForTest, CanOwnExecutor) {
  auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(2);
  const size_t indexInvokedSize = 100;
  std::unordered_map<size_t, std::atomic<size_t>> indexInvoked;
  indexInvoked.reserve(indexInvokedSize);
  for (size_t i = 0; i < indexInvokedSize; ++i) {
    indexInvoked[i] = 0UL;
  }

  ParallelFor pf(executor, 0, indexInvokedSize, 9);
  switch (GetParam()) {
    case MODE::INDEX:
      pf.execute([&indexInvoked](size_t i) { ++indexInvoked[i]; });
      break;
    case MODE::RANGE:
      pf.execute([&indexInvoked](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
          ++indexInvoked[i];
        }
      });
      break;
  }

  EXPECT_EQ(indexInvoked.size(), indexInvokedSize);
  for (size_t i = 0; i < indexInvokedSize; ++i) {
    EXPECT_EQ(indexInvoked[i], 1);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ParallelForTestIndex,
    ParallelForTest,
    ValuesIn({MODE::INDEX}));

INSTANTIATE_TEST_SUITE_P(
    ParallelForTestRange,
    ParallelForTest,
    ValuesIn({MODE::RANGE}));
