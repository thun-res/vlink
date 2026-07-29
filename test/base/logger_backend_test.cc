/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
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

// NOLINTBEGIN

#include "./base/logger_backend.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "./base/utils.h"

namespace {

std::filesystem::path backend_test_dir(const std::string& name) {
  auto path = std::filesystem::path(vlink::Utils::get_tmp_dir()) / "vlink-logger-backend-tests" / name;
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path;
}

vlink::LoggerBackend::Config backend_config(const std::filesystem::path& path) {
  vlink::LoggerBackend::Config config;
  config.app_name = "logger_backend_test";
  config.log_path = path.string();
  config.max_file_size = 1024U * 1024U;
  config.max_files = 2U;
  config.queue_size = 32U;
  config.flush_interval_ms = 0U;
  config.fixed_filename = true;
  config.append = false;
  config.block_when_full = true;
  config.use_utc = true;
  return config;
}

std::string read_backend_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

size_t count_backend_records(std::string_view content, std::string_view marker) {
  size_t count = 0U;
  size_t offset = 0U;

  while ((offset = content.find(marker, offset)) != std::string_view::npos) {
    ++count;
    offset += marker.size();
  }

  return count;
}

}  // namespace

TEST_SUITE("base-LoggerBackend") {
  TEST_CASE("backend exposes the configured MessageLoop dispatcher capacity") {
    static_assert(std::is_base_of_v<vlink::MessageLoop, vlink::LoggerBackend>);

    auto config = backend_config(backend_test_dir("message-loop"));
    config.queue_size = 17U;
    config.block_when_full = false;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    CHECK_EQ(backend.get_max_task_count(), 17U);
    CHECK(backend.is_running());
  }

  TEST_CASE("constructor rejects invalid timestamp rotation limits") {
    auto config = backend_config(backend_test_dir("invalid-size"));
    config.max_file_size = 0U;
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::invalid_argument);

    config = backend_config(backend_test_dir("invalid-count"));
    config.fixed_filename = false;
    config.max_files = 0U;
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::invalid_argument);

    config = backend_config(backend_test_dir("invalid-fixed-count-limit"));
    config.max_files = std::numeric_limits<size_t>::max();
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::invalid_argument);

    config = backend_config(backend_test_dir("invalid-timestamp-count-limit"));
    config.fixed_filename = false;
    config.max_files = std::numeric_limits<size_t>::max();
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::invalid_argument);
  }

  TEST_CASE("zero dispatcher capacity is clamped to one") {
    auto config = backend_config(backend_test_dir("zero-queue-size"));
    config.queue_size = 0U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    CHECK_EQ(backend.get_max_task_count(), 1U);
  }

  TEST_CASE("fixed file output contains timestamp thread level and message") {
    const auto directory = backend_test_dir("fixed-output");
    const auto file = directory / "logger_backend_test.log";
    vlink::LoggerBackend backend(backend_config(directory), nullptr);

    CHECK(backend.log(vlink::Logger::kInfo, "direct backend message"));
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find(" UTC @") != std::string::npos);
    CHECK(content.find(" - INFO  - direct backend message\n") != std::string::npos);
    CHECK_FALSE(backend.has_error());
    CHECK_FALSE(backend.log(vlink::Logger::kOff, "off record"));
  }

  TEST_CASE("fixed append preserves an existing file") {
    const auto directory = backend_test_dir("fixed-append");
    const auto file = directory / "logger_backend_test.log";
    {
      std::ofstream stream(file, std::ios::binary);
      stream << "existing fixed record\n";
    }

    auto config = backend_config(directory);
    config.append = true;
    vlink::LoggerBackend backend(std::move(config), nullptr);
    REQUIRE(backend.log(vlink::Logger::kInfo, "appended fixed record"));
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find("existing fixed record") != std::string::npos);
    CHECK(content.find("appended fixed record") != std::string::npos);
  }

  TEST_CASE("fixed non-append startup rotates an existing active file") {
    const auto directory = backend_test_dir("fixed-non-append");
    const auto file = directory / "logger_backend_test.log";
    const auto backup = directory / "logger_backend_test.1.log";
    {
      std::ofstream stream(file, std::ios::binary);
      stream << "existing fixed record\n";
    }

    vlink::LoggerBackend backend(backend_config(directory), nullptr);
    REQUIRE(backend.log(vlink::Logger::kInfo, "new fixed record"));
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find("new fixed record") != std::string::npos);
    CHECK(content.find("existing fixed record") == std::string::npos);
    CHECK(read_backend_file(backup).find("existing fixed record") != std::string::npos);
  }

  TEST_CASE("fixed rotation keeps an exact size boundary in the active file") {
    const auto directory = backend_test_dir("fixed-exact-boundary");
    const auto file = directory / "logger_backend_test.log";
    const auto backup = directory / "logger_backend_test.1.log";
    constexpr std::string_view kMessage = "exact boundary record";

    {
      vlink::LoggerBackend backend(backend_config(directory), nullptr);
      REQUIRE(backend.log(vlink::Logger::kInfo, kMessage));
      backend.flush();
    }

    const auto record_size = std::filesystem::file_size(file);
    auto config = backend_config(directory);
    config.append = true;
    config.max_file_size = static_cast<size_t>(record_size * 2U);
    vlink::LoggerBackend backend(std::move(config), nullptr);

    REQUIRE(backend.log(vlink::Logger::kInfo, kMessage));
    backend.flush();
    CHECK_EQ(std::filesystem::file_size(file), record_size * 2U);
    CHECK_FALSE(std::filesystem::exists(backup));

    REQUIRE(backend.log(vlink::Logger::kInfo, kMessage));
    backend.flush();
    CHECK_EQ(std::filesystem::file_size(file), record_size);
    CHECK_EQ(std::filesystem::file_size(backup), record_size * 2U);
  }

  TEST_CASE("fixed rotation retains multiple generations in order") {
    const auto directory = backend_test_dir("fixed-generations");
    const auto file = directory / "logger_backend_test.log";
    const auto first_backup = directory / "logger_backend_test.1.log";
    const auto second_backup = directory / "logger_backend_test.2.log";
    auto config = backend_config(directory);
    config.max_file_size = 128U;
    config.max_files = 2U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    REQUIRE(backend.log(vlink::Logger::kInfo, "first generation " + std::string(256U, 'a')));
    REQUIRE(backend.log(vlink::Logger::kInfo, "second generation " + std::string(256U, 'b')));
    REQUIRE(backend.log(vlink::Logger::kInfo, "third generation " + std::string(256U, 'c')));
    backend.flush();

    CHECK(read_backend_file(file).find("third generation") != std::string::npos);
    CHECK(read_backend_file(first_backup).find("second generation") != std::string::npos);
    CHECK(read_backend_file(second_backup).find("first generation") != std::string::npos);
  }

  TEST_CASE("periodic timer flushes an idle file") {
    const auto directory = backend_test_dir("periodic-flush");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.flush_interval_ms = 20U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    CHECK(backend.log(vlink::Logger::kInfo, "periodic flush message"));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (read_backend_file(file).find("periodic flush message") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }

    CHECK(read_backend_file(file).find("periodic flush message") != std::string::npos);
  }

  TEST_CASE("error record flushes immediately with a nonzero interval") {
    const auto directory = backend_test_dir("error-immediate-flush");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.flush_interval_ms = 5000U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    REQUIRE(backend.log(vlink::Logger::kError, "immediately flushed error"));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (read_backend_file(file).find("immediately flushed error") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }

    CHECK(read_backend_file(file).find("immediately flushed error") != std::string::npos);
  }

  TEST_CASE("backtrace keeps the newest records and dumps formatted lines") {
    const auto directory = backend_test_dir("backtrace");
    vlink::LoggerBackend backend(backend_config(directory), nullptr);
    std::vector<std::pair<vlink::Logger::Level, std::string>> console_records;
    vlink::LoggerBackend::ConsoleWriter console_writer = [&console_records](vlink::Logger::Level level,
                                                                            std::string_view line) {
      console_records.emplace_back(level, line);
    };

    backend.enable_backtrace(2U);
    CHECK(backend.log(vlink::Logger::kTrace, "evicted backtrace record"));
    CHECK(backend.log(vlink::Logger::kInfo, "retained backtrace info"));
    CHECK(backend.log(vlink::Logger::kWarn, "retained backtrace warning"));
    backend.dump_backtrace(console_writer);
    backend.dump_backtrace(console_writer);
    backend.disable_backtrace();

    REQUIRE_EQ(console_records.size(), 4U);
    CHECK(console_records[0].second.find("Backtrace Start") != std::string::npos);
    CHECK_EQ(console_records[1].first, vlink::Logger::kInfo);
    CHECK(console_records[1].second.find("retained backtrace info") != std::string::npos);
    CHECK_EQ(console_records[2].first, vlink::Logger::kWarn);
    CHECK(console_records[2].second.find("retained backtrace warning") != std::string::npos);
    CHECK(console_records[3].second.find("Backtrace End") != std::string::npos);
  }

  TEST_CASE("zero capacity backtrace suppresses low levels without retaining records") {
    const auto directory = backend_test_dir("zero-backtrace");
    const auto file = directory / "logger_backend_test.log";
    vlink::LoggerBackend backend(backend_config(directory), nullptr);
    size_t console_records = 0U;
    vlink::LoggerBackend::ConsoleWriter console_writer = [&console_records](vlink::Logger::Level, std::string_view) {
      ++console_records;
    };

    backend.enable_backtrace(0U);
    CHECK(backend.log(vlink::Logger::kTrace, "zero capacity trace"));
    CHECK(backend.log(vlink::Logger::kWarn, "zero capacity warning"));
    backend.dump_backtrace(console_writer);
    backend.disable_backtrace();
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find("zero capacity trace") == std::string::npos);
    CHECK(content.find("zero capacity warning") != std::string::npos);
    CHECK_EQ(console_records, 0U);
  }

  TEST_CASE("backtrace emits warning records immediately to its console writer") {
    const auto directory = backend_test_dir("backtrace-console");
    std::vector<std::pair<vlink::Logger::Level, std::string>> console_records;
    vlink::LoggerBackend backend(backend_config(directory), nullptr,
                                 [&console_records](vlink::Logger::Level level, std::string_view line) {
                                   console_records.emplace_back(level, line);
                                 });

    backend.enable_backtrace(2U);
    REQUIRE(backend.log(vlink::Logger::kInfo, "retained without immediate console"));
    REQUIRE(backend.log(vlink::Logger::kWarn, "immediate backtrace warning"));
    backend.flush();

    REQUIRE_EQ(console_records.size(), 1U);
    CHECK_EQ(console_records.front().first, vlink::Logger::kWarn);
    CHECK(console_records.front().second.find("immediate backtrace warning") != std::string::npos);
  }

  TEST_CASE("persistent console writer exceptions enter the permanent error state") {
    auto config = backend_config(backend_test_dir("backtrace-console-error"));
    vlink::LoggerBackend backend(std::move(config), nullptr, [](vlink::Logger::Level, std::string_view) { throw 1; });

    backend.enable_backtrace(1U);
    REQUIRE(backend.log(vlink::Logger::kWarn, "throwing persistent console writer"));
    backend.flush();

    CHECK(backend.has_error());
  }

  TEST_CASE("dump console writer exceptions enter the permanent error state") {
    auto config = backend_config(backend_test_dir("backtrace-dump-console-error"));
    vlink::LoggerBackend backend(std::move(config), nullptr);

    backend.enable_backtrace(1U);
    REQUIRE(backend.log(vlink::Logger::kInfo, "throwing dump console writer"));
    backend.dump_backtrace([](vlink::Logger::Level, std::string_view) { throw 1; });

    CHECK(backend.has_error());
  }

  TEST_CASE("depth one blocking queue drains every accepted record before flush") {
    const auto directory = backend_test_dir("depth-one");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.queue_size = 1U;
    config.flush_interval_ms = 1U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    for (int index = 0; index < 500; ++index) {
      REQUIRE(backend.log(vlink::Logger::kInfo, "blocking backend record " + std::to_string(index)));
    }

    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find("blocking backend record 0") != std::string::npos);
    CHECK(content.find("blocking backend record 499") != std::string::npos);
    CHECK_EQ(static_cast<size_t>(std::count(content.begin(), content.end(), '\n')), 500U);
  }

  TEST_CASE("multiple blocking producers drain every accepted record") {
    const auto directory = backend_test_dir("multiple-producers");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.queue_size = 1U;
    config.flush_interval_ms = 1U;
    vlink::LoggerBackend backend(std::move(config), nullptr);
    std::vector<std::thread> producers;
    std::atomic_bool all_accepted{true};

    for (int thread_index = 0; thread_index < 4; ++thread_index) {
      producers.emplace_back([thread_index, &backend, &all_accepted] {
        for (int record_index = 0; record_index < 250; ++record_index) {
          if (!backend.log(vlink::Logger::kInfo, "multi producer record " + std::to_string(thread_index) + "/" +
                                                     std::to_string(record_index))) {
            all_accepted.store(false, std::memory_order_relaxed);
          }
        }
      });
    }

    for (auto& producer : producers) {
      producer.join();
    }

    backend.flush();

    CHECK(all_accepted.load(std::memory_order_relaxed));
    CHECK_EQ(count_backend_records(read_backend_file(file), "multi producer record "), 1000U);
  }

  TEST_CASE("destructor drains accepted records without an explicit flush") {
    const auto directory = backend_test_dir("destructor-drain");
    const auto file = directory / "logger_backend_test.log";

    {
      auto config = backend_config(directory);
      config.flush_interval_ms = 5000U;
      vlink::LoggerBackend backend(std::move(config), nullptr);

      for (int index = 0; index < 500; ++index) {
        REQUIRE(backend.log(vlink::Logger::kInfo, "destructor drain record " + std::to_string(index)));
      }
    }

    CHECK_EQ(count_backend_records(read_backend_file(file), "destructor drain record "), 500U);
  }

  TEST_CASE("concurrent producers and flush barriers preserve blocking records") {
    const auto directory = backend_test_dir("concurrent-flush");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.max_file_size = 16U * 1024U * 1024U;
    config.queue_size = 32U;
    config.flush_interval_ms = 1000U;
    vlink::LoggerBackend backend(std::move(config), nullptr);
    std::vector<std::thread> producers;
    std::atomic_size_t completed_producers{0U};
    std::atomic_bool all_accepted{true};

    for (int thread_index = 0; thread_index < 8; ++thread_index) {
      producers.emplace_back([thread_index, &backend, &completed_producers, &all_accepted] {
        for (int record_index = 0; record_index < 1000; ++record_index) {
          if (!backend.log(vlink::Logger::kInfo, "concurrent flush record " + std::to_string(thread_index) + "/" +
                                                     std::to_string(record_index))) {
            all_accepted.store(false, std::memory_order_relaxed);
          }
        }

        completed_producers.fetch_add(1U, std::memory_order_release);
      });
    }

    std::thread flusher([&backend, &completed_producers] {
      while (completed_producers.load(std::memory_order_acquire) < 8U) {
        backend.flush();
        std::this_thread::yield();
      }
    });

    for (auto& producer : producers) {
      producer.join();
    }

    flusher.join();
    backend.flush();

    CHECK(all_accepted.load(std::memory_order_relaxed));
    CHECK_EQ(count_backend_records(read_backend_file(file), "concurrent flush record "), 8000U);
  }

  TEST_CASE("nonblocking full queue keeps accepting while dropping old records") {
    const auto directory = backend_test_dir("nonblocking-full");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.queue_size = 1U;
    config.flush_interval_ms = 1000U;
    config.block_when_full = false;
    std::atomic_bool worker_paused{false};
    std::atomic_bool release_worker{false};
    vlink::LoggerBackend backend(std::move(config), nullptr);
    REQUIRE(backend.post_task([&worker_paused, &release_worker] {
      worker_paused.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    const bool task_did_start =
        common_test::wait_until([&worker_paused] { return worker_paused.load(std::memory_order_acquire); });

    if (!task_did_start) {
      release_worker.store(true, std::memory_order_release);
      (void)backend.quit(true);
      CHECK(backend.wait_for_quit());
    }

    REQUIRE(task_did_start);

    for (int index = 0; index < 32; ++index) {
      REQUIRE(backend.log(vlink::Logger::kInfo, "nonblocking record " + std::to_string(index)));
    }

    CHECK_EQ(backend.get_task_count(), 1U);
    release_worker.store(true, std::memory_order_release);
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK_EQ(count_backend_records(content, "nonblocking record "), 1U);
    CHECK(content.find("nonblocking record 31") != std::string::npos);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("nonblocking full queue preserves error records") {
    const auto directory = backend_test_dir("nonblocking-errors");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.queue_size = 1U;
    config.flush_interval_ms = 1000U;
    config.block_when_full = false;
    std::atomic_bool worker_paused{false};
    std::atomic_bool release_worker{false};
    vlink::LoggerBackend backend(std::move(config), nullptr);
    REQUIRE(backend.post_task([&worker_paused, &release_worker] {
      worker_paused.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    const bool task_did_start =
        common_test::wait_until([&worker_paused] { return worker_paused.load(std::memory_order_acquire); });

    if (!task_did_start) {
      release_worker.store(true, std::memory_order_release);
      (void)backend.quit(true);
      CHECK(backend.wait_for_quit());
    }

    REQUIRE(task_did_start);
    REQUIRE(backend.log(vlink::Logger::kError, "protected error record 0"));

    std::atomic_bool producer_started{false};
    std::atomic_bool producer_returned{false};
    bool accepted = false;
    std::thread producer([&] {
      producer_started.store(true, std::memory_order_release);
      accepted = backend.log(vlink::Logger::kError, "protected error record 1");
      producer_returned.store(true, std::memory_order_release);
    });

    REQUIRE(common_test::wait_until([&producer_started] { return producer_started.load(std::memory_order_acquire); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(producer_returned.load(std::memory_order_acquire));

    release_worker.store(true, std::memory_order_release);
    producer.join();
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(accepted);
    CHECK_EQ(count_backend_records(content, "protected error record "), 2U);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("mixed severity overload preserves protected records from multiple producers") {
    const auto directory = backend_test_dir("mixed-severity-overload");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.max_file_size = 16U * 1024U * 1024U;
    config.queue_size = 64U;
    config.flush_interval_ms = 1000U;
    config.block_when_full = false;
    std::atomic_bool worker_paused{false};
    std::atomic_bool release_worker{false};
    vlink::LoggerBackend backend(std::move(config), nullptr);
    REQUIRE(backend.post_task([&worker_paused, &release_worker] {
      worker_paused.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!worker_paused.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < pause_deadline) {
      std::this_thread::yield();
    }

    if (!worker_paused.load(std::memory_order_acquire)) {
      release_worker.store(true, std::memory_order_release);
      (void)backend.quit(true);
      CHECK(backend.wait_for_quit());
    }

    REQUIRE(worker_paused.load(std::memory_order_acquire));

    std::vector<std::thread> producers;
    std::atomic_bool protected_accepted{true};

    for (int thread_index = 0; thread_index < 8; ++thread_index) {
      producers.emplace_back([thread_index, &backend, &protected_accepted] {
        for (int record_index = 0; record_index < 1000; ++record_index) {
          if (record_index % 200 == 0) {
            if (!backend.log(vlink::Logger::kError, "mixed protected record " + std::to_string(thread_index) + "/" +
                                                        std::to_string(record_index))) {
              protected_accepted.store(false, std::memory_order_relaxed);
            }
          } else {
            (void)backend.log(vlink::Logger::kInfo, "mixed droppable record");
          }
        }
      });
    }

    for (auto& producer : producers) {
      producer.join();
    }

    CHECK_EQ(backend.get_task_count(), 64U);
    release_worker.store(true, std::memory_order_release);
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(protected_accepted.load(std::memory_order_relaxed));
    CHECK_EQ(count_backend_records(content, "mixed protected record "), 40U);
    CHECK(count_backend_records(content, "mixed droppable record") < 7960U);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("record allocation preserves messages across pool and system size classes") {
    const auto directory = backend_test_dir("allocation-sizes");
    const auto file = directory / "logger_backend_test.log";
    vlink::LoggerBackend backend(backend_config(directory), nullptr);
    const std::string small_message = "small-" + std::string(300U, 'a');
    const std::string medium_message = "medium-" + std::string(600U, 'b');
    const std::string large_message = "large-" + std::string(4096U, 'c');
    const std::string binary_message("binary\0payload", 14U);

    REQUIRE(backend.log(vlink::Logger::kInfo, small_message));
    REQUIRE(backend.log(vlink::Logger::kInfo, medium_message));
    REQUIRE(backend.log(vlink::Logger::kInfo, large_message));
    REQUIRE(backend.log(vlink::Logger::kInfo, binary_message));
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find(small_message) != std::string::npos);
    CHECK(content.find(medium_message) != std::string::npos);
    CHECK(content.find(large_message) != std::string::npos);
    CHECK(content.find(binary_message) != std::string::npos);
    CHECK_EQ(static_cast<size_t>(std::count(content.begin(), content.end(), '\n')), 4U);
  }

  TEST_CASE("timestamp rotation prunes old files and retains the newest record") {
    const auto directory = backend_test_dir("timestamp-rotation");
    auto config = backend_config(directory);
    config.fixed_filename = false;
    config.max_file_size = 256U;
    config.max_files = 2U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    for (int index = 0; index < 4; ++index) {
      REQUIRE(backend.log(vlink::Logger::kInfo,
                          "timestamp rotation record " + std::to_string(index) + " " + std::string(300U, 'x')));
    }

    backend.flush();

    size_t file_count = 0U;
    bool found_newest = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".log") {
        ++file_count;
        found_newest =
            found_newest || read_backend_file(entry.path()).find("timestamp rotation record 3") != std::string::npos;
      }
    }

    CHECK_EQ(file_count, 2U);
    CHECK(found_newest);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("timestamp fallback directory preserves files across restarts") {
    const auto directory = backend_test_dir("timestamp-fallback-restart");
    const auto occupied_path = directory / "occupied-path";
    const auto fallback_directory = std::filesystem::path(occupied_path.string() + "_dir");
    {
      std::ofstream stream(occupied_path, std::ios::binary);
      stream << "occupied";
    }

    auto first_config = backend_config(occupied_path);
    first_config.fixed_filename = false;
    {
      vlink::LoggerBackend backend(std::move(first_config), nullptr);
      REQUIRE(backend.log(vlink::Logger::kInfo, "first fallback record"));
      backend.flush();
    }

    auto second_config = backend_config(occupied_path);
    second_config.fixed_filename = false;
    {
      vlink::LoggerBackend backend(std::move(second_config), nullptr);
      REQUIRE(backend.log(vlink::Logger::kInfo, "second fallback record"));
      backend.flush();
    }

    size_t file_count = 0U;
    bool found_first = false;
    bool found_second = false;

    for (const auto& entry : std::filesystem::directory_iterator(fallback_directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".log") {
        ++file_count;
        const auto content = read_backend_file(entry.path());
        found_first = found_first || content.find("first fallback record") != std::string::npos;
        found_second = found_second || content.find("second fallback record") != std::string::npos;
      }
    }

    CHECK_EQ(file_count, 2U);
    CHECK(found_first);
    CHECK(found_second);
  }

  TEST_CASE("timestamp append continues the newest file after restart") {
    const auto directory = backend_test_dir("timestamp-append-restart");

    auto first_config = backend_config(directory);
    first_config.fixed_filename = false;
    {
      vlink::LoggerBackend backend(std::move(first_config), nullptr);
      REQUIRE(backend.log(vlink::Logger::kInfo, "timestamp append first record"));
      backend.flush();
    }

    auto second_config = backend_config(directory);
    second_config.fixed_filename = false;
    second_config.append = true;
    {
      vlink::LoggerBackend backend(std::move(second_config), nullptr);
      REQUIRE(backend.log(vlink::Logger::kInfo, "timestamp append second record"));
      backend.flush();
    }

    size_t file_count = 0U;
    std::string content;

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".log") {
        ++file_count;
        content = read_backend_file(entry.path());
      }
    }

    CHECK_EQ(file_count, 1U);
    CHECK(content.find("timestamp append first record") != std::string::npos);
    CHECK(content.find("timestamp append second record") != std::string::npos);
  }

  TEST_CASE("timestamp append prunes files above a reduced retention limit") {
    const auto directory = backend_test_dir("timestamp-append-prune");

    for (size_t index = 1U; index <= 4U; ++index) {
      std::ofstream stream(directory / ("2026-01-01_00-00-00." + std::to_string(index) + ".log"), std::ios::binary);
      stream << "timestamp file " << index << '\n';
    }

    auto config = backend_config(directory);
    config.fixed_filename = false;
    config.max_files = 2U;
    config.append = true;
    {
      vlink::LoggerBackend backend(std::move(config), nullptr);
      backend.flush();
    }

    size_t file_count = 0U;
    bool found_newest = false;

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".log") {
        ++file_count;
        found_newest = found_newest || read_backend_file(entry.path()).find("timestamp file 4") != std::string::npos;
      }
    }

    CHECK_EQ(file_count, 2U);
    CHECK(found_newest);
  }

  TEST_CASE("constructor reports a log path that cannot contain files") {
    const auto directory = backend_test_dir("invalid-log-path");
    const auto regular_file = directory / "regular-file";
    {
      std::ofstream stream(regular_file, std::ios::binary);
      stream << "not a directory";
    }

    auto config = backend_config(regular_file);
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::filesystem::filesystem_error);
  }

  TEST_CASE("fixed rotation failure preserves the active file") {
    const auto directory = backend_test_dir("fixed-rotation-failure");
    const auto file = directory / "logger_backend_test.log";
    const auto backup = directory / "logger_backend_test.1.log";
    auto config = backend_config(directory);
    config.max_file_size = 256U;
    config.max_files = 1U;
    std::atomic_size_t errors{0U};
    vlink::LoggerBackend backend(std::move(config),
                                 [&errors](std::string_view) { errors.fetch_add(1U, std::memory_order_relaxed); });

    REQUIRE(backend.log(vlink::Logger::kInfo, "record preserved before failed rotation"));
    backend.flush();
    std::filesystem::create_directories(backup / "blocker");
    REQUIRE(backend.log(vlink::Logger::kInfo, std::string(512U, 'x')));
    backend.flush();

    CHECK(backend.has_error());
    CHECK_EQ(errors.load(std::memory_order_relaxed), 1U);
    CHECK(read_backend_file(file).find("record preserved before failed rotation") != std::string::npos);
  }

#ifndef _WIN32
  TEST_CASE("external truncate does not create a sparse gap") {
    const auto directory = backend_test_dir("external-truncate");
    const auto file = directory / "logger_backend_test.log";
    vlink::LoggerBackend backend(backend_config(directory), nullptr);

    REQUIRE(backend.log(vlink::Logger::kInfo, "record before external truncate"));
    backend.flush();
    std::filesystem::resize_file(file, 0U);
    REQUIRE(backend.log(vlink::Logger::kInfo, "record after external truncate"));
    backend.flush();

    const auto content = read_backend_file(file);
    CHECK(content.find("record after external truncate") != std::string::npos);
    CHECK(content.find('\0') == std::string::npos);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("rotation recreates an externally removed parent directory") {
    const auto directory = backend_test_dir("removed-parent");
    const auto file = directory / "logger_backend_test.log";
    auto config = backend_config(directory);
    config.max_file_size = 128U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    REQUIRE(backend.log(vlink::Logger::kInfo, "record before parent removal " + std::string(256U, 'a')));
    backend.flush();
    REQUIRE(std::filesystem::remove(file));
    REQUIRE(std::filesystem::remove(directory));

    REQUIRE(backend.log(vlink::Logger::kInfo, "record after parent removal " + std::string(256U, 'b')));
    backend.flush();

    CHECK(std::filesystem::exists(directory));
    CHECK(read_backend_file(file).find("record after parent removal") != std::string::npos);
    CHECK_FALSE(backend.has_error());
  }

  TEST_CASE("rotation reports a runtime directory permission failure once") {
    const auto directory = backend_test_dir("runtime-permission");
    auto config = backend_config(directory);
    config.max_file_size = 128U;
    std::atomic_size_t errors{0U};
    vlink::LoggerBackend backend(std::move(config),
                                 [&errors](std::string_view) { errors.fetch_add(1U, std::memory_order_relaxed); });

    REQUIRE(backend.log(vlink::Logger::kInfo, "record before permission change " + std::string(256U, 'a')));
    backend.flush();

    std::error_code permission_error;
    std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, permission_error);
    REQUIRE_FALSE(permission_error);

    std::ofstream probe(directory / "permission-probe", std::ios::binary);
    const bool permission_enforced = !probe.is_open();
    probe.close();

    if (permission_enforced) {
      REQUIRE(backend.log(vlink::Logger::kInfo, "record after permission change " + std::string(256U, 'b')));
      backend.flush();
      CHECK(backend.has_error());
      CHECK_EQ(errors.load(std::memory_order_relaxed), 1U);
    }

    std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 permission_error);
    CHECK_FALSE(permission_error);
  }

#if defined(__linux__)
  TEST_CASE("write failure notifies once and isolates a throwing error handler") {
    const auto directory = backend_test_dir("write-failure");
    const auto file = directory / "logger_backend_test.log";
    std::filesystem::create_symlink("/dev/full", file);
    auto config = backend_config(directory);
    config.append = true;
    std::atomic_size_t errors{0U};
    vlink::LoggerBackend backend(std::move(config), [&errors](std::string_view) {
      errors.fetch_add(1U, std::memory_order_relaxed);
      throw std::runtime_error("test error handler failure");
    });

    REQUIRE(backend.log(vlink::Logger::kInfo, "record rejected by full device"));
    backend.flush();

    CHECK(backend.has_error());
    CHECK_EQ(errors.load(std::memory_order_relaxed), 1U);
    CHECK_FALSE(backend.log(vlink::Logger::kInfo, "record after permanent error"));
  }
#endif

  TEST_CASE("timestamp prune failure is retried without stopping the active file") {
    const auto directory = backend_test_dir("timestamp-prune-failure");
    auto config = backend_config(directory);
    config.fixed_filename = false;
    config.max_file_size = 256U;
    config.max_files = 1U;
    vlink::LoggerBackend backend(std::move(config), nullptr);

    REQUIRE(backend.log(vlink::Logger::kInfo, "record before timestamp prune failure"));
    backend.flush();
    const auto files = [&directory] {
      std::vector<std::filesystem::path> result;
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
          result.emplace_back(entry.path());
        }
      }

      return result;
    }();
    REQUIRE_EQ(files.size(), 1U);
    std::filesystem::remove(files.front());
    std::filesystem::create_directories(files.front() / "blocker");

    REQUIRE(backend.log(vlink::Logger::kInfo, std::string(512U, 'x')));
    backend.flush();
    REQUIRE(backend.log(vlink::Logger::kInfo, "record after timestamp prune failure"));
    backend.flush();

    CHECK_FALSE(backend.has_error());
    bool found_record = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() &&
          read_backend_file(entry.path()).find("record after timestamp prune failure") != std::string::npos) {
        found_record = true;
      }
    }

    CHECK(found_record);

    REQUIRE(std::filesystem::remove(files.front() / "blocker"));
    REQUIRE(std::filesystem::remove(files.front()));
    {
      std::ofstream sentinel(files.front(), std::ios::binary);
      REQUIRE(sentinel.is_open());
      sentinel << "stale timestamp record\n";
    }

    REQUIRE(backend.log(vlink::Logger::kInfo, std::string(512U, 'y')));
    backend.flush();

    CHECK_FALSE(std::filesystem::exists(files.front()));
    size_t log_file_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".log") {
        ++log_file_count;
      }
    }

    CHECK_EQ(log_file_count, 1U);
  }
