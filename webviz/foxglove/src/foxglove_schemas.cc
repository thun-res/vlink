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

#include <ArrowPrimitive_bfbs.fbs.hpp>
#include <ByteVector_bfbs.fbs.hpp>
#include <CameraCalibration_bfbs.fbs.hpp>
#include <CircleAnnotation_bfbs.fbs.hpp>
#include <Color_bfbs.fbs.hpp>
#include <CompressedAudio_bfbs.fbs.hpp>
#include <CompressedImage_bfbs.fbs.hpp>
#include <CompressedPointCloud_bfbs.fbs.hpp>
#include <CompressedVideo_bfbs.fbs.hpp>
#include <CubePrimitive_bfbs.fbs.hpp>
#include <CylinderPrimitive_bfbs.fbs.hpp>
#include <Event_bfbs.fbs.hpp>
#include <FrameTransform_bfbs.fbs.hpp>
#include <FrameTransforms_bfbs.fbs.hpp>
#include <GeoJSON_bfbs.fbs.hpp>
#include <Grid_bfbs.fbs.hpp>
#include <ImageAnnotations_bfbs.fbs.hpp>
#include <JointState_bfbs.fbs.hpp>
#include <JointStates_bfbs.fbs.hpp>
#include <KeyValuePair_bfbs.fbs.hpp>
#include <LaserScan_bfbs.fbs.hpp>
#include <LinePrimitive_bfbs.fbs.hpp>
#include <LocationFix_bfbs.fbs.hpp>
#include <LocationFixes_bfbs.fbs.hpp>
#include <Log_bfbs.fbs.hpp>
#include <ModelPrimitive_bfbs.fbs.hpp>
#include <Odometry_bfbs.fbs.hpp>
#include <PackedElementField_bfbs.fbs.hpp>
#include <Point2_bfbs.fbs.hpp>
#include <Point3InFrame_bfbs.fbs.hpp>
#include <Point3_bfbs.fbs.hpp>
#include <PointCloud_bfbs.fbs.hpp>
#include <PointsAnnotation_bfbs.fbs.hpp>
#include <PoseInFrame_bfbs.fbs.hpp>
#include <Pose_bfbs.fbs.hpp>
#include <PosesInFrame_bfbs.fbs.hpp>
#include <Quaternion_bfbs.fbs.hpp>
#include <RawAudio_bfbs.fbs.hpp>
#include <RawImage_bfbs.fbs.hpp>
#include <SceneEntityDeletion_bfbs.fbs.hpp>
#include <SceneEntity_bfbs.fbs.hpp>
#include <SceneUpdate_bfbs.fbs.hpp>
#include <SpherePrimitive_bfbs.fbs.hpp>
#include <TextAnnotation_bfbs.fbs.hpp>
#include <TextPrimitive_bfbs.fbs.hpp>
#include <TriangleListPrimitive_bfbs.fbs.hpp>
#include <Vector2_bfbs.fbs.hpp>
#include <Vector3_bfbs.fbs.hpp>
#include <VoxelGrid_bfbs.fbs.hpp>

#include "./foxglove_writer.h"

namespace vlink {
namespace webviz {

std::string_view foxglove_schema(std::string_view name) {
  if (name == "foxglove.ArrowPrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::ArrowPrimitiveBinarySchema::data()),
            ::foxglove::ArrowPrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.ByteVector") {
    return {reinterpret_cast<const char*>(::foxglove::ByteVectorBinarySchema::data()),
            ::foxglove::ByteVectorBinarySchema::size()};
  }

  if (name == "foxglove.CameraCalibration") {
    return {reinterpret_cast<const char*>(::foxglove::CameraCalibrationBinarySchema::data()),
            ::foxglove::CameraCalibrationBinarySchema::size()};
  }

  if (name == "foxglove.CircleAnnotation") {
    return {reinterpret_cast<const char*>(::foxglove::CircleAnnotationBinarySchema::data()),
            ::foxglove::CircleAnnotationBinarySchema::size()};
  }

  if (name == "foxglove.Color") {
    return {reinterpret_cast<const char*>(::foxglove::ColorBinarySchema::data()),
            ::foxglove::ColorBinarySchema::size()};
  }

  if (name == "foxglove.CompressedAudio") {
    return {reinterpret_cast<const char*>(::foxglove::CompressedAudioBinarySchema::data()),
            ::foxglove::CompressedAudioBinarySchema::size()};
  }

  if (name == "foxglove.CompressedImage") {
    return {reinterpret_cast<const char*>(::foxglove::CompressedImageBinarySchema::data()),
            ::foxglove::CompressedImageBinarySchema::size()};
  }

  if (name == "foxglove.CompressedPointCloud") {
    return {reinterpret_cast<const char*>(::foxglove::CompressedPointCloudBinarySchema::data()),
            ::foxglove::CompressedPointCloudBinarySchema::size()};
  }

  if (name == "foxglove.CompressedVideo") {
    return {reinterpret_cast<const char*>(::foxglove::CompressedVideoBinarySchema::data()),
            ::foxglove::CompressedVideoBinarySchema::size()};
  }

  if (name == "foxglove.CubePrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::CubePrimitiveBinarySchema::data()),
            ::foxglove::CubePrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.CylinderPrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::CylinderPrimitiveBinarySchema::data()),
            ::foxglove::CylinderPrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.Event") {
    return {reinterpret_cast<const char*>(::foxglove::EventBinarySchema::data()),
            ::foxglove::EventBinarySchema::size()};
  }

