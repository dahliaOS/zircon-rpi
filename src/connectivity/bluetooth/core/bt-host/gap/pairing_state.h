// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_PAIRING_STATE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_PAIRING_STATE_H_

#include <fbl/macros.h>

#include <optional>
#include <vector>

#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"

namespace bt {
namespace gap {

// Represents the local user interaction that will occur, as inferred from Core
// Spec v5.0 Vol 3, Part C, Sec 5.2.2.6 (Table 5.7). This is not directly
// coupled to the reply action for the HCI "User" event for pairing; e.g.
// kDisplayPasskey may mean automatically confirming User Confirmation Request
// or displaying the value from User Passkey Notification.
enum class PairingAction {
  // Don't involve the user.
  kAutomatic,

  // Request yes/no consent.
  kGetConsent,

  // Display 6-digit value with "cancel."
  kDisplayPasskey,

  // Display 6-digit value with "yes/no."
  kComparePasskey,

  // Request a 6-digit value entry.
  kRequestPasskey,
};

// Tracks the pairing state of a peer's BR/EDR link. This drives HCI
// transactions and user interactions for pairing in order to obtain the highest
// possible level of link security given the capabilities of the controllers
// and hosts participating in the pairing.
//
// This implements Core Spec v5.0 Vol 2, Part F, Sec 4.2 through Sec 4.4, per
// logic requirements in Vol 3, Part C, Sec 5.2.2.
//
// This tracks both the bonded case (both hosts furnish their Link Keys to their
// controllers) and the unbonded case (both controllers perform Secure Simple
// Pairing and deliver the resulting Link Keys to their hosts).
//
// Pairing is considered complete when the Link Keys have been used to
// successfully encrypt the link, at which time pairing may be restarted (e.g.
// with different capabilities).
//
// This class is not thread-safe and should only be called on the thread on
// which it was created.
class PairingState final {
 public:
  // Used to report the status of a pairing procedure. |status| will contain
  // HostError::kNotSupported if the pairing procedure does not proceed in the
  // order of events expected.
  using StatusCallback =
      fit::function<void(hci::ConnectionHandle, hci::Status)>;

  // Constructs a PairingState for the ACL connection |link|. This object will
  // receive its "encryption change" callbacks.
  //
  // |link| must be valid for the lifetime of this object.
  PairingState(hci::Connection* link, StatusCallback status_cb);
  ~PairingState() = default;

  // True if there is currently a pairing procedure in progress that the local
  // device initiated.
  bool initiator() const {
    return is_pairing() ? current_pairing_->initiator : false;
  }

  // Starts pairing against the peer, if pairing is not already in progress.
  // If not, this device becomes the pairing initiator, and returns
  // |kSendAuthenticationRequest| to indicate that the caller shall send an
  // Authentication Request for this peer.
  //
  // When pairing completes or errors out, the |status_cb| of each call to this
  // function will be invoked with the result.
  enum class InitiatorAction {
    kDoNotSendAuthenticationRequest,
    kSendAuthenticationRequest,
  };
  [[nodiscard]] InitiatorAction InitiatePairing(StatusCallback status_cb);

  // Event handlers. Caller must ensure that the event is addressed to the link
  // for this PairingState.

  // Returns value for IO Capability Request Reply, else std::nullopt for IO
  // Capability Negative Reply.
  //
  // TODO(BT-8): Indicate presence of out-of-band (OOB) data.
  [[nodiscard]] std::optional<hci::IOCapability> OnIoCapabilityRequest();

  // Caller is not expected to send a response.
  void OnIoCapabilityResponse(hci::IOCapability peer_iocap);

  // |cb| is called with: true to send User Confirmation Request Reply, else
  // for to send User Confirmation Request Negative Reply.
  using UserConfirmationCallback = fit::callback<void(bool confirm)>;
  void OnUserConfirmationRequest(uint32_t numeric_value,
                                 UserConfirmationCallback cb);

  // |cb| is called with: passkey value to send User Passkey Request Reply, else
  // std::nullopt to send User Passkey Request Negative Reply.
  using UserPasskeyCallback =
      fit::callback<void(std::optional<uint32_t> passkey)>;
  void OnUserPasskeyRequest(UserPasskeyCallback cb);

  // Caller is not expected to send a response.
  void OnUserPasskeyNotification(uint32_t numeric_value);

  // Caller is not expected to send a response.
  void OnSimplePairingComplete(hci::StatusCode status_code);

  // Caller is not expected to send a response.
  void OnLinkKeyNotification(const UInt128& link_key,
                             hci::LinkKeyType key_type);

  // Caller is not expected to send a response.
  void OnAuthenticationComplete(hci::StatusCode status_code);

  // Handler for hci::Connection::set_encryption_change_callback.
  void OnEncryptionChange(hci::Status status, bool enabled);

 private:
  enum class State {
    // Wait for initiator's IO Capability Response or for locally-initiated
    // pairing.
    kIdle,

    // As initiator, wait for IO Capability Request or Authentication Complete.
    kInitiatorPairingStarted,

    // As initiator, wait for IO Capability Response.
    kInitiatorWaitIoCapResponse,

    // As responder, wait for IO Capability Request.
    kResponderWaitIoCapRequest,

    // Wait for controller event for pairing action.
    kWaitPairingEvent,

    // Wait for Simple Pairing Complete.
    kWaitPairingComplete,

    // Wait for Link Key Notification.
    kWaitLinkKey,

    // As initiator, wait for Authentication Complete.
    kInitiatorWaitAuthComplete,

    // Wait for Encryption Change.
    kWaitEncryption,

    // Error occurred; wait for link closure and ignore events.
    kFailed,
  };

  // Extra information for pairing constructed when pairing begins and destroyed
  // when pairing is reset or errors out.
  struct Data final {
    // True if the local device initiated pairing.
    bool initiator;

    // Callbacks from callers of |InitiatePairing|.
    std::vector<StatusCallback> initiator_callbacks;
  };

  static const char* ToString(State state);

  State state() const { return state_; }

  bool is_pairing() const { return current_pairing_.has_value(); }

  hci::ConnectionHandle handle() const { return link_->handle(); }

  // Call the permanent status callback this object was created with as well as
  // any callbacks from local initiators.
  void SignalStatus(hci::Status status);

  // Called to enable encryption on the link for this peer. Sets |state_| to
  // kWaitEncryption.
  void EnableEncryption();

  // Called when an event is received while in a state that doesn't expect that
  // event. Invokes |status_callback_| with HostError::kNotSupported and sets
  // |state_| to kFailed.
  void FailWithUnexpectedEvent();

  // The BR/EDR link whose pairing is being driven by this object.
  hci::Connection* const link_;

  // State machine representation.
  State state_;

  // Represents an ongoing pairing procedure.
  std::optional<Data> current_pairing_;

  // Holds the callback that this object was constructed with.
  StatusCallback status_callback_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PairingState);
};

PairingAction GetInitiatorPairingAction(hci::IOCapability initiator_cap,
                                        hci::IOCapability responder_cap);
PairingAction GetResponderPairingAction(hci::IOCapability initiator_cap,
                                        hci::IOCapability responder_cap);
hci::EventCode GetExpectedEvent(hci::IOCapability local_cap,
                                hci::IOCapability peer_cap);
bool IsPairingAuthenticated(hci::IOCapability local_cap,
                            hci::IOCapability peer_cap);

}  // namespace gap
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_PAIRING_STATE_H_
