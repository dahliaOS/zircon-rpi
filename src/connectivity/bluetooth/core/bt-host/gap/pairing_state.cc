// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/gap/pairing_state.h"

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"

namespace bt {
namespace gap {

using hci::IOCapability;

PairingState::PairingState(PeerId peer_id, hci::Connection* link,
                           StatusCallback status_cb)
    : peer_id_(peer_id),
      link_(link),
      state_(State::kIdle),
      status_callback_(std::move(status_cb)) {
  ZX_ASSERT(link_);
  ZX_ASSERT(link_->ll_type() != hci::Connection::LinkType::kLE);
  ZX_ASSERT(status_callback_);
  link_->set_encryption_change_callback(
      fit::bind_member(this, &PairingState::OnEncryptionChange));
}

PairingState::InitiatorAction PairingState::InitiatePairing(
    StatusCallback status_cb) {
  if (!pairing_delegate()) {
    bt_log(TRACE, "gap-bredr",
           "No pairing delegate for link %#.04x (id: %s); not pairing",
           handle(), bt_str(peer_id()));
    status_cb(handle(), hci::Status(HostError::kNotReady));
    return InitiatorAction::kDoNotSendAuthenticationRequest;
  }

  if (state() == State::kIdle) {
    ZX_ASSERT(!is_pairing());
    current_pairing_ = Data();
    current_pairing_->initiator = true;
    current_pairing_->initiator_callbacks.push_back(std::move(status_cb));
    bt_log(TRACE, "gap-bredr", "Initiating pairing on %#.04x (id %s)", handle(),
           bt_str(peer_id()));
    state_ = State::kInitiatorPairingStarted;
    return InitiatorAction::kSendAuthenticationRequest;
  }

  if (is_pairing()) {
    bt_log(TRACE, "gap-bredr",
           "Already pairing %#.04x (id: %s); blocking callback on completion",
           handle(), bt_str(peer_id()));
    current_pairing_->initiator_callbacks.push_back(std::move(status_cb));
  }

  return InitiatorAction::kDoNotSendAuthenticationRequest;
}

std::optional<hci::IOCapability> PairingState::OnIoCapabilityRequest() {
  if (state() == State::kInitiatorPairingStarted) {
    ZX_ASSERT(initiator());
    state_ = State::kInitiatorWaitIoCapResponse;
  } else if (state() == State::kResponderWaitIoCapRequest) {
    ZX_ASSERT(is_pairing());
    ZX_ASSERT(!initiator());

    if (!pairing_delegate()) {
      bt_log(TRACE, "gap-bredr",
             "No pairing delegate for link %#.04x (id: %s); not pairing",
             handle(), bt_str(peer_id()));
      Reset();
      return std::nullopt;
    }

    // TODO(xow): Compute pairing event to wait for.
    state_ = State::kWaitPairingEvent;
  } else {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    return std::nullopt;
  }

  // TODO(xow): Return local IO Capability.
  ZX_ASSERT(pairing_delegate());
  return hci::IOCapability::kNoInputNoOutput;
}

void PairingState::OnIoCapabilityResponse(hci::IOCapability peer_iocap) {
  // TODO(xow): Store peer IO Capability.
  if (state() == State::kIdle) {
    ZX_ASSERT(!is_pairing());
    current_pairing_ = Data();
    current_pairing_->initiator = false;
    state_ = State::kResponderWaitIoCapRequest;
  } else if (state() == State::kInitiatorWaitIoCapResponse) {
    ZX_ASSERT(initiator());
    state_ = State::kWaitPairingEvent;

    // TODO(xow): Compute pairing event to wait for.
  } else {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
  }
}

void PairingState::OnUserConfirmationRequest(uint32_t numeric_value,
                                             UserConfirmationCallback cb) {
  if (state() != State::kWaitPairingEvent) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    cb(false);
    return;
  }
  ZX_ASSERT(is_pairing());

