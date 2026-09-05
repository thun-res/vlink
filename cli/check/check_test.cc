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

#include "./check_common.h"

enum class TestOutcome : uint8_t {
  kPassed = 0,
  kWarning = 1,
  kFailed = 2,
};

std::string g_test_current_title;  // NOLINT(runtime/string)

void print_test_row(const std::string& label) {
  std::string title = "* Test " + label + "...";

  if (title.size() < static_cast<size_t>(kTitleWidth)) {
    title.append(kTitleWidth - title.size(), ' ');
  }

  g_test_current_title = title;

  std::cout << title << "......";
  std::cout.flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

void print_outcome(TestOutcome outcome, const std::string& detail) {
  const int detail_len = static_cast<int>(detail.size());

  std::cout << "\033[2K\r";

  switch (outcome) {
    case TestOutcome::kPassed:
      std::cout << kColorPass << g_test_current_title << "PASSED";
      std::cout << std::string(std::max(kStatusPassPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    case TestOutcome::kWarning:
      std::cout << kColorWarn << g_test_current_title << "WARNING";
      std::cout << std::string(std::max(kStatusWarnPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    case TestOutcome::kFailed:
      std::cout << kColorFail << g_test_current_title << "FAILED";
      std::cout << std::string(std::max(kStatusFailPad - detail_len, 2), ' ');
      std::cout << detail << kColorReset << std::endl;
      break;
    default:
      break;
  }
}

std::pair<TestOutcome, std::string> run_event_roundtrip(const std::string& url, int timeout_ms, int message_count,
                                                        bool external_allowed) {
  std::atomic<int> received_count{0};
  std::atomic<bool> mismatch{false};

  try {
    vlink::Subscriber<std::string> sub(url);
    sub.listen([&received_count, &mismatch, message_count](const std::string& payload) {
      const int idx = received_count.load();

      if VUNLIKELY (idx >= message_count) {
        return;
      }

      const auto expected = std::string("payload-") + std::to_string(idx);

      if VUNLIKELY (payload != expected) {
        mismatch = true;
      }

      ++received_count;
    });

    vlink::Publisher<std::string> pub(url);

    if VUNLIKELY (!pub.wait_for_subscribers(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "no subscriber within " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "no subscriber within " + std::to_string(timeout_ms) + "ms"};
    }

    for (int i = 0; i < message_count; ++i) {
      pub.publish(std::string("payload-") + std::to_string(i));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (received_count < message_count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }

  if VUNLIKELY (mismatch) {
    return {TestOutcome::kFailed, "payload mismatch"};
  }

  if VUNLIKELY (received_count < message_count) {
    if (external_allowed && received_count == 0) {
      return {TestOutcome::kWarning,
              "delivered " + std::to_string(received_count) + "/" + std::to_string(message_count)};
    }

    return {TestOutcome::kFailed, "delivered " + std::to_string(received_count) + "/" + std::to_string(message_count)};
  }

  return {TestOutcome::kPassed, std::to_string(received_count) + "/" + std::to_string(message_count) + " messages"};
}

std::pair<TestOutcome, std::string> run_method_roundtrip(const std::string& url, int timeout_ms,
                                                         bool external_allowed) {
  try {
    vlink::Server<std::string, std::string> server(url);
    server.listen([](const std::string& req, std::string& resp) {
      resp = "echo:" + req;
      return true;
    });

    vlink::Client<std::string, std::string> client(url);

    if VUNLIKELY (!client.wait_for_connected(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "server not reachable within " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "server not reachable within " + std::to_string(timeout_ms) + "ms"};
    }

    std::string resp;

    if VUNLIKELY (!client.invoke(std::string("ping"), resp, std::chrono::milliseconds(timeout_ms))) {
      return {TestOutcome::kFailed, "invoke timed out"};
    }

    if VUNLIKELY (resp != "echo:ping") {
      return {TestOutcome::kFailed, "unexpected response '" + resp + "'"};
    }

    return {TestOutcome::kPassed, "1/1 calls"};
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }
}

std::pair<TestOutcome, std::string> run_field_roundtrip(const std::string& url, int timeout_ms, bool external_allowed) {
  try {
    vlink::Getter<int> getter(url);
    vlink::Setter<int> setter(url);
    setter.set(42);

    if VUNLIKELY (!getter.wait_for_value(std::chrono::milliseconds(timeout_ms))) {
      if (external_allowed) {
        return {TestOutcome::kWarning, "getter did not receive a value in " + std::to_string(timeout_ms) + "ms"};
      }

      return {TestOutcome::kFailed, "getter did not receive a value in " + std::to_string(timeout_ms) + "ms"};
    }

    const auto value = getter.get();

    if VUNLIKELY (!value.has_value() || *value != 42) {
      return {TestOutcome::kFailed, "expected 42, got " + (value ? std::to_string(*value) : std::string("nullopt"))};
    }

    return {TestOutcome::kPassed, "1/1 values"};
  } catch (const std::exception& e) {
    return {TestOutcome::kFailed, std::string("exception: ") + e.what()};
  } catch (...) {
    return {TestOutcome::kFailed, "unknown exception during node construction"};
  }
}

TestOutcome run_paradigm_event() {
  print_test_row("EVENT  intra");

  const auto [outcome, detail] = run_event_roundtrip("intra://check/paradigm/event", 2000, 5, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_paradigm_method() {
  print_test_row("METHOD intra");

  const auto [outcome, detail] = run_method_roundtrip("intra://check/paradigm/method", 2000, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_paradigm_field() {
  print_test_row("FIELD  intra");

  const auto [outcome, detail] = run_field_roundtrip("intra://check/paradigm/field", 2000, false);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_event_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                  bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_event_roundtrip(url, timeout_ms, 5, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_method_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                   bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_method_roundtrip(url, timeout_ms, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

TestOutcome run_module_field_test(const char* label, const std::string& url, int timeout_ms, bool external_allowed,
                                  bool precondition_met, const char* skip_reason) {
  print_test_row(label);

  if (!precondition_met) {
    const std::string detail = std::string("skipped -- ") + (skip_reason ? skip_reason : "precondition not met");
    print_outcome(TestOutcome::kWarning, detail);
    return TestOutcome::kWarning;
  }

  const auto [outcome, detail] = run_field_roundtrip(url, timeout_ms, external_allowed);
  print_outcome(outcome, detail);
  return outcome;
}

int check_test() {
  std::cout << kColorHeader;
  std::cout << "[TITLE]" << std::string(kTitleWidth - 8, ' ') << "[STATUS]" << std::string(41, ' ') << "[DETAIL]";
  std::cout << kColorReset << std::endl;

  int passed = 0;
  int warned = 0;
  int failed = 0;

  auto account = [&passed, &warned, &failed](TestOutcome o) {
    switch (o) {
      case TestOutcome::kPassed:
        ++passed;
        break;
      case TestOutcome::kWarning:
        ++warned;
        break;
      case TestOutcome::kFailed:
        ++failed;
        break;
    }
  };

  account(run_paradigm_event());
  account(run_paradigm_method());
  account(run_paradigm_field());

#ifdef VLINK_SUPPORT_INTRA
  account(run_module_event_test("EVENT  intra://", "intra://check/module/intra/event", 1500, false, true, nullptr));
  account(run_module_method_test("METHOD intra://", "intra://check/module/intra/method", 1500, false, true, nullptr));
  account(run_module_field_test("FIELD  intra://", "intra://check/module/intra/field", 1500, false, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_SHM
  {
    const bool roudi_ok = vlink::ShmConf::auto_init_roudi(true, 2);
    account(run_module_event_test("EVENT  shm://", "shm://check/module/shm/event", 2000, true, roudi_ok,
                                  "iox-roudi not running"));
    account(run_module_method_test("METHOD shm://", "shm://check/module/shm/method", 2000, true, roudi_ok,
                                   "iox-roudi not running"));
    account(run_module_field_test("FIELD  shm://", "shm://check/module/shm/field", 2000, true, roudi_ok,
                                  "iox-roudi not running"));
  }
#endif

#ifdef VLINK_SUPPORT_SHM2
  {
    bool ok = true;

    try {
      const auto info = std::filesystem::space("/dev/shm");
      ok = info.available >= 64ULL * 1024ULL * 1024ULL;
    } catch (std::exception&) {
      ok = false;
    }

    account(run_module_event_test("EVENT  shm2://", "shm2://check/module/shm2/event", 2000, true, ok,
                                  "/dev/shm < 64MB or unavailable"));
    account(run_module_method_test("METHOD shm2://", "shm2://check/module/shm2/method", 2000, true, ok,
                                   "/dev/shm < 64MB or unavailable"));
    account(run_module_field_test("FIELD  shm2://", "shm2://check/module/shm2/field", 2000, true, ok,
                                  "/dev/shm < 64MB or unavailable"));
  }
#endif

#ifdef VLINK_SUPPORT_DDS
  account(run_module_event_test("EVENT  dds://", "dds://check/module/dds/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD dds://", "dds://check/module/dds/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  dds://", "dds://check/module/dds/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_DDSC
  account(run_module_event_test("EVENT  ddsc://", "ddsc://check/module/ddsc/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD ddsc://", "ddsc://check/module/ddsc/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  ddsc://", "ddsc://check/module/ddsc/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_DDSR
  account(run_module_event_test("EVENT  ddsr://", "ddsr://check/module/ddsr/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD ddsr://", "ddsr://check/module/ddsr/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  ddsr://", "ddsr://check/module/ddsr/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_ZENOH
  account(run_module_event_test("EVENT  zenoh://", "zenoh://check/module/zenoh/event", 3000, true, true, nullptr));
  account(run_module_method_test("METHOD zenoh://", "zenoh://check/module/zenoh/method", 3000, true, true, nullptr));
  account(run_module_field_test("FIELD  zenoh://", "zenoh://check/module/zenoh/field", 3000, true, true, nullptr));
#endif

#ifdef VLINK_SUPPORT_SOMEIP
  account(run_module_event_test("EVENT  someip://", "someip://0x5566/0x5486?groups=0x8&event=0x9", 2000, true, true,
                                nullptr));
#endif

#ifdef VLINK_SUPPORT_MQTT
  {
    const auto broker = vlink::Utils::get_env("VLINK_MQTT_BROKER");
    account(run_module_event_test("EVENT  mqtt://", "mqtt://check/module/mqtt", 2000, true, !broker.empty(),
                                  "VLINK_MQTT_BROKER not set"));
  }
#endif

#ifdef VLINK_SUPPORT_FDBUS
  {
    const bool name_server_ok = vlink::FdbusConf::has_name_server();
    account(run_module_event_test("EVENT  fdbus://", "fdbus://check/module/fdbus/event", 2000, true, name_server_ok,
                                  "name_server not running"));
    account(run_module_method_test("METHOD fdbus://", "fdbus://check/module/fdbus/method", 2000, true, name_server_ok,
                                   "name_server not running"));
    account(run_module_field_test("FIELD  fdbus://", "fdbus://check/module/fdbus/field", 2000, true, name_server_ok,
                                  "name_server not running"));
  }
#endif

  std::cout << std::endl;
  std::cout << kColorInfo << "Summary: " << kColorReset;
  std::cout << kColorPass << passed << " PASSED" << kColorReset << ", ";
  std::cout << kColorWarn << warned << " WARNING" << kColorReset << ", ";
  std::cout << kColorFail << failed << " FAILED" << kColorReset << std::endl;

  return failed;
}
