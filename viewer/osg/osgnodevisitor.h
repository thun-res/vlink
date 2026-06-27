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

#pragma once

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/NodeVisitor>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <iostream>
#include <string>

class OsgNameNodeVisitor : public osg::NodeVisitor {
 public:
  OsgNameNodeVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
  osg::ref_ptr<osg::Node> getNode(const osg::ref_ptr<osg::Node>& node, const std::string& name) {
    m_name = name;
    this->apply(*node);
    return m_node;
  }

 protected:
  void apply(osg::Node& node) override {
    if (node.getName() == m_name) {
      m_node = &node;
      return;
    }

    traverse(node);
  }

 private:
  std::string m_name;
  osg::ref_ptr<osg::Node> m_node = nullptr;
};

template <typename TARGET, typename BASE>
class OsgAnimationNodeVisitor : public osg::NodeVisitor {
 public:
  OsgAnimationNodeVisitor() : osg::NodeVisitor(TRAVERSE_NONE) {}
  osg::ref_ptr<TARGET> getNode(const osg::ref_ptr<osg::Node>& node) {
    osg::ref_ptr<TARGET> manager;
    this->apply(*node);

    if (m_node.valid()) {
      manager = new TARGET(*m_node);
      node->setUpdateCallback(manager);
    }

    return manager;
  }

 protected:
  void apply(osg::Node& node) override {
    if (node.getUpdateCallback()) {
      m_node = dynamic_cast<BASE*>(node.getUpdateCallback());
      return;
    }

    traverse(node);
  }

 private:
  osg::ref_ptr<BASE> m_node{nullptr};
};

#endif

// NOLINTEND
