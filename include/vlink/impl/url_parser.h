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

/**
 * @file url_parser.h
 * @brief Strict subset of an RFC 3986 URL parser used by the VLink transport layer.
 *
 * @details
 * This is an internal implementation header used by @c Protocol and the
 * URL-driven transport routing path; application code does not interact with
 * it directly.  @c UrlParser decomposes a VLink topic URL into the parts that
 * @c Url needs to select a transport @c Conf and to configure the resulting
 * @c NodeImpl.  Typical inputs look like:
 *
 * @code
 *   dds://my_domain/vehicle/speed?domain_id=1&qos=best_effort
 *   intra://my_topic
 *   someip://127.0.0.1:30490/my_service?instance_id=1
 * @endcode
 *
 * @par URL grammar
 * | Component  | Example                  | Description                                       |
 * | ---------- | ------------------------ | ------------------------------------------------- |
 * | transport  | @c dds                   | URI scheme / VLink transport prefix before @c :// |
 * | content    | @c //host/path           | Full content portion after the scheme separator   |
 * | username   | @c user                  | Optional credential before host                   |
 * | password   | @c pass                  | Optional credential after @c :                    |
 * | host       | @c 127.0.0.1             | Hostname or IP address                            |
 * | port       | @c 30490                 | TCP / UDP port number                             |
 * | path       | @c vehicle/speed         | Topic path; leading slash stored separately       |
 * | query      | @c domain_id=1&qos=...   | Raw query string after @c ?                       |
 * | fragment   | @c section1              | Fragment identifier after @c #                    |
 *
 * @par Parser outputs
 * | Accessor                        | Resulting type                              |
 * | ------------------------------- | ------------------------------------------- |
 * | @c get_transport() const        | @c const std::string&                       |
 * | @c get_category() const         | @c Category                                 |
 * | @c get_content() const          | @c const std::string& (non-hierarchical)    |
 * | @c get_username() const         | @c const std::string& (hierarchical)        |
 * | @c get_password() const         | @c const std::string& (hierarchical)        |
 * | @c get_host() const             | @c const std::string& (hierarchical)        |
 * | @c get_port() const             | @c int64_t (hierarchical)                   |
 * | @c get_path() const             | @c const std::string& (hierarchical)        |
 * | @c get_query() const            | @c const std::string&                       |
 * | @c get_query_dictionary() const | @c const std::map<std::string,std::string>& |
 * | @c get_fragment() const         | @c const std::string&                       |
 * | @c to_string() const            | @c std::string (reconstructed URL)          |
 *
 * @par Query dictionary
 * The raw query string is split into a @c std::map<std::string,std::string>
 * using either @c & (default) or @c ; as the key/value pair separator.  Each
 * token is split on the first @c = character; keys without a value become
 * empty strings.
 *
 * @par Category
 * - @c kHierarchical: standard @c scheme://authority/path?query#fragment URL.
 * - @c kNonHierarchical: opaque @c scheme:content form (e.g. @c mailto:user(at)host).
 *
 * @par Example
 * @code
 * vlink::UrlParser parser("dds://10.0.0.1:7400/cars/1?qos=best_effort#stats");
 *
 * parser.get_transport();         // "dds"
 * parser.get_host();              // "10.0.0.1"
 * parser.get_port();              // 7400
 * parser.get_path();              // "cars/1"
 * parser.get_query_dictionary();  // {"qos" -> "best_effort"}
 * parser.get_fragment();          // "stats"
 * @endcode
 *
 * @note @c UrlParser is a value type; parsing happens once during construction
 *       and accessors return references valid for the lifetime of the object.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "../base/macros.h"

namespace vlink {

/**
 * @class UrlParser
 * @brief Immutable URL decomposition used by the VLink transport router.
 *
 * @details
 * Parses the input URL once at construction and exposes each component through
 * @c const accessors.  For hierarchical URLs the stored path drops the leading
 * @c / marker; the rootedness flag is preserved separately so @c to_string()
 * can reconstruct an equivalent representation.
 */