  if (name == "foxglove.FrameTransform") {
    return {reinterpret_cast<const char*>(::foxglove::FrameTransformBinarySchema::data()),
            ::foxglove::FrameTransformBinarySchema::size()};
  }

  if (name == "foxglove.FrameTransforms") {
    return {reinterpret_cast<const char*>(::foxglove::FrameTransformsBinarySchema::data()),
            ::foxglove::FrameTransformsBinarySchema::size()};
  }

  if (name == "foxglove.GeoJSON") {
    return {reinterpret_cast<const char*>(::foxglove::GeoJSONBinarySchema::data()),
            ::foxglove::GeoJSONBinarySchema::size()};
  }

  if (name == "foxglove.Grid") {
    return {reinterpret_cast<const char*>(::foxglove::GridBinarySchema::data()), ::foxglove::GridBinarySchema::size()};
  }

  if (name == "foxglove.ImageAnnotations") {
    return {reinterpret_cast<const char*>(::foxglove::ImageAnnotationsBinarySchema::data()),
            ::foxglove::ImageAnnotationsBinarySchema::size()};
  }

  if (name == "foxglove.JointState") {
    return {reinterpret_cast<const char*>(::foxglove::JointStateBinarySchema::data()),
            ::foxglove::JointStateBinarySchema::size()};
  }

  if (name == "foxglove.JointStates") {
    return {reinterpret_cast<const char*>(::foxglove::JointStatesBinarySchema::data()),
            ::foxglove::JointStatesBinarySchema::size()};
  }

  if (name == "foxglove.KeyValuePair") {
    return {reinterpret_cast<const char*>(::foxglove::KeyValuePairBinarySchema::data()),
            ::foxglove::KeyValuePairBinarySchema::size()};
  }

  if (name == "foxglove.LaserScan") {
    return {reinterpret_cast<const char*>(::foxglove::LaserScanBinarySchema::data()),
            ::foxglove::LaserScanBinarySchema::size()};
  }

  if (name == "foxglove.LinePrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::LinePrimitiveBinarySchema::data()),
            ::foxglove::LinePrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.LocationFix") {
    return {reinterpret_cast<const char*>(::foxglove::LocationFixBinarySchema::data()),
            ::foxglove::LocationFixBinarySchema::size()};
  }

  if (name == "foxglove.LocationFixes") {
    return {reinterpret_cast<const char*>(::foxglove::LocationFixesBinarySchema::data()),
            ::foxglove::LocationFixesBinarySchema::size()};
  }

  if (name == "foxglove.Log") {
    return {reinterpret_cast<const char*>(::foxglove::LogBinarySchema::data()), ::foxglove::LogBinarySchema::size()};
  }

  if (name == "foxglove.ModelPrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::ModelPrimitiveBinarySchema::data()),
            ::foxglove::ModelPrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.Odometry") {
    return {reinterpret_cast<const char*>(::foxglove::OdometryBinarySchema::data()),
            ::foxglove::OdometryBinarySchema::size()};
  }

  if (name == "foxglove.PackedElementField") {
    return {reinterpret_cast<const char*>(::foxglove::PackedElementFieldBinarySchema::data()),
            ::foxglove::PackedElementFieldBinarySchema::size()};
  }

