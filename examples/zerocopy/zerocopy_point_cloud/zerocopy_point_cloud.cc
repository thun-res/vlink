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
#include <vlink/vlink.h>
#include <vlink/zerocopy/point_cloud.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// zerocopy_point_cloud.cc
//
// PointCloud is the zerocopy container for variable-schema point sets
// (lidar, radar, depth). Unlike CameraFrame it carries a *self-describing*
// schema: each point is a tightly packed struct whose field names + types
// are stored alongside the buffer. The schema describes:
//   * names    -- e.g. {"x","y","z","intensity","ring"}
//   * sizes    -- bytes per field (1/2/4/8)
//   * types    -- enum tag (kFloat / kDouble / kUint8 / kInt32 / ...)
// pack_size() is the sum of field sizes; a 1000-point XYZ cloud is
// 1000 * 12 = 12000 bytes plus a small header.
//
// Field-access path:
//   get_key_map() -- name -> byte offset within a point.
//   get_value<T>(index, key_map, "name") -- typed read.
//   Specialised XYZ helpers (push_value_v3f / get_value_v3f) skip the map
//   lookup for the common case where the first three fields are float XYZ.
//
// On the wire, the schema travels with the payload: a receiver that has
// never seen this exact field set can still iterate it correctly. This
// is what makes PointCloud play nicely with rerun.io / Foxglove / ROS2.
// ---------------------------------------------------------------------------