class VLINK_EXPORT UrlParser final {
 public:
  /**
   * @enum Category
   * @brief Distinguishes hierarchical and non-hierarchical URI forms.
   */
  enum class Category : uint8_t {
    kHierarchical = 0,     ///< Standard @c scheme://authority/path layout (most VLink transports).
    kNonHierarchical = 1,  ///< Opaque @c scheme:content layout (e.g. @c mailto:).
  };

  /**
   * @enum Component
   * @brief Component identifiers accepted by the explicit-components constructor.
   */
  enum class Component : uint8_t {
    kTransport = 0,  ///< URI scheme / VLink transport prefix.
    kContent = 1,    ///< Full content string after the scheme separator.
    kUsername = 2,   ///< Optional authentication username.
    kPassword = 3,   ///< Optional authentication password.
    kHost = 4,       ///< Hostname or IP address.
    kPort = 5,       ///< Port number (stored as string in the components map).
    kPath = 6,       ///< Resource path (e.g. @c /vehicle/speed).
    kQuery = 7,      ///< Raw query string (without the leading @c ?).
    kFragment = 8,   ///< Fragment identifier (without the leading @c #).
  };

  /**
   * @enum Separator
   * @brief Selectable separator used to break the query string into key/value pairs.
   */
  enum class Separator : uint8_t {
    kAmpersand = 0,  ///< @c & separator (default; @c key=val&key2=val2).
    kSemicolon = 1,  ///< @c ; separator (alternative; @c key=val;key2=val2).
  };

  /**
   * @brief Parses @p str as a null-terminated URL string.
   *
   * @param str        URL string to parse.
   * @param category   Hierarchical or non-hierarchical layout; default hierarchical.
   * @param separator  Query-pair separator; default @c &.
   */
  explicit UrlParser(const char* str, Category category = Category::kHierarchical,
                     Separator separator = Separator::kAmpersand);

  /**
   * @brief Parses @p str as an @c std::string URL.
   *
   * @param str        URL string to parse.
   * @param category   Hierarchical or non-hierarchical layout; default hierarchical.
   * @param separator  Query-pair separator; default @c &.
   */
  explicit UrlParser(const std::string& str, Category category = Category::kHierarchical,
                     Separator separator = Separator::kAmpersand);

  /**
   * @brief Builds a parser directly from an explicit component map.
   *
   * @details
   * Useful for constructing a URL from a previously decomposed set of fields
   * instead of parsing a raw string.
   *
   * @param components  Map of @c Component to value for every present component.
   * @param category    Hierarchical or non-hierarchical layout.
   * @param rooted      @c true when the path starts with @c / (hierarchical URLs).
   * @param separator   Query-pair separator; default @c &.
   *
   * @throws Exception::RuntimeError when @p category is @c Category::kHierarchical
   *         and @c Component::kPath is missing, or when @p category is
   *         @c Category::kNonHierarchical and @c Component::kContent is missing.
   */
  explicit UrlParser(const std::map<Component, std::string>& components, Category category, bool rooted,
                     Separator separator = Separator::kAmpersand);

  /**
   * @brief Builds a parser by copying @p other and overriding selected components.
   *
   * @details
   * Equivalent to producing a modified copy of an existing URL.
   *
   * @param other         Source parser.
   * @param replacements  Components that override the corresponding entries in @p other.
   */
  explicit UrlParser(const UrlParser& other, const std::map<Component, std::string>& replacements);

  /**
   * @brief Returns the parsed transport string (e.g. @c "dds" or @c "intra").
   *
   * @return Reference to the transport component; empty when absent.
   */
  [[nodiscard]] const std::string& get_transport() const;

  /**
   * @brief Returns the URL category supplied at construction time.
   *
   * @return Either @c Category::kHierarchical or @c Category::kNonHierarchical.
   */
  [[nodiscard]] Category get_category() const;

  /**
   * @brief Returns the full content portion of a non-hierarchical URL.
   *
   * @details
   * Calling this method on a hierarchical URL throws @c Exception::RuntimeError.
   *
   * @return Reference to the content string.
   */
  [[nodiscard]] const std::string& get_content() const;

