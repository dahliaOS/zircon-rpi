// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use uuid::Uuid;

use crate::actor::ActorHandle;

/// A token returned to someone who has requested discovery
pub struct DiscoverySession {
    pub session_id: Uuid,
    discoverer: ActorHandle<CloseDiscoverySession>
}

impl DiscoverySession {
    pub fn new(discoverer: ActorHandle<CloseDiscoverySession>) -> DiscoverySession {
        let session_id = Uuid::new_v4();
        DiscoverySession{ session_id, discoverer }
    }
}

impl Drop for DiscoverySession {
    fn drop(&mut self) {
        self.discoverer.send(CloseDiscoverySession{ session_id: self.session_id })
    }
}

// Message sent to instruct Dispatcher to stop discovery
pub struct CloseDiscoverySession {
    pub session_id: Uuid
}
