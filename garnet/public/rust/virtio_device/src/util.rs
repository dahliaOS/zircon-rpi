// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::sync::atomic;

pub use crate::queue::*;
pub use crate::desc::*;

struct BufferedNotifyInner<N> {
    notify: N,
    was_notified: atomic::AtomicBool,
}

pub struct BufferedNotify<N>(std::sync::Arc<BufferedNotifyInner<N>>);

impl<N> Clone for BufferedNotify<N> {
    fn clone(&self) -> BufferedNotify<N> {
        BufferedNotify(self.0.clone())
    }
}

impl<N> DriverNotify for BufferedNotify<N> {
    fn notify(&self) {
        self.0.was_notified.store(true, atomic::Ordering::Relaxed);
    }
}

impl<N: DriverNotify> BufferedNotify<N> {
    pub fn flush(&self) {
        if self.0.was_notified.swap(false, atomic::Ordering::Relaxed) {
            self.0.notify.notify()
        } else {
        }
    }
}

impl<N> BufferedNotify<N> {
    pub fn new(notify: N) -> BufferedNotify<N> {
        BufferedNotify(std::sync::Arc::new(BufferedNotifyInner{notify, was_notified: atomic::AtomicBool::new(false)}))
    }
}