  // TODO(xow): Return actual user response.
  state_ = State::kWaitPairingComplete;
  cb(true);
}

void PairingState::OnUserPasskeyRequest(UserPasskeyCallback cb) {
  if (state() != State::kWaitPairingEvent) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    cb(std::nullopt);
    return;
  }
  ZX_ASSERT(is_pairing());

  // TODO(xow): Return actual user response.
  state_ = State::kWaitPairingComplete;
  cb(0);
}

void PairingState::OnUserPasskeyNotification(uint32_t numeric_value) {
  if (state() != State::kWaitPairingEvent) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    return;
  }
  ZX_ASSERT(is_pairing());

  // TODO(xow): Display passkey to user.
  state_ = State::kWaitPairingComplete;
}

void PairingState::OnSimplePairingComplete(hci::StatusCode status_code) {
  if (state() != State::kWaitPairingComplete) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    return;
  }
  ZX_ASSERT(is_pairing());

  if (const hci::Status status(status_code); bt_is_error(
          status, INFO, "gap-bredr", "Pairing failed on link %#.04x (id: %s)",
          handle(), bt_str(peer_id()))) {
    SignalStatus(status);
    state_ = State::kFailed;
    return;
  }

  state_ = State::kWaitLinkKey;
}

void PairingState::OnLinkKeyNotification(const UInt128& link_key,
                                         hci::LinkKeyType key_type) {
  if (state() != State::kWaitLinkKey) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    return;
  }
  ZX_ASSERT(is_pairing());

  // TODO(xow): Store link key.
  if (initiator()) {
    state_ = State::kInitiatorWaitAuthComplete;
  } else {
    EnableEncryption();
  }
}

void PairingState::OnAuthenticationComplete(hci::StatusCode status_code) {
  if (state() != State::kInitiatorPairingStarted &&
      state() != State::kInitiatorWaitAuthComplete) {
    bt_log(ERROR, "gap-bredr",
           "%#.04x (id: %s): Unexpected event %s while in state \"%s\"",
           handle(), bt_str(peer_id()), __func__, ToString(state()));
    FailWithUnexpectedEvent();
    return;
  }
  ZX_ASSERT(initiator());

  if (const hci::Status status(status_code);
      bt_is_error(status, INFO, "gap-bredr",
                  "Authentication failed on link %#.04x (id: %s)", handle(),
                  bt_str(peer_id()))) {
    SignalStatus(status);
    state_ = State::kFailed;
    return;
  }

  EnableEncryption();
}

void PairingState::OnEncryptionChange(hci::Status status, bool enabled) {
  if (state() != State::kWaitEncryption) {
    bt_log(INFO, "gap-bredr",
           "%#.04x (id: %s): Ignoring %s(%s, %s) in state \"%s\", before "
           "pairing completed",
           handle(), bt_str(peer_id()), __func__, bt_str(status),
           enabled ? "true" : "false", ToString(state()));
    return;
  }

  if (status && !enabled) {
    // With Secure Connections, encryption should never be disabled (v5.0 Vol 2,
    // Part E, Sec 7.1.16) at all.
    bt_log(WARN, "gap-bredr",
           "Pairing failed due to encryption disable on link %#.04x (id: %s)",
           handle(), bt_str(peer_id()));
    status = hci::Status(HostError::kFailed);
  }

  // TODO(xow): Write link key to Connection::ltk to register new security
  //            properties.
  SignalStatus(status);

  // Perform state transition.
  if (status) {
    Reset();
  } else {
    state_ = State::kFailed;
  }
}

const char* PairingState::ToString(PairingState::State state) {
  switch (state) {
    case State::kIdle:
      return "Idle";
    case State::kInitiatorPairingStarted:
      return "InitiatorPairingStarted";
    case State::kInitiatorWaitIoCapResponse:
      return "InitiatorWaitIoCapResponse";
    case State::kResponderWaitIoCapRequest:
      return "ResponderWaitIoCapRequest";
    case State::kWaitPairingEvent:
      return "WaitPairingEvent";
    case State::kWaitPairingComplete:
      return "WaitPairingComplete";
    case State::kWaitLinkKey:
      return "WaitLinkKey";
    case State::kInitiatorWaitAuthComplete:
      return "InitiatorWaitAuthComplete";
    case State::kWaitEncryption:
      return "WaitEncryption";
    case State::kFailed:
      return "Failed";
    default:
      break;
  }
  return "";
}

