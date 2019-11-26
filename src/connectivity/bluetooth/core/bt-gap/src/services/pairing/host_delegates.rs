// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_bluetooth_control::{
        PairingDelegateRequest,
        PairingDelegateRequestStream,
    },
    fuchsia_syslog::{fx_log_warn, fx_log_info},
    futures::{
        Future, TryStreamExt, select,
        future::{self, BoxFuture, FusedFuture, FutureExt, TryFutureExt},
        stream::{self, Stream, StreamExt},
    },
    pin_utils::pin_mut,
    std::{
        hash::Hash,
        collections::HashMap,
        fmt::Debug,
        task::{Poll, Context, Waker},
        pin::Pin,
    },
};

/// Collection of all our current streams. Delegates is a future which will complete with the next
/// available request from any host, or a closed notification for a host whose stream has
/// terminated, whichever occurs first.
pub struct Delegates<K, S> {
    /// Streams `S` identified by key `K`
    inner: HashMap<K, S>,
    /// Waker for pending task
    waker: Option<Waker>,
}

impl<K: Eq + Hash, S> Delegates<K, S> {
    /// Create a new empty Delegates
    pub fn empty() -> Delegates<K, S> {
        Delegates { inner: HashMap::new(), waker: None }
    }
    /// Insert a new stream into the Delegates
    pub fn insert(&mut self, key: K, stream: S) -> Option<S> {
        if let Some(waker) = self.waker.take() {
          waker.wake()
        };
        self.inner.insert(key, stream)
    }
    #[cfg(test)]
    fn values_mut(&mut self) -> impl Iterator<Item = &mut S> {
        self.inner.values_mut()
    }
}

/// Possible result of polling a HostDelegate - a new request, or termination of the request stream
#[derive(Debug)]
pub enum DelegatesMsg<K, V> {
    /// A new Request from the given host
    Request(K, V),
    /// The given host channel has terminated
    StreamClosed(K),
}

//TODO (nickpollard) - can we generalize the error type to one that is Debug-able?
// (or any Fail/Error type?)
impl<K, S, V> Future for Delegates<K, S>
where S: Stream<Item = Result<V, fidl::Error>> + Unpin,
      K: Clone + Debug + Eq + Hash + Unpin {
    type Output = DelegatesMsg<K, V>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut result = Poll::Pending;
        let mut to_remove = None;
        for (key, stream) in self.inner.iter_mut() {
            match Pin::new(&mut stream.next()).poll(cx) {
                Poll::Ready(Some(Ok(req))) => {
                    result = Poll::Ready(DelegatesMsg::Request(key.clone(), req));
                    break;
                }
                // if a stream returns None, remove it and return StreamClosed
                Poll::Ready(None) => {
                    to_remove = Some(key.clone());
                    result = Poll::Ready(DelegatesMsg::StreamClosed(key.clone()));
                    break;
                }
                // if a stream returns Error, remove it, log, and return StreamClosed
                Poll::Ready(Some(Err(e))) => {
                    to_remove = Some(key.clone());
                    fx_log_warn!("Error reading Request Stream from key {:?}, closing channel: {}", key, e);
                    result = Poll::Ready(DelegatesMsg::StreamClosed(key.clone()));
                    break;
                }
                Poll::Pending => (),
            }
        }
        if let Some(key) = to_remove {
            self.inner.remove(&key);
        }
        if result.is_pending() {
            self.waker = Some(cx.waker().clone());
        }
        result
    }
}

impl<K, S, V> FusedFuture for Delegates<K, S>
where S: Stream<Item = Result<V, fidl::Error>> + Unpin,
      K: Clone + Debug + Eq + Hash + Unpin {
    fn is_terminated(&self) -> bool {
        false
    }
}

// TODO(nickpollard)
#[cfg(test)]
mod test {
    use super::*;
    use futures::Future;
    use proptest::prelude::*;
    use proptest::strategy::LazyJust;
    use futures::channel::mpsc;
    use std::collections::HashSet;

    ///! We validate the behavior of the Delegates future by enumerating all possible external
    ///! events, and then generating permutations of valid sequences of those events. These model
    ///! the possible executions sequences the future could go through in program execution. We
    ///! then assert that:
    ///!   a) At all points during execution, all invariants are held
    ///!   b) The final result is as expected
    ///!
    ///! In this case, the invariants are:
    ///!   TODO
    ///!
    ///! The result is:
    ///!   TODO
    ///!
    ///! Together these show:
    ///!   TODO

