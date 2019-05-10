// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/gap/fake_pairing_delegate.h"

#include "gtest/gtest.h"

namespace bt {
namespace gap {

FakePairingDelegate::FakePairingDelegate(sm::IOCapability io_capability)
    : io_capability_(io_capability),
      complete_pairing_count_(0),
      confirm_pairing_count_(0),
      display_passkey_count_(0),
      request_passkey_count_(0),
      weak_ptr_factory_(this) {}

FakePairingDelegate::~FakePairingDelegate() {
  if (complete_pairing_cb_ && complete_pairing_count_ == 0) {
    ADD_FAILURE() << "Expected CompletePairing never called";
  }
  if (confirm_pairing_cb_ && confirm_pairing_count_ == 0) {
    ADD_FAILURE() << "Expected ConfirmPairing never called";
  }
  if (display_passkey_cb_ && display_passkey_count_ == 0) {
    ADD_FAILURE() << "Expected DisplayPasskey never called";
  }
  if (request_passkey_cb_ && request_passkey_count_ == 0) {
    ADD_FAILURE() << "Expected RequestPasskey never called";
  }
}

void FakePairingDelegate::CompletePairing(PeerId peer_id, sm::Status status) {
  if (complete_pairing_cb_) {
    complete_pairing_cb_(peer_id, status);
    complete_pairing_count_++;
  } else {
    ADD_FAILURE() << "Unexpected call: " << __func__ << "("
                  << peer_id.ToString() << ", " << status.ToString() << ")";
  }
}

void FakePairingDelegate::ConfirmPairing(PeerId peer_id,
                                         ConfirmCallback confirm) {
  if (confirm_pairing_cb_) {
    confirm_pairing_cb_(peer_id, std::move(confirm));
    confirm_pairing_count_++;
  } else {
    ADD_FAILURE() << "Unexpected call: " << __func__ << "("
                  << peer_id.ToString() << ", ...)";
  }
}

void FakePairingDelegate::DisplayPasskey(PeerId peer_id, uint32_t passkey,
                                         bool local_consent,
                                         ConfirmCallback confirm) {
  if (display_passkey_cb_) {
    display_passkey_cb_(peer_id, passkey, local_consent, std::move(confirm));
    display_passkey_count_++;
  } else {
    ADD_FAILURE() << "Unexpected call: " << __func__ << "("
                  << peer_id.ToString() << ", " << passkey << ", "
                  << local_consent << ", ...)";
  }
}

void FakePairingDelegate::RequestPasskey(PeerId peer_id,
                                         PasskeyResponseCallback respond) {
  if (request_passkey_cb_) {
    request_passkey_cb_(peer_id, std::move(respond));
    request_passkey_count_++;
  } else {
    ADD_FAILURE() << "Unexpected call: " << __func__ << "("
                  << peer_id.ToString() << ", ...)";
  }
}

}  // namespace gap
}  // namespace bt