void PairingState::SignalStatus(hci::Status status) {
  bt_log(SPEW, "gap-bredr",
         "Signaling pairing listeners for %#.04x (id: %s) with %s", handle(),
         bt_str(peer_id()), bt_str(status));
  status_callback_(handle(), status);
  if (is_pairing()) {
    for (auto& cb : current_pairing_->initiator_callbacks) {
      cb(handle(), status);
    }
  }
}

void PairingState::EnableEncryption() {
  if (!link_->StartEncryption()) {
    FailWithUnexpectedEvent();
    return;
  }
  state_ = State::kWaitEncryption;
}

void PairingState::FailWithUnexpectedEvent() {
  SignalStatus(hci::Status(HostError::kNotSupported));
  current_pairing_ = std::nullopt;
  state_ = State::kFailed;
}

void PairingState::Reset() {
  state_ = State::kIdle;
  current_pairing_ = std::nullopt;
}

PairingAction GetInitiatorPairingAction(IOCapability initiator_cap,
                                        IOCapability responder_cap) {
  if (initiator_cap == IOCapability::kNoInputNoOutput) {
    return PairingAction::kAutomatic;
  }
  if (responder_cap == IOCapability::kNoInputNoOutput) {
    if (initiator_cap == IOCapability::kDisplayYesNo) {
      return PairingAction::kGetConsent;
    }
    return PairingAction::kAutomatic;
  }
  if (initiator_cap == IOCapability::kKeyboardOnly) {
    return PairingAction::kRequestPasskey;
  }
  if (responder_cap == IOCapability::kDisplayOnly) {
    if (initiator_cap == IOCapability::kDisplayYesNo) {
      return PairingAction::kComparePasskey;
    }
    return PairingAction::kAutomatic;
  }
  return PairingAction::kDisplayPasskey;
}

PairingAction GetResponderPairingAction(IOCapability initiator_cap,
                                        IOCapability responder_cap) {
  if (initiator_cap == IOCapability::kNoInputNoOutput &&
      responder_cap == IOCapability::kKeyboardOnly) {
    return PairingAction::kGetConsent;
  }
  if (initiator_cap == IOCapability::kDisplayYesNo &&
      responder_cap == IOCapability::kDisplayYesNo) {
    return PairingAction::kComparePasskey;
  }
  return GetInitiatorPairingAction(responder_cap, initiator_cap);
}

hci::EventCode GetExpectedEvent(IOCapability local_cap, IOCapability peer_cap) {
  if (local_cap == IOCapability::kNoInputNoOutput ||
      peer_cap == IOCapability::kNoInputNoOutput) {
    return hci::kUserConfirmationRequestEventCode;
  }
  if (local_cap == IOCapability::kKeyboardOnly) {
    return hci::kUserPasskeyRequestEventCode;
  }
  if (peer_cap == IOCapability::kKeyboardOnly) {
    return hci::kUserPasskeyNotificationEventCode;
  }
  return hci::kUserConfirmationRequestEventCode;
}

bool IsPairingAuthenticated(IOCapability local_cap, IOCapability peer_cap) {
  if (local_cap == IOCapability::kNoInputNoOutput ||
      peer_cap == IOCapability::kNoInputNoOutput) {
    return false;
  }
  if (local_cap == IOCapability::kDisplayYesNo &&
      peer_cap == IOCapability::kDisplayYesNo) {
    return true;
  }
  if (local_cap == IOCapability::kKeyboardOnly ||
      peer_cap == IOCapability::kKeyboardOnly) {
    return true;
  }
  return false;
}

}  // namespace gap
}  // namespace bt
