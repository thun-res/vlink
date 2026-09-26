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

#include "./monitor_common.h"

bool append_command_arguments(const std::string& text, std::vector<std::string>& args) {
  std::string current;
  char quote = '\0';
  bool has_token = false;

  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];

    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else if (c == '\\' && quote == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        current.push_back(text[++i]);
      } else {
        current.push_back(c);
      }

      has_token = true;
      continue;
    }

    if (c == '\'' || c == '"') {
      quote = c;
      has_token = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (has_token) {
        args.emplace_back(std::move(current));
        current.clear();
        has_token = false;
      }
    } else if (c == '\\' && i + 1 < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '\'' || text[i + 1] == '"')) {
      current.push_back(text[++i]);
      has_token = true;
    } else {
      current.push_back(c);
      has_token = true;
    }
  }

  if (quote != '\0') {
    return false;
  }

  if (has_token) {
    args.emplace_back(std::move(current));
  }

  return true;
}

int run_decoder_process(const std::string& executable, const std::vector<std::string>& args) {
#ifdef _WIN32
  std::wstring command_line;

  auto append_quoted_argument = [&command_line](const std::wstring& argument) {
    if (!command_line.empty()) {
      command_line.push_back(L' ');
    }

    command_line.push_back(L'"');
    size_t backslash_count = 0;

    for (const wchar_t c : argument) {
      if (c == L'\\') {
        ++backslash_count;
      } else if (c == L'"') {
        command_line.append(backslash_count * 2 + 1, L'\\');
        command_line.push_back(c);
        backslash_count = 0;
      } else {
        command_line.append(backslash_count, L'\\');
        command_line.push_back(c);
        backslash_count = 0;
      }
    }

    command_line.append(backslash_count * 2, L'\\');
    command_line.push_back(L'"');
  };

  const auto wide_executable = vlink::Helpers::string_to_wstring(executable);

  for (const auto& arg : args) {
    append_quoted_argument(vlink::Helpers::string_to_wstring(arg));
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  const HANDLE standard_handles[] = {::GetStdHandle(STD_INPUT_HANDLE), ::GetStdHandle(STD_OUTPUT_HANDLE),
                                     ::GetStdHandle(STD_ERROR_HANDLE)};
  HANDLE inherited_handles[3]{};

  for (size_t i = 0; i < 3; ++i) {
    if (standard_handles[i] == nullptr || standard_handles[i] == INVALID_HANDLE_VALUE ||
        !::DuplicateHandle(::GetCurrentProcess(), standard_handles[i], ::GetCurrentProcess(), &inherited_handles[i], 0,
                           TRUE, DUPLICATE_SAME_ACCESS)) {
      for (size_t j = 0; j < i; ++j) {
        ::CloseHandle(inherited_handles[j]);
      }

      return -1;
    }
  }

  startup_info.hStdInput = inherited_handles[0];
  startup_info.hStdOutput = inherited_handles[1];
  startup_info.hStdError = inherited_handles[2];
  PROCESS_INFORMATION process_info{};

  const bool created = ::CreateProcessW(wide_executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup_info, &process_info);

  for (const HANDLE handle : inherited_handles) {
    ::CloseHandle(handle);
  }

  if (!created) {
    return -1;
  }

  ::CloseHandle(process_info.hThread);

  const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 0;
  const bool exited = wait_result == WAIT_OBJECT_0 && ::GetExitCodeProcess(process_info.hProcess, &exit_code);
  ::CloseHandle(process_info.hProcess);

  return exited ? static_cast<int>(exit_code) : -1;
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);

  for (const auto& arg : args) {
    argv.emplace_back(const_cast<char*>(arg.c_str()));
  }

  argv.emplace_back(nullptr);

  struct sigaction ignore_action{};
  struct sigaction old_int_action{};
  struct sigaction old_quit_action{};
  ignore_action.sa_handler = SIG_IGN;
  sigemptyset(&ignore_action.sa_mask);

  if (sigaction(SIGINT, &ignore_action, &old_int_action) != 0) {
    return -1;
  }

  if (sigaction(SIGQUIT, &ignore_action, &old_quit_action) != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    return -1;
  }

  posix_spawnattr_t attr;

  if (posix_spawnattr_init(&attr) != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  sigset_t child_default_signals;
  sigemptyset(&child_default_signals);
  sigaddset(&child_default_signals, SIGINT);
  sigaddset(&child_default_signals, SIGQUIT);

  if (posix_spawnattr_setsigdefault(&attr, &child_default_signals) != 0 ||
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF) != 0) {
    posix_spawnattr_destroy(&attr);
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  pid_t pid = -1;
#ifdef __APPLE__
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  const int spawn_error = posix_spawn(&pid, executable.c_str(), nullptr, &attr, argv.data(), environment);
  posix_spawnattr_destroy(&attr);

  if (spawn_error != 0) {
    sigaction(SIGINT, &old_int_action, nullptr);
    sigaction(SIGQUIT, &old_quit_action, nullptr);
    return -1;
  }

  int status = 0;

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      sigaction(SIGINT, &old_int_action, nullptr);
      sigaction(SIGQUIT, &old_quit_action, nullptr);
      return -1;
    }
  }

  sigaction(SIGINT, &old_int_action, nullptr);
  sigaction(SIGQUIT, &old_quit_action, nullptr);

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }

  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
#endif
}