#endif

  TEST_CASE("timestamp rotation keeps a monotonic index after wall clock rollback") {
    const auto directory = backend_test_dir("timestamp-clock-rollback");
    {
      std::ofstream stream(directory / "2099-12-31_23-59-59.9.log", std::ios::binary);
      stream << "future timestamp record\n";
    }

    auto config = backend_config(directory);
    config.fixed_filename = false;
    config.max_files = 2U;
    {
      vlink::LoggerBackend backend(std::move(config), nullptr);
      backend.flush();
    }

    bool found_next_index = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().filename().string().find(".10.log") != std::string::npos) {
        found_next_index = true;
      }
    }

    CHECK(found_next_index);
  }

  TEST_CASE("timestamp rotation rejects an exhausted file index") {
    const auto directory = backend_test_dir("timestamp-index-overflow");
    const auto filename = "2099-12-31_23-59-59." + std::to_string(std::numeric_limits<size_t>::max()) + ".log";
    {
      std::ofstream stream(directory / filename, std::ios::binary);
      stream << "maximum index record\n";
    }

    auto config = backend_config(directory);
    config.fixed_filename = false;
    CHECK_THROWS_AS(vlink::LoggerBackend(std::move(config), nullptr), std::runtime_error);
  }

  TEST_CASE("flush remains valid after the inherited loop is stopped") {
    const auto directory = backend_test_dir("stopped-loop");
    const auto file = directory / "logger_backend_test.log";
    vlink::LoggerBackend backend(backend_config(directory), nullptr);

    CHECK(backend.log(vlink::Logger::kInfo, "record before inherited quit"));
    backend.flush();
    CHECK(backend.quit());
    CHECK(backend.wait_for_quit());
    CHECK_FALSE(backend.log(vlink::Logger::kInfo, "record after inherited quit"));
    backend.flush();

    CHECK(read_backend_file(file).find("record before inherited quit") != std::string::npos);
  }

  TEST_CASE("force quit releases a queued flush barrier") {
    const auto directory = backend_test_dir("force-quit-barrier");
    vlink::LoggerBackend backend(backend_config(directory), nullptr);
    std::atomic_bool task_started{false};
    std::atomic_bool release_task{false};
    REQUIRE(backend.post_task([&task_started, &release_task] {
      task_started.store(true, std::memory_order_release);

      while (!release_task.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    const auto task_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!task_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < task_deadline) {
      std::this_thread::yield();
    }

    const bool task_did_start = task_started.load(std::memory_order_acquire);

    if (!task_did_start) {
      release_task.store(true, std::memory_order_release);
      (void)backend.quit(true);
      CHECK(backend.wait_for_quit());
    }

    REQUIRE(task_did_start);

    std::thread flusher([&backend] { backend.flush(); });
    const auto barrier_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (backend.get_task_count() == 0U && std::chrono::steady_clock::now() < barrier_deadline) {
      std::this_thread::yield();
    }

    CHECK(backend.get_task_count() > 0U);
    CHECK(backend.quit(true));
    release_task.store(true, std::memory_order_release);
    flusher.join();
    CHECK(backend.wait_for_quit());
  }
}

// NOLINTEND