  if (name == "foxglove.Point2") {
    return {reinterpret_cast<const char*>(::foxglove::Point2BinarySchema::data()),
            ::foxglove::Point2BinarySchema::size()};
  }

  if (name == "foxglove.Point3") {
    return {reinterpret_cast<const char*>(::foxglove::Point3BinarySchema::data()),
            ::foxglove::Point3BinarySchema::size()};
  }

  if (name == "foxglove.Point3InFrame") {
    return {reinterpret_cast<const char*>(::foxglove::Point3InFrameBinarySchema::data()),
            ::foxglove::Point3InFrameBinarySchema::size()};
  }

  if (name == "foxglove.PointCloud") {
    return {reinterpret_cast<const char*>(::foxglove::PointCloudBinarySchema::data()),
            ::foxglove::PointCloudBinarySchema::size()};
  }

  if (name == "foxglove.PointsAnnotation") {
    return {reinterpret_cast<const char*>(::foxglove::PointsAnnotationBinarySchema::data()),
            ::foxglove::PointsAnnotationBinarySchema::size()};
  }

  if (name == "foxglove.Pose") {
    return {reinterpret_cast<const char*>(::foxglove::PoseBinarySchema::data()), ::foxglove::PoseBinarySchema::size()};
  }

  if (name == "foxglove.PoseInFrame") {
    return {reinterpret_cast<const char*>(::foxglove::PoseInFrameBinarySchema::data()),
            ::foxglove::PoseInFrameBinarySchema::size()};
  }

  if (name == "foxglove.PosesInFrame") {
    return {reinterpret_cast<const char*>(::foxglove::PosesInFrameBinarySchema::data()),
            ::foxglove::PosesInFrameBinarySchema::size()};
  }

  if (name == "foxglove.Quaternion") {
    return {reinterpret_cast<const char*>(::foxglove::QuaternionBinarySchema::data()),
            ::foxglove::QuaternionBinarySchema::size()};
  }

  if (name == "foxglove.RawAudio") {
    return {reinterpret_cast<const char*>(::foxglove::RawAudioBinarySchema::data()),
            ::foxglove::RawAudioBinarySchema::size()};
  }

  if (name == "foxglove.RawImage") {
    return {reinterpret_cast<const char*>(::foxglove::RawImageBinarySchema::data()),
            ::foxglove::RawImageBinarySchema::size()};
  }

  if (name == "foxglove.SceneEntity") {
    return {reinterpret_cast<const char*>(::foxglove::SceneEntityBinarySchema::data()),
            ::foxglove::SceneEntityBinarySchema::size()};
  }

  if (name == "foxglove.SceneEntityDeletion") {
    return {reinterpret_cast<const char*>(::foxglove::SceneEntityDeletionBinarySchema::data()),
            ::foxglove::SceneEntityDeletionBinarySchema::size()};
  }

  if (name == "foxglove.SceneUpdate") {
    return {reinterpret_cast<const char*>(::foxglove::SceneUpdateBinarySchema::data()),
            ::foxglove::SceneUpdateBinarySchema::size()};
  }

  if (name == "foxglove.SpherePrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::SpherePrimitiveBinarySchema::data()),
            ::foxglove::SpherePrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.TextAnnotation") {
    return {reinterpret_cast<const char*>(::foxglove::TextAnnotationBinarySchema::data()),
            ::foxglove::TextAnnotationBinarySchema::size()};
  }

  if (name == "foxglove.TextPrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::TextPrimitiveBinarySchema::data()),
            ::foxglove::TextPrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.TriangleListPrimitive") {
    return {reinterpret_cast<const char*>(::foxglove::TriangleListPrimitiveBinarySchema::data()),
            ::foxglove::TriangleListPrimitiveBinarySchema::size()};
  }

  if (name == "foxglove.Vector2") {
    return {reinterpret_cast<const char*>(::foxglove::Vector2BinarySchema::data()),
            ::foxglove::Vector2BinarySchema::size()};
  }

  if (name == "foxglove.Vector3") {
    return {reinterpret_cast<const char*>(::foxglove::Vector3BinarySchema::data()),
            ::foxglove::Vector3BinarySchema::size()};
  }

  if (name == "foxglove.VoxelGrid") {
    return {reinterpret_cast<const char*>(::foxglove::VoxelGridBinarySchema::data()),
            ::foxglove::VoxelGridBinarySchema::size()};
  }

  return {};
}

}  // namespace webviz
}  // namespace vlink