int main() {
  // ---- Section 1: create point cloud with float XYZ ----
  // Three float fields named x/y/z. capacity=1000 reserves storage for up
  // to 1000 points; actual size grows with push_value.
  {
    VLOG_I("[1] Create Point Cloud with float XYZ");

    vlink::zerocopy::PointCloud pc;
    const bool ok = pc.create<float, float, float>(1000, {"x", "y", "z"});
    VLOG_I("  create<float,float,float>(1000) = ", ok);
    VLOG_I("  pack_size=", pc.pack_size(), " bytes/point; is_owner=", pc.is_owner());

    pc.push_value(1.0F, 2.0F, 3.0F);
    pc.push_value(4.0F, 5.0F, 6.0F);
    pc.push_value(7.0F, 8.0F, 9.0F);
    VLOG_I("  after push: size=", pc.size());

    float x;
    float y;
    float z;
    pc.get_value_v3f(x, y, z, 0);
    VLOG_I("  point[0] = (", x, ", ", y, ", ", z, ")");

    auto v = pc.get_value_v3f(1);
    VLOG_I("  point[1] = (", v.x, ", ", v.y, ", ", v.z, ")");
  }

  // ---- Section 2: XYZ + intensity (create_v3f) ----
  // create_v3f<float>(N, extras) is shorthand for "three XYZ floats plus
  // these extra typed fields". pack_size = 12 (XYZ) + 4 (intensity) = 16.
  {
    VLOG_I("[2] Float XYZ + Intensity (create_v3f)");

    vlink::zerocopy::PointCloud pc;
    pc.create_v3f<float>(500, {"intensity"});
    VLOG_I("  pack_size=", pc.pack_size(), " bytes/point (3*4 + 1*4 = 16)");

    pc.push_value_v3f(1.0F, 2.0F, 3.0F, 0.8F);
    pc.push_value_v3f(4.0F, 5.0F, 6.0F, 0.5F);
    pc.push_value_v3f(10.0F, 20.0F, 30.0F, 1.0F);

    auto key_map = pc.get_key_map();
    auto intensity = pc.get_value<float>(2, key_map, "intensity");
    VLOG_I("  point[2].intensity = ", intensity);
  }

  // ---- Section 3: schema inspection ----
  // Tools / visualisers reflect on the stored schema instead of hard-coding
  // a layout. KeyList preserves declaration order; key_map gives O(1)
  // offset lookup by name.
  {
    VLOG_I("[3] Schema Inspection");

    vlink::zerocopy::PointCloud pc;
    pc.create<float, float, float, uint8_t>(100, {"x", "y", "z", "ring"});
    VLOG_I("  names=", pc.get_protocol_name_str());
    VLOG_I("  sizes=", pc.get_protocol_size_str());
    VLOG_I("  types=", pc.get_protocol_type_str());

    vlink::zerocopy::PointCloud::KeyList key_list;
    auto key_map = pc.get_key_map(&key_list);

    for (const auto& key : key_list) {
      VLOG_I("    name=", key.name, " type=", static_cast<int>(key.type), " size=", static_cast<int>(key.size),
             " offset=", key_map[key.name]);
    }
  }

  // ---- Section 4: serialize / deserialize ----
  // Wire format = [header][schema descriptor][point payload]. check_valid
  // verifies the magic / length consistency before downstream code touches
  // the buffer.
  {
    VLOG_I("[4] Serialize / Deserialize");

    vlink::zerocopy::PointCloud original;
    original.create_v3f(1000);
    original.header.seq = 99;
    std::strncpy(original.header.frame_id, "lidar_0", sizeof(original.header.frame_id) - 1);
    original.header.frame_id[sizeof(original.header.frame_id) - 1] = '\0';

    for (int i = 0; i < 100; ++i) {
      auto fi = static_cast<float>(i);
      original.push_value_v3f(fi, fi * 2, fi * 3);
    }

    vlink::Bytes wire;
    original >> wire;
    VLOG_I("  points=", original.size(), " serialized=", wire.size(), " bytes");
    VLOG_I("  check_valid=", vlink::zerocopy::PointCloud::check_valid(wire));

    vlink::zerocopy::PointCloud restored;
    restored << wire;
    VLOG_I("  restored size=", restored.size(), " seq=", restored.header.seq, " is_owner=", restored.is_owner());

    auto v = restored.get_value_v3f(50);
    VLOG_I("  point[50] = (", v.x, ", ", v.y, ", ", v.z, ")");
  }

  // ---- Section 5: resize + set_value ----
  // resize() adjusts the logical point count without reallocating (subject
  // to capacity). set_value_v3f writes at an arbitrary index -- useful for
  // patching sparse clouds without rebuilding.
  {
    VLOG_I("[5] resize() + set_value()");

    vlink::zerocopy::PointCloud pc;
    pc.create<float, float, float>(10, {"x", "y", "z"});
    pc.resize(5);

    pc.set_value_v3f(0, 100.0F, 200.0F, 300.0F);
    pc.set_value_v3f(4, 400.0F, 500.0F, 600.0F);

    auto v0 = pc.get_value_v3f(0);
    auto v4 = pc.get_value_v3f(4);
    VLOG_I("  point[0] = (", v0.x, ", ", v0.y, ", ", v0.z, ")");
    VLOG_I("  point[4] = (", v4.x, ", ", v4.y, ", ", v4.z, ")");
  }

  // ---- Section 6: pub/sub with PointCloud ----
  // End-to-end demo. Listener fires on `loop`'s thread; pc is a non-
  // owning view in shm:// (this example uses dds:// for portability).
  {
    VLOG_I("[6] Pub/Sub with PointCloud");

    vlink::MessageLoop loop;
    loop.set_name("pc_loop");
    loop.async_run();

    int received = 0;
    vlink::Subscriber<vlink::zerocopy::PointCloud> sub("dds://zerocopy/pointcloud");
    sub.attach(&loop);
    sub.listen([&received](const vlink::zerocopy::PointCloud& pc) {
      received++;
      VLOG_I("  [Sub] seq=", pc.header.seq, " points=", pc.size(), " pack=", pc.pack_size());
    });

    vlink::Publisher<vlink::zerocopy::PointCloud> pub("dds://zerocopy/pointcloud");
    pub.wait_for_subscribers();

    for (uint32_t i = 1; i <= 3; ++i) {
      vlink::zerocopy::PointCloud pc;
      pc.create_v3f(100);
      pc.header.seq = i;

      for (int j = 0; j < 10; ++j) {
        auto fj = static_cast<float>(j);
        pc.push_value_v3f(fj, fj, fj);
      }

      pub.publish(pc);
    }

    loop.wait_for_idle(1000);
    VLOG_I("  total received: ", received);

    loop.quit();
    loop.wait_for_quit();
  }

  // ---- Section 7: double-precision variant ----
  // create_v3d uses double instead of float for XYZ -- pack_size doubles
  // (3 * 8 = 24 bytes/point). Use for survey-grade lidar where float
  // precision (~7 digits) is insufficient.
  {
    VLOG_I("[7] Double-Precision (create_v3d)");

    vlink::zerocopy::PointCloud pc;
    pc.create_v3d(100);
    pc.push_value_v3d(1.111111, 2.222222, 3.333333);
    pc.push_value_v3d(4.444444, 5.555555, 6.666666);

    auto v = pc.get_value_v3d(0);
    VLOG_I("  pack_size=", pc.pack_size(), " point[0]=(", v.x, ", ", v.y, ", ", v.z, ")");
  }

  // ---- Section 8: optional compression (extent quantize + vertical layout) ----
  // create(..., extent, vertical) trades fidelity for footprint.
  //   * extent > 0: the maximum absolute coordinate magnitude. XYZ are quantised
  //     with Quantize::encode<int16_t>(extent, value), equivalent to
  //     int16 = round(v * 32767 / extent), instead of float -- pack_size shrinks
  //     (3*4 -> 3*2 for plain XYZ). The v3f/v3d
  //     getters dequantise transparently; resolution is extent / 32767. Points
  //     with any XYZ outside (-extent, +extent) are discarded (push/set returns
  //     false) rather than saturated.
  //   * vertical: the serialized payload is reordered into structure-of-arrays
  //     column order (xx..x yy..y zz..z), which packs better under a downstream
  //     entropy coder; operator<< restores the interleaved layout.
  {
    VLOG_I("[8] Compression (create extent + vertical)");

    vlink::zerocopy::PointCloud pc;
    pc.create_v3f<float>(1000, {"intensity"}, /*extent=*/10, /*vertical=*/true);
    VLOG_I("  pack_size=", pc.pack_size(), " bytes/point (3*2 + 1*4 = 10)");

    pc.push_value_v3f(1.234F, 5.678F, 9.012F, 0.5F);
    pc.push_value_v3f(-8.5F, 0.01F, 9.999F, 0.8F);

    vlink::Bytes wire;
    pc >> wire;
    VLOG_I("  points=", pc.size(), " serialized=", wire.size(), " bytes");

    vlink::zerocopy::PointCloud rx;
    rx << wire;
    auto v = rx.get_value_v3f(0);
    VLOG_I("  restored point[0] = (", v.x, ", ", v.y, ", ", v.z, ") extent=", static_cast<int>(rx.get_extent()),
           " vertical=", rx.get_vertical());
  }

  VLOG_I("PointCloud example complete.");
  return 0;
}