  /**
   * @brief Returns the authentication username component.
   *
   * @details
   * Valid only for hierarchical URLs; otherwise throws @c Exception::RuntimeError.
   *
   * @return Reference to the username; empty when absent.
   */
  [[nodiscard]] const std::string& get_username() const;

  /**
   * @brief Returns the authentication password component.
   *
   * @details
   * Valid only for hierarchical URLs; otherwise throws @c Exception::RuntimeError.
   *
   * @return Reference to the password; empty when absent.
   */
  [[nodiscard]] const std::string& get_password() const;

  /**
   * @brief Returns the host component (hostname or IP address).
   *
   * @details
   * Valid only for hierarchical URLs; otherwise throws @c Exception::RuntimeError.
   *
   * @return Reference to the host string; empty when absent.
   */
  [[nodiscard]] const std::string& get_host() const;

  /**
   * @brief Returns the port number, or @c 0 when no port was specified.
   *
   * @details
   * Valid only for hierarchical URLs; otherwise throws @c Exception::RuntimeError.
   *
   * @return Parsed port as @c int64_t; @c 0 when absent.
   */
  [[nodiscard]] int64_t get_port() const;

  /**
   * @brief Returns the path component of the URL.
   *
   * @details
   * Valid only for hierarchical URLs; otherwise throws @c Exception::RuntimeError.
   * The returned string omits the leading @c / for rooted hierarchical URLs;
   * the rootedness flag is retained internally and emitted again by
   * @c to_string().
   *
   * @return Reference to the path; empty when absent.
   */
  [[nodiscard]] const std::string& get_path() const;

  /**
   * @brief Returns the raw query string (without the leading @c ?).
   *
   * @return Reference to the raw query; empty when no query was present.
   */
  [[nodiscard]] const std::string& get_query() const;

  /**
   * @brief Returns the parsed query string as a key/value dictionary.
   *
   * @details
   * Built by splitting the raw query on the configured @c Separator and then
   * splitting each token on the first @c = character.  Keys without a value
   * appear with an empty string.
   *
   * @return Reference to the query dictionary.
   */
  [[nodiscard]] const std::map<std::string, std::string>& get_query_dictionary() const;

  /**
   * @brief Returns the fragment identifier (without the leading @c #).
   *
   * @return Reference to the fragment; empty when absent.
   */
  [[nodiscard]] const std::string& get_fragment() const;

  /**
   * @brief Reconstructs the URL string from the parsed components.
   *
   * @details
   * Hierarchical URLs are reassembled as @c scheme://authority/path?query#fragment
   * when an authority is present; non-hierarchical URLs as
   * @c scheme:content?query#fragment.  The output may differ from the original
   * input when equivalent components were originally written differently.
   *
   * @return Reconstructed URL string.
   */
  [[nodiscard]] std::string to_string() const;

 private:
  void setup(const std::string& str, Category category);

  std::string::const_iterator parse_transport(const std::string& str, std::string::const_iterator transport_start);

  std::string::const_iterator parse_content(const std::string& str, std::string::const_iterator content_start);

  std::string::const_iterator parse_username(const std::string& str, const std::string& content,
                                             std::string::const_iterator username_start);

  std::string::const_iterator parse_password(const std::string& str, const std::string& content,
                                             std::string::const_iterator password_start);

  std::string::const_iterator parse_host(const std::string& str, const std::string& content,
                                         std::string::const_iterator host_start);

  std::string::const_iterator parse_port(const std::string& str, const std::string& content,
                                         std::string::const_iterator port_start);

  std::string::const_iterator parse_query(const std::string& str, std::string::const_iterator query_start);

  std::string::const_iterator parse_fragment(const std::string& str, std::string::const_iterator fragment_start);

  void init_query_dictionary();

  std::string transport_;
  std::string content_;
  std::string username_;
  std::string password_;
  std::string host_;
  std::string path_;
  std::string query_;
  std::string fragment_;
  std::map<std::string, std::string> query_dict_;
  Category category_{Category::kHierarchical};
  int64_t port_{0};
  bool is_rooted_{false};
  Separator separator_{Separator::kAmpersand};
};

}  // namespace vlink
