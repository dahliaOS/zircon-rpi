// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_listener.h"

#include <lib/syslog/cpp/macros.h>

namespace accessibility_test {

void MockSemanticListener::Bind(
    fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticListener> *listener) {
  semantic_listener_bindings_.AddBinding(this, listener->NewRequest());
}

void MockSemanticListener::SetHitTestResult(std::optional<uint32_t> node_id) {
  hit_test_node_id_ = node_id;
}

void MockSemanticListener::OnAccessibilityActionRequested(
    uint32_t node_id, fuchsia::accessibility::semantics::Action action,
    fuchsia::accessibility::semantics::SemanticListener::OnAccessibilityActionRequestedCallback
        callback) {
  received_action_ = action;
  action_node_id_ = node_id;
  callback(true);
}

void MockSemanticListener::HitTest(::fuchsia::math::PointF local_point, HitTestCallback callback) {
  fuchsia::accessibility::semantics::Hit hit;
  if (hit_test_node_id_) {
    hit.set_node_id(*hit_test_node_id_);
    hit.mutable_path_from_root()->push_back(*hit_test_node_id_);
  }
  callback(std::move(hit));
}

void MockSemanticListener::OnSemanticsModeChanged(bool update_enabled,
                                                  OnSemanticsModeChangedCallback callback) {
  semantics_enabled_ = update_enabled;
  callback();
}

void MockSemanticListener::SetRequestedAction(fuchsia::accessibility::semantics::Action action) {
  received_action_ = action;
}

fuchsia::accessibility::semantics::Action MockSemanticListener::GetRequestedAction() const {
  return received_action_;
}

uint32_t MockSemanticListener::GetRequestedActionNodeId() const { return action_node_id_; }

}  // namespace accessibility_test
