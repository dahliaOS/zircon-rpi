// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_bluetooth::types::PeerId,
    fuchsia_syslog::{fx_log_warn, fx_log_info},
    futures::{
        channel::oneshot,
        Future, TryStreamExt, select,
        future::{self, BoxFuture, FusedFuture, FutureExt, TryFutureExt},
        sink::SinkExt,
        stream::{self, StreamExt},
    },
    pin_utils::pin_mut,
    std::{
        fmt,
        collections::{HashMap, hash_map::Entry},
        task::{Poll, Context, Waker},
        pin::Pin,
    },
};

use crate::types::HostId;

/// An outstanding Pairing Request awaiting a response by the upstream PairingDelegate
/// We parameterize over the future return type for easier testing
pub struct PairingRequest<T> {
    /// Which host did this request come from
    host: HostId,
    /// Which peer are we attempting to pair
    peer: PeerId,
    /// Future that will obtain response
    response: BoxFuture<'static, T>,
}

impl<T> fmt::Debug for PairingRequest<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PairingRequest {{ host: {}, peer: {}, response: <..> }}", self.host, self.peer)
    }
}

impl<T> PairingRequest<T> {
    pub fn new(host: HostId, peer: PeerId, response: BoxFuture<'static, T>) -> PairingRequest<T> {
        PairingRequest{ host, peer, response }
    }
    pub fn peer(&self) -> &PeerId {
        &self.peer
    }
}

/// A collection of outstanding Pairing Requests, index by Host. Can be polled to obtain the first
/// request that obtains a completed response.
pub struct PairingRequests<T> {
    inner: HashMap<HostId, Vec<PairingRequest<T>>>,
    /// Waker for pending task
    waker: Option<Waker>,
}

impl<T> PairingRequests<T> {
    pub fn empty() -> PairingRequests<T> {
        PairingRequests { inner: HashMap::new(), waker: None }
    }
    pub fn insert(&mut self, request: PairingRequest<T>) {
        self.inner.entry(request.host.clone()).or_insert(vec![]).push(request);
        if let Some(waker) = self.waker.take() {
          waker.wake()
        }
    }
    pub fn remove_host(&mut self, host: HostId) -> Option<Vec<PairingRequest<T>>> {
        self.inner.remove(&host)
    }
    pub fn drain<'a>(&'a mut self) -> impl Iterator<Item = (HostId, Vec<PairingRequest<T>>)> + 'a {
        self.inner.drain()
    }

    #[cfg(test)]
    fn iter(&mut self) -> impl Iterator<Item = &mut PairingRequest<T>> {
        self.inner.iter_mut().flat_map(|(_,vec)| vec)
    }
    #[cfg(test)]
    fn pending_count(&self) -> usize {
        self.inner.iter().flat_map(|(_,vec)| vec).count()
    }
}

impl<T> Future for PairingRequests<T> {
    type Output = T;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut completed = None;
        for (_, requests) in self.inner.iter_mut() {
            for (index, request) in requests.iter_mut().enumerate() {
                if let Poll::Ready(response) = request.response.as_mut().poll(cx) {
                    completed = Some((index, response));
                    break;
                }
            }
            if let Some((idx, result)) = completed {
                requests.remove(idx);
                return Poll::Ready(result);
            }
        }
        self.waker = Some(cx.waker().clone());
        Poll::Pending
    }
}

impl<T> FusedFuture for PairingRequests<T> {
    fn is_terminated(&self) -> bool {
        false
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use futures::Future;
    use proptest::prelude::*;
    use proptest::strategy::LazyJust;

    ///! We validate the behavior of the PairingRequests future by enumerating all possible external
    ///! events, and then generating permutations of valid sequences of those events. These model
    ///! the possible executions sequences the future could go through in program execution. We
    ///! then assert that:
    ///!   a) At all points during execution, all invariants are held
    ///!   b) The final result is as expected
    ///!
    ///! In this case, the invariants are:
    ///! * All inserted requests are either pending (in the PairingRequests struct), or yielded
    ///! * If the future is not awoken, then all responses must be pending
    ///!
    ///! The result is:
    ///! * All expected requests have been yielded (none are still pending)
    ///!
    ///! Together these show:
    ///! * Progress is always eventually made - the Future cannot be stalled
    ///! * All inserted elements will eventually be yielded
    ///! * Elements are never duplicated

    /// Possible actions to take in evaluating the future
    #[derive(Debug)]
    enum Event {
        /// Insert a new request (which may or may not be ready) into the struct
        Insert(PairingRequest<()>),
        /// Complete a pending request
        Complete(oneshot::Sender<()>),
        /// Schedule the executor. The executor will only run the task if awoken, otherwise it will
        /// do nothing
        Execute,
    }

    fn request_pair(host: HostId, peer: PeerId) -> (PairingRequest<()>, oneshot::Sender<()>) {
        let (sender, receiver) = oneshot::channel::<()>();
        let response: BoxFuture<'static, ()> = receiver.map(|r| r.unwrap()).boxed();
        (PairingRequest{ host, peer, response }, sender)
    }

    fn event_pair(host: HostId, peer: PeerId) -> Vec<Event> {
        let (request, sender) = request_pair(host, peer);
        vec![Event::Insert(request), Event::Complete(sender)]
    }

    /// Strategy that produces random permutations of a set of events, corresponding to inserting
    /// and completing 3 different requests in random order, also interspersed with running the
    /// executor
    fn execution_sequences() -> impl Strategy<Value = Vec<Event>> {
        fn generate_events() -> Vec<Event> {
            vec![
                event_pair(HostId(1), PeerId(1)),
                event_pair(HostId(1), PeerId(2)),
                event_pair(HostId(2), PeerId(3)),
                vec![Event::Execute, Event::Execute, Event::Execute],
            ].into_iter().flatten().collect()
        }

        // We want to produce random permutations of these events
        LazyJust::new(generate_events).prop_shuffle()
    }

    proptest! {
        #[test]
        fn test_invariants(mut execution in execution_sequences()) {
            /// Add enough execution events to ensure we will complete, no matter the order
            execution.extend(std::iter::repeat_with(|| Event::Execute).take(3));

            let (waker, count) = futures_test::task::new_count_waker();
            let mut requests = PairingRequests::empty();
            let mut next_wake = 0;
            let mut yielded = 0;
            let mut inserted = 0;
            for event in execution {
                match event {
                    Event::Insert(req) => {
                        requests.insert(req);
                        inserted = inserted + 1;
                    }
                    Event::Complete(fut) => {
                        let _ = fut.send(());
                    }
                    Event::Execute if count.get() >= next_wake => {
                        match Pin::new(&mut requests).poll(&mut Context::from_waker(&waker)) {
                            Poll::Ready(()) => {
                                yielded = yielded + 1;
                                // Ensure that we wake up next time;
                                next_wake = count.get();
                            }
                            Poll::Pending => {
                                next_wake = count.get() + 1;
                            }
                        };
                    }
                    Event::Execute => (),
                }
                // Test invariants
                // * Ensure that all inserted requests are either pending or yielded
                assert_eq!(requests.pending_count(), inserted - yielded, "pending == inserted - yielded");
                // * If the task has not been woken, then all responses are pending
                assert!(count.get() >= next_wake || requests.iter().all(
                        |r| Pin::new(&mut r.response).poll(&mut Context::from_waker(&waker)) == Poll::Pending
                    ));
            }
            assert_eq!(inserted, 3, "All requests inserted");
            assert_eq!(yielded, 3, "All responses yielded");
            assert_eq!(requests.pending_count(), 0, "No pending requests remaining");
        }
    }
}
