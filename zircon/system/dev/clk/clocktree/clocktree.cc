// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dev/clk/clocktree/clocktree.h>
#include <stdio.h>

namespace clk {

namespace {

bool IsError(const zx_status_t st) {
    // A clock may or may not choose to implement any of the core clock operations.
    // If an operation is not implemented the method must return ZX_ERR_NOT_SUPPORTED
    // which is not considered an error per se.
    return st != ZX_OK && st != ZX_ERR_NOT_SUPPORTED;
}

}  // namespace

zx_status_t ClockTree::Enable(const uint32_t id) {
    if (id == kClkNoParent) { return ZX_OK; }  // At the root.
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    BaseClock* self = clocks_[id];
    const uint32_t parent_id = self->ParentId();

    zx_status_t st = Enable(parent_id);
    if (IsError(st)) {
        // Something went wrong, unwind.
        Disable(parent_id);
        return st;
    }

    return self->EnableInternal();
}

zx_status_t ClockTree::Disable(const uint32_t id) {
    if (id == kClkNoParent) { return ZX_OK; }  // At the root.
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    BaseClock* self = clocks_[id];
    const uint32_t parent_id = self->ParentId();

    // Disable this clock and then disable its parent. Don't try to unwind if
    // disable fails.
    zx_status_t self_st = self->DisableInternal();
    zx_status_t parent_st = Disable(parent_id);

    // If this clock fails to disable and a clock somewhere in the parent chain
    // fails to disable, return the error caused by the clock closest to the
    // caller (i.e. this clock).
    if (IsError(self_st)) { return self_st; }
    if (IsError(parent_st)) { return parent_st; }

    return ZX_OK;
}

zx_status_t ClockTree::IsEnabled(const uint32_t id, bool* out) {
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    return clocks_[id]->IsEnabled(out);
}

zx_status_t ClockTree::SetRate(const uint32_t id, const Hertz rate) {
    return ZX_ERR_NOT_SUPPORTED;
}
zx_status_t ClockTree::QuerySupportedRate(const uint32_t id, const Hertz max) {
    return ZX_ERR_NOT_SUPPORTED;
}
zx_status_t ClockTree::GetRate(const uint32_t id, Hertz* out) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t ClockTree::SetInput(const uint32_t id, const uint32_t input_index) {
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    BaseClock* self = clocks_[id];

    return self->SetInput(input_index);
}
zx_status_t ClockTree::GetNumInputs(const uint32_t id, uint32_t* out) {
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    BaseClock* self = clocks_[id];

    return self->GetNumInputs(out);
}
zx_status_t ClockTree::GetInput(const uint32_t id, uint32_t* out) {
    if (!InRange(id)) { return ZX_ERR_OUT_OF_RANGE; }

    BaseClock* self = clocks_[id];

    return self->GetInput(out);
}

// Helper functions
bool ClockTree::InRange(const uint32_t index) const {
    return index < count_;
}

}  // namespace clk