    /// Possible actions to take in evaluating the future
    enum Event<K> {
        /// Insert a new request stream
        InsertStream(K, mpsc::Receiver<Result<u64, fidl::Error>>),
        /// Send a new request
        SendRequest(K, mpsc::Sender<Result<u64, fidl::Error>>),
        /// Close an existing request stream
        CloseStream(K, mpsc::Sender<Result<u64, fidl::Error>>),
        /// Schedule the executor. The executor will only run the task if awoken, otherwise it will
        /// do nothing
        Execute,
    }

    impl<K: Debug> Debug for Event<K> {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            match self {
                Event::InsertStream(k, _) => write!(f, "InsertStream({:?})", k),
                Event::SendRequest(k, _) => write!(f, "SendRequest({:?})", k),
                Event::CloseStream(k, _) => write!(f, "CloseStream({:?})", k),
                Event::Execute => write!(f, "Execute"),
            }
        }
    }

    fn stream_events<K: Clone + Eq + Hash>(key: K) -> Vec<Event<K>> {
        // Ensure that the channel is big enough to always handle all the Sends we make
        let (sender, receiver) = mpsc::channel::<Result<u64, fidl::Error>>(10);
        vec![Event::InsertStream(key.clone(), receiver),
            Event::SendRequest(key.clone(), sender.clone()),
            Event::CloseStream(key, sender)]
    }

    /// Determine how many events are sent on open channels (a channel is ope if it has not been
    /// closed, even if it has not yet been inserted into the delegate)
    fn expected_yield<K: Eq + Hash>(events: &Vec<Event<K>>) -> usize {
        events.iter().fold((HashSet::new(), 0), |(mut terminated, closed), event| {
            match event {
                Event::CloseStream(k, _) => {
                    terminated.insert(k);
                    (terminated, closed)
                }
                Event::SendRequest(k, _) if !terminated.contains(k) => (terminated, closed + 1),
                _ => (terminated, closed),
            }
        }).1
    }

    /// Strategy that produces random permutations of a set of events, corresponding to inserting
    /// and completing 3 different requests in random order, also interspersed with running the
    /// executor
    fn execution_sequences() -> impl Strategy<Value = Vec<Event<u64>>> {
        fn generate_events() -> Vec<Event<u64>> {
            vec![
                stream_events(0),
                stream_events(1),
                stream_events(2),
                vec![Event::Execute, Event::Execute, Event::Execute,
                     Event::Execute, Event::Execute, Event::Execute],
            ].into_iter().flatten().collect()
        }

        // We want to produce random permutations of these events
        LazyJust::new(generate_events).prop_shuffle()
    }

    proptest! {
        #[test]
        fn test_invariants(mut execution in execution_sequences()) {
            /// Add enough execution events to ensure we will complete, no matter the order
            execution.extend(std::iter::repeat_with(|| Event::Execute).take(6));

            let expected = expected_yield(&execution);

            let (waker, count) = futures_test::task::new_count_waker();
            let (send_waker, _) = futures_test::task::new_count_waker();
            let mut delegates = Delegates::empty();
            let mut next_wake = 0;
            let mut yielded = 0;
            let mut inserted = 0;
            let mut closed = 0;
            for event in execution {
                match event {
                    Event::InsertStream(key, stream) => {
                        delegates.insert(key, stream);
                    }
                    Event::SendRequest(_, mut sender) => {
                        if let Poll::Ready(Ok(())) = sender.poll_ready(&mut Context::from_waker(&send_waker)) {
                            prop_assert_eq!(sender.start_send(Ok(1)), Ok(()));
                            inserted = inserted + 1;
                        }
                    }
                    Event::CloseStream(_, mut stream) => {
                        stream.close_channel();
                    }
                    Event::Execute if count.get() >= next_wake => {
                        match Pin::new(&mut delegates).poll(&mut Context::from_waker(&waker)) {
                            Poll::Ready(DelegatesMsg::Request(k,v)) => {
                                yielded = yielded + 1;
                                // Ensure that we wake up next time;
                                next_wake = count.get();
                            }
                            Poll::Ready(DelegatesMsg::StreamClosed(s)) => {
                                closed = closed + 1;
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
                // * If the task has not been woken, then all responses are pending
                prop_assert!(count.get() >= next_wake || delegates.values_mut().all(
                        |mut s| Pin::new(&mut s).poll_next(&mut Context::from_waker(&waker)).is_pending()
                ));
            }
            prop_assert_eq!(inserted, expected, "All expected requests inserted");
            prop_assert_eq!(yielded, expected, "All expected requests yielded");
            prop_assert_eq!(closed, 3, "All streams closed");
        }
    }
}
