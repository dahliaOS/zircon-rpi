// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

#[derive(Debug)]
pub struct RejectResponse {
    pub pdu_id: u8,
    pub status_code: u8,
}

impl RejectResponse {
    pub fn new(pdu_id: &PduId, status_code: &StatusCode) -> Self {
        Self { pdu_id: u8::from(pdu_id), status_code: u8::from(status_code) }
    }
}

impl VendorDependent for RejectResponse {
    fn pdu_id(&self) -> PduId {
        PduId::try_from(self.pdu_id).unwrap()
    }
}

impl Encodable for RejectResponse {
    fn encoded_len(&self) -> usize {
        1
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        if buf.len() < 1 {
            return Err(Error::OutOfRange);
        }

        buf[0] = self.status_code;
        Ok(())
    }
}