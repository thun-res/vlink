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

#include "./impl/url_parser.h"

#include <exception>
#include <map>
#include <string>
#include <utility>

#include "./base/exception.h"

namespace vlink {

[[maybe_unused]] static int64_t parse_port_value(const std::string& port_str) {
  try {
    auto parsed = std::stoul(port_str);

    if VUNLIKELY (parsed > 65535U) {
      throw Exception::RuntimeError("Port out of range (must be 0-65535): \"" + port_str + "\".");
    }

    return static_cast<int64_t>(parsed);
  } catch (const std::exception&) {
    throw Exception::RuntimeError("Invalid port \"" + port_str + "\".");
  }
}

// UrlParser
UrlParser::UrlParser(const char* str, Category category, Separator separator)
    : category_(category), separator_(separator) {
  setup(std::string(str), category);
}

UrlParser::UrlParser(const std::string& str, Category category, Separator separator)
    : category_(category), separator_(separator) {
  setup(str, category);
}

UrlParser::UrlParser(const std::map<Component, std::string>& components, Category category, bool rooted,
                     Separator separator)
    : category_(category), is_rooted_(rooted), separator_(separator) {
  if VLIKELY (components.count(Component::kTransport) != 0) {
    if VUNLIKELY (components.at(Component::kTransport).empty()) {
      throw Exception::RuntimeError("The URL transport prefix cannot be empty.");
    }

    transport_ = components.at(Component::kTransport);
  } else {
    throw Exception::RuntimeError("A URL must include a transport prefix.");
  }

  if (category == Category::kHierarchical) {
    if VUNLIKELY (components.count(Component::kContent) != 0) {
      throw Exception::RuntimeError("The content component is only valid for non-hierarchical URLs.");
    }

    bool has_username = components.count(Component::kUsername) != 0;
    bool has_password = components.count(Component::kPassword) != 0;

    if (has_username && has_password) {
      username_ = components.at(Component::kUsername);
      password_ = components.at(Component::kPassword);
    } else if VUNLIKELY ((has_username && !has_password) || (!has_username && has_password)) {
      throw Exception::RuntimeError("If a username or password is supplied, both must be provided.");
    }

    if (components.count(Component::kHost) != 0) {
      host_ = components.at(Component::kHost);
    }

    if (components.count(Component::kPort) != 0) {
      port_ = parse_port_value(components.at(Component::kPort));
    }

    if VLIKELY (components.count(Component::kPath) != 0) {
      path_ = components.at(Component::kPath);
    } else {
      throw Exception::RuntimeError("A path is required on a hierarchical URL, even an empty path.");
    }

  } else {
    if VUNLIKELY (components.count(Component::kUsername) != 0 || components.count(Component::kPassword) != 0 ||
                  components.count(Component::kHost) != 0 || components.count(Component::kPort) != 0 ||
                  components.count(Component::kPath) != 0) {
      throw Exception::RuntimeError("None of the hierarchical components are allowed in a non-hierarchical URL.");
    }

    if VLIKELY (components.count(Component::kContent) != 0) {
      content_ = components.at(Component::kContent);
    } else {
      throw Exception::RuntimeError("A non-hierarchical URL requires a content component, even if it is empty.");
    }
  }

  if (components.count(Component::kQuery) != 0) {
    query_ = components.at(Component::kQuery);
  }

  if (components.count(Component::kFragment) != 0) {
    fragment_ = components.at(Component::kFragment);
  }

  init_query_dictionary();
}

UrlParser::UrlParser(const UrlParser& other, const std::map<Component, std::string>& replacements)
    : category_(other.category_), is_rooted_(other.is_rooted_), separator_(other.separator_) {
  transport_ = (replacements.count(Component::kTransport)) ? replacements.at(Component::kTransport) : other.transport_;

  if (category_ == Category::kHierarchical) {
    username_ = (replacements.count(Component::kUsername)) ? replacements.at(Component::kUsername) : other.username_;
    password_ = (replacements.count(Component::kPassword)) ? replacements.at(Component::kPassword) : other.password_;
    host_ = (replacements.count(Component::kHost)) ? replacements.at(Component::kHost) : other.host_;
    port_ = (replacements.count(Component::kPort)) ? parse_port_value(replacements.at(Component::kPort)) : other.port_;
    path_ = (replacements.count(Component::kPath)) ? replacements.at(Component::kPath) : other.path_;
  } else {
    content_ = (replacements.count(Component::kContent)) ? replacements.at(Component::kContent) : other.content_;
  }

  query_ = (replacements.count(Component::kQuery)) ? replacements.at(Component::kQuery) : other.query_;
  fragment_ = (replacements.count(Component::kFragment)) ? replacements.at(Component::kFragment) : other.fragment_;

  init_query_dictionary();
}

const std::string& UrlParser::get_transport() const { return transport_; }

UrlParser::Category UrlParser::get_category() const { return category_; }

const std::string& UrlParser::get_content() const {
  if VUNLIKELY (category_ != Category::kNonHierarchical) {
    throw Exception::RuntimeError("The content Component is only valid for non-hierarchical URLs.");
  }

  return content_;
}

const std::string& UrlParser::get_username() const {
  if VUNLIKELY (category_ != Category::kHierarchical) {
    throw Exception::RuntimeError("The username Component is only valid for hierarchical URLs.");
  }

  return username_;
}

const std::string& UrlParser::get_password() const {
  if VUNLIKELY (category_ != Category::kHierarchical) {
    throw Exception::RuntimeError("The password Component is only valid for hierarchical URLs.");
  }

  return password_;
}

const std::string& UrlParser::get_host() const {
  if VUNLIKELY (category_ != Category::kHierarchical) {
    throw Exception::RuntimeError("The host Component is only valid for hierarchical URLs.");
  }

  return host_;
}

int64_t UrlParser::get_port() const {
  if VUNLIKELY (category_ != Category::kHierarchical) {
    throw Exception::RuntimeError("The port Component is only valid for hierarchical URLs.");
  }

  return port_;
}

const std::string& UrlParser::get_path() const {
  if VUNLIKELY (category_ != Category::kHierarchical) {
    throw Exception::RuntimeError("The path Component is only valid for hierarchical URLs.");
  }

  return path_;
}

const std::string& UrlParser::get_query() const { return query_; }

const std::map<std::string, std::string>& UrlParser::get_query_dictionary() const { return query_dict_; }

const std::string& UrlParser::get_fragment() const { return fragment_; }

std::string UrlParser::to_string() const {
  std::string full_url;
  full_url.append(transport_);
  full_url.append(":");

  if (category_ == Category::kNonHierarchical) {
    full_url.append(content_);
  } else if (content_.length() > path_.length()) {
    full_url.append("//");

    if (!(username_.empty() || password_.empty())) {
      full_url.append(username_);
      full_url.append(":");
      full_url.append(password_);
      full_url.append("@");
    }

    full_url.append(host_);

    if (port_ != 0) {
      full_url.append(":");
      full_url.append(std::to_string(port_));
    }
  }

  if (is_rooted_) {
    full_url.append("/");
  }

  full_url.append(path_);

  if (!query_.empty()) {
    full_url.append("?");
    full_url.append(query_);
  }

  if (!fragment_.empty()) {
    full_url.append("#");
    full_url.append(fragment_);
  }

  return full_url;
}

void UrlParser::setup(const std::string& str, Category category) {
  (void)category;

  size_t const url_length = str.length();

  if VUNLIKELY (url_length == 0) {
    throw Exception::RuntimeError("URLs cannot be of zero length.");
  }

  std::string::const_iterator cursor = parse_transport(str, str.begin());
  cursor = parse_content(str, (cursor + 1));

  if ((cursor != str.end()) && (*cursor == '?')) {
    cursor = parse_query(str, (cursor + 1));
  }

  if ((cursor != str.end()) && (*cursor == '#')) {
    cursor = parse_fragment(str, (cursor + 1));
  }

  init_query_dictionary();
}

std::string::const_iterator UrlParser::parse_transport(const std::string& str,
                                                       std::string::const_iterator transport_start) {
  std::string::const_iterator transport_end = transport_start;

  while ((transport_end != str.end()) && (*transport_end != ':')) {
    if VUNLIKELY (!std::isalnum(*transport_end) && (*transport_end != '-') && (*transport_end != '+') &&
                  (*transport_end != '.')) {
      throw Exception::RuntimeError("Invalid character found in the URL transport prefix. Supplied URL was: \"" + str +
                                    "\".");
    }

    ++transport_end;
  }

  if VUNLIKELY (transport_end == str.end()) {
    throw Exception::RuntimeError("End of URL found while parsing the transport prefix. Supplied URL was: \"" + str +
                                  "\".");
  }

  if VUNLIKELY (transport_start == transport_end) {
    throw Exception::RuntimeError("The URL transport prefix cannot be zero-length. Supplied URL was: \"" + str + "\".");
  }

  transport_ = std::string(transport_start, transport_end);

  return transport_end;
}

std::string::const_iterator UrlParser::parse_content(const std::string& str,
                                                     std::string::const_iterator content_start) {
  std::string::const_iterator content_end = content_start;

  while ((content_end != str.end()) && (*content_end != '?') && (*content_end != '#')) {
    ++content_end;
  }

  content_ = std::string(content_start, content_end);

  if ((category_ == Category::kHierarchical) && (!content_.empty())) {
    std::string::const_iterator path_start = content_.begin();
    std::string::const_iterator path_end = content_.end();

    if (!content_.compare(0, 2, "//")) {
      std::string::const_iterator authority_cursor = (content_.begin() + 2);

      if (content_.find_first_of('@') != std::string::npos) {
        std::string::const_iterator userpass_divider = parse_username(str, content_, authority_cursor);
        authority_cursor = parse_password(str, content_, (userpass_divider + 1));
        ++authority_cursor;
      }

      authority_cursor = parse_host(str, content_, authority_cursor);

      if ((authority_cursor != content_.end()) && (*authority_cursor == ':')) {
        authority_cursor = parse_port(str, content_, (authority_cursor + 1));
      }

      if ((authority_cursor != content_.end()) && (*authority_cursor == '/')) {
        is_rooted_ = true;
        path_start = authority_cursor + 1;
      }

      if (authority_cursor == content_.end()) {
        path_start = content_.end();
      }
    } else if (!content_.compare(0, 1, "/")) {
      is_rooted_ = true;
      ++path_start;
    }

    path_ = std::string(path_start, path_end);
  }

  return content_end;
}

std::string::const_iterator UrlParser::parse_username(const std::string& str, const std::string& content,
                                                      std::string::const_iterator username_start) {
  (void)content;

  std::string::const_iterator username_end = username_start;

  while (username_end != content.end() && *username_end != ':') {
    if VUNLIKELY (*username_end == '@') {
      throw Exception::RuntimeError(
          "A username in the authority must be followed by ':password'. Supplied URL was: \"" + str + "\".");
    }

    ++username_end;
  }

  if VUNLIKELY (username_end == content.end()) {
    throw Exception::RuntimeError("A username in the authority must be followed by ':password'. Supplied URL was: \"" +
                                  str + "\".");
  }

  username_ = std::string(username_start, username_end);

  return username_end;
}

std::string::const_iterator UrlParser::parse_password(const std::string& str, const std::string& content,
                                                      std::string::const_iterator password_start) {
  (void)content;

  std::string::const_iterator password_end = password_start;

  while (password_end != content.end() && *password_end != '@') {
    ++password_end;
  }

  if VUNLIKELY (password_end == content.end()) {
    throw Exception::RuntimeError("A password in the authority must be followed by '@'. Supplied URL was: \"" + str +
                                  "\".");
  }

  password_ = std::string(password_start, password_end);

  return password_end;
}

std::string::const_iterator UrlParser::parse_host(const std::string& str, const std::string& content,
                                                  std::string::const_iterator host_start) {
  std::string::const_iterator host_end = host_start;

  while (host_end != content.end()) {
    if (*host_end == '[') {
      while ((host_end != content.end()) && (*host_end != ']')) {
        ++host_end;
      }

      if VUNLIKELY (host_end == content.end()) {
        throw Exception::RuntimeError(
            "End of content Component encountered "
            "while parsing the host Component. "
            "Supplied URL was: \"" +
            str + "\".");
      }

      ++host_end;

      break;
    }

    if ((*host_end == ':') || (*host_end == '/')) {
      break;
    }

    ++host_end;
  }

  host_ = std::string(host_start, host_end);
  return host_end;
}

std::string::const_iterator UrlParser::parse_port(const std::string& str, const std::string& content,
                                                  std::string::const_iterator port_start) {
  std::string::const_iterator port_end = port_start;

  while ((port_end != content.end()) && (*port_end != '/')) {
    if VUNLIKELY (!std::isdigit(*port_end)) {
      throw Exception::RuntimeError(
          "Invalid character while parsing the port. "
          "Supplied URL was: \"" +
          str + "\".");
    }

    ++port_end;
  }

  if (port_start != port_end) {
    port_ = parse_port_value(std::string(port_start, port_end));
  }

  return port_end;
}

std::string::const_iterator UrlParser::parse_query(const std::string& str, std::string::const_iterator query_start) {
  std::string::const_iterator query_end = query_start;

  while ((query_end != str.end()) && (*query_end != '#')) {
    ++query_end;
  }

  query_ = std::string(query_start, query_end);

  return query_end;
}

std::string::const_iterator UrlParser::parse_fragment(const std::string& str,
                                                      std::string::const_iterator fragment_start) {
  fragment_ = std::string(fragment_start, str.end());

  return str.end();
}

void UrlParser::init_query_dictionary() {
  if (!query_.empty()) {
    char separator = (separator_ == Separator::kAmpersand) ? '&' : ';';
    size_t carat = 0;
    size_t stanza_end = query_.find_first_of(separator);

    do {
      std::string stanza =
          query_.substr(carat, ((stanza_end != std::string::npos) ? (stanza_end - carat) : std::string::npos));
      size_t key_value_divider = stanza.find_first_of('=');
      std::string key = stanza.substr(0, key_value_divider);
      std::string value;

      if (key_value_divider != std::string::npos) {
        value = stanza.substr((key_value_divider + 1));
      }

      if VUNLIKELY (query_dict_.count(key) != 0) {
        throw Exception::RuntimeError("Bad key in the query string!");
      }

      query_dict_.emplace(key, std::move(value));
      carat = ((stanza_end != std::string::npos) ? (stanza_end + 1) : std::string::npos);
      stanza_end = query_.find_first_of(separator, carat);
    } while ((stanza_end != std::string::npos) || (carat != std::string::npos));
  }
}

}  // namespace vlink
