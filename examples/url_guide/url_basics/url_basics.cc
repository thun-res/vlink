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

#include <vlink/base/logger.h>
#include <vlink/impl/url_parser.h>
#include <vlink/vlink.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// url_basics.cc
//
// VLink uses a URL string as the single source of truth for "where + how"
// a message flows: changing the wire backend is just changing the URL
// prefix, no code edits required. The UrlParser parses any URL into its
// canonical components:
//
//   <transport>://<host>/<path>?<query>#<fragment>
//
// Components:
//   transport -- backend tag (intra / shm / dds / ddsc / ddsr / ddst /
//                zenoh / someip / mqtt / fdbus / qnx). Drives backend
//                selection during Node construction.
//   host      -- logical address; for local backends this is the first
//                segment of the topic name.
//   path      -- remainder of the topic identifier (slash-separated).
//   query     -- key/value pairs (domain, depth, qos profile name, etc.)
//                parsed into a dictionary the backend consults at attach.
//   fragment  -- backend-specific hint (e.g. `#direct` for intra zero-copy,
//                broker spec for mqtt, mode tag for fdbus/qnx).
//
// Component dictionary construction and clone-with-override below show how
// tooling builds/edits URLs without string surgery. Static classifier
// helpers (is_local_type / is_intra_type / get_sort_index) expose the URL
// category without instantiating any Node, useful for ordering decisions
// in proxies and bag tools.
// ---------------------------------------------------------------------------

// Pretty-print every UrlParser-exposed field. The query dictionary is built
// lazily on first access; subsequent get_query_dictionary() calls are O(1).
static void show(const std::string& url_str) {
  vlink::UrlParser parser(url_str);

  VLOG_I("URL:", url_str);
  VLOG_I("  transport=", parser.get_transport(), " host=", parser.get_host(), " path=", parser.get_path(),
         " port=", parser.get_port());
  VLOG_I("  query=", parser.get_query(), " fragment=", parser.get_fragment());

  const auto& dict = parser.get_query_dictionary();

  if (!dict.empty()) {
    for (const auto& [key, value] : dict) {
      VLOG_I("    ", key, "=", value);
    }
  }

  VLOG_I("  reconstructed=", parser.to_string());
}
int main() {
  // ---- Anatomy: one parser walk per transport family ----
  // Each example exercises a different transport-specific quirk so the
  // output makes the URL grammar concrete.
  show("intra://sensor/lidar?event=scan&pipeline=4#direct");
  show("shm://vehicle/speed?domain=1&depth=16&history=5&wait=1");
  show("dds://vehicle/speed?domain=42&depth=10&qos=sensor");
  show("ddsc://navigation/path?domain=1&qos=reliable");
  show("zenoh://robot/arm/joint1?domain=0&qos=sensor");
  show("someip://4660/22136?groups=1|2&event=16&field=1");
  show("mqtt://home/temperature?qos=1#tcp://192.168.1.100:1883");
  show("fdbus://audio/volume?event=level_changed#svc");
  show("qnx://sensor/radar?event=target_detected");

  // ---- Build a URL from individual components ----
  // Component dictionary -> canonical URL string. The Category argument
  // (kHierarchical) controls whether the parser emits `://` (hierarchical)
  // or `:` (opaque). The boolean flag enables strict validation -- it
  // throws on malformed component combinations instead of silently
  // producing garbage.
  std::map<vlink::UrlParser::Component, std::string> components;
  components[vlink::UrlParser::Component::kTransport] = "dds";
  components[vlink::UrlParser::Component::kHost] = "vehicle";
  components[vlink::UrlParser::Component::kPath] = "/telemetry/gps";
  components[vlink::UrlParser::Component::kQuery] = "domain=5&qos=sensor";
  components[vlink::UrlParser::Component::kFragment] = "";

  vlink::UrlParser built(components, vlink::UrlParser::Category::kHierarchical, true);
  VLOG_I("Built URL:", built.to_string());

  // ---- Override query on an existing URL without re-parsing the rest ----
  // The (parent, overrides) constructor is the "clone with edits" path. The
  // parsed AST is reused; only the overridden components are re-parsed.
  // UrlRemap layers on top of this to do runtime URL rewriting (e.g.
  // dev<->prod environment swap) without touching application code.
  vlink::UrlParser original("dds://vehicle/speed?domain=0&qos=sensor");
  std::map<vlink::UrlParser::Component, std::string> overrides;
  overrides[vlink::UrlParser::Component::kQuery] = "domain=99&qos=best";

  vlink::UrlParser modified(original, overrides);
  VLOG_I("Original:", original.to_string(), " Modified:", modified.to_string());

  // ---- Same API regardless of transport -- only the URL string changes ----
  // Subscriber/Publisher take the URL directly; replacing "intra://" with
  // "dds://" or "shm://" yields identical code, identical behavior, just
  // different wire path. Listener runs on the publisher's thread for intra
  // direct mode.
  vlink::Subscriber<std::string> sub("intra://demo/url_basics");
  sub.listen([](const std::string& msg) { VLOG_I("Received:", msg); });

  vlink::Publisher<std::string> pub("intra://demo/url_basics");
  pub.wait_for_subscribers();
  pub.publish("Hello from url_basics example!");
  std::this_thread::sleep_for(100ms);

  // ---- Static classifiers -- URL category without constructing a node ----
  // These are pure functions over the URL string; they don't initialize any
  // backend. Used by routing tools to decide whether to bridge / record /
  // forward. get_sort_index() returns a deterministic int that proxies use
  // to order topics (intra=0, shm=1, dds=2, ...) so deterministic snapshots
  // are reproducible across runs.
  VLOG_I("is_local_type('intra://x'):", vlink::Url::is_local_type("intra://x"));
  VLOG_I("is_intra_type('intra://x'):", vlink::Url::is_intra_type("intra://x"));
  VLOG_I("is_shm_type('shm://x'):", vlink::Url::is_shm_type("shm://x"));
  VLOG_I("get_sort_index('intra://x'):", vlink::Url::get_sort_index("intra://x"));
  VLOG_I("get_sort_index('dds://x'):", vlink::Url::get_sort_index("dds://x"));

  return 0;
}
