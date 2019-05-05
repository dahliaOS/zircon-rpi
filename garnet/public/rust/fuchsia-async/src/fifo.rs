// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::RWHandle,
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::{ready, task::Context, Future, Poll, Stream},
    std::{
        fmt,
        marker::{PhantomData, Unpin},
        pin::Pin,
    },
};

/// Marker trait for types that can be read/written with a `Fifo`.
/// Unsafe because not all types may be represented by arbitrary bit patterns.
pub unsafe trait FifoEntry {}

/// Identifies that the object may be used to write entries into a FIFO.
pub trait FifoWritable<W: FifoEntry>
where
    Self: Sized,
{
    /// Creates a future that transmits entries to be written.
    ///
    /// The returned future will return after an entry has been received on
    /// this fifo. The future will resolve to the fifo once all elements
    /// have been transmitted.
    ///
    /// An error during writing will cause the fifo to get
    /// destroyed and the status will be returned.
    fn write_entries<'a>(&'a self, entries: &'a [W]) -> WriteEntry<'a, Self, W> {
        WriteEntry::new(self, entries)
    }

    /// Writes entries to the fifo and registers this `Fifo` as
    /// needing a write on receiving a `zx::Status::SHOULD_WAIT`.
    ///
    /// Returns the number of elements processed.
    fn write(&self, cx: &mut Context<'_>, entries: &[W]) -> Poll<Result<usize, zx::Status>>;
}

/// Identifies that the object may be used to read entries from a FIFO.
pub trait FifoReadable<R: FifoEntry>
where
    Self: Sized,
{
    /// Creates a future that receives an entry to be written to the element
    /// provided.
    ///
    /// The returned future will return after an entry has been received on
    /// this fifo. The future will resolve to the fifo and the entry.
    ///
    /// An error during reading will cause the fifo and entry to get
    /// destroyed and the status will be returned.
    fn read_entry(&self) -> ReadEntry<Self, R> {
        ReadEntry::new(self)
    }

    /// Reads an entry from the fifo and registers this `Fifo` as
    /// needing a read on receiving a `zx::Status::SHOULD_WAIT`.
    fn read(&self, cx: &mut Context<'_>) -> Poll<Result<Option<R>, zx::Status>>;

    /// Reads multiple entries from the fifo and registers this `Fifo` as needing a read on
    /// receiving a `zx::Status::SHOULD_WAIT`.
    ///
    /// On success the actual number of `entries` read into will be returned.
    fn readn(
        &self,
        cx: &mut Context<'_>,
        entries: &mut [R],
    ) -> Poll<Result<Option<usize>, zx::Status>>;

    /// Creates a stream that can be polled and waited on to yield entries.
    ///
    /// The [`EntryStream`] returned implements `Stream<Item = Result<R, zx::Status>>`
    fn stream(&self) -> EntryStream<Self, R> {
        EntryStream::new(self)
    }

    /// Creates a stream that buffers internally and can be polled and waited on to yield entries.
    ///
    /// The yielded items of this stream are identical to those produced by
    /// [`stream`](FifoReadable::stream), with the difference being this stream may be more
    /// efficient due to attempting to read `buffer_depth` items off the `Fifo` at any given point
    /// in time. The buffered items can then be returned in successive polls without needing to
    /// further access the fifo.
    fn buffered_stream(&self, buffer_depth: usize) -> BufferedEntryStream<Self, R> {
        BufferedEntryStream::new(self, buffer_depth)
    }
}

/// An I/O object representing a `Fifo`.
pub struct Fifo<R: FifoEntry, W: FifoEntry = R> {
    handle: RWHandle<zx::Fifo>,
    read_marker: PhantomData<R>,
    write_marker: PhantomData<W>,
}

impl<R: FifoEntry, W: FifoEntry> AsRef<zx::Fifo> for Fifo<R, W> {
    fn as_ref(&self) -> &zx::Fifo {
        self.handle.get_ref()
    }
}

impl<R: FifoEntry, W: FifoEntry> AsHandleRef for Fifo<R, W> {
    fn as_handle_ref(&self) -> zx::HandleRef {
        self.handle.get_ref().as_handle_ref()
    }
}

impl<R: FifoEntry, W: FifoEntry> From<Fifo<R, W>> for zx::Fifo {
    fn from(fifo: Fifo<R, W>) -> zx::Fifo {
        fifo.handle.into_inner()
    }
}

impl<R: FifoEntry, W: FifoEntry> Fifo<R, W> {
    /// Creates a new `Fifo` from a previously-created `zx::Fifo`.
    pub fn from_fifo(fifo: zx::Fifo) -> Result<Self, zx::Status> {
        Ok(Fifo {
            handle: RWHandle::new(fifo)?,
            read_marker: PhantomData,
            write_marker: PhantomData,
        })
    }

    /// Test whether this fifo is ready to be written or not.
    ///
    /// If the fifo is *not* writable then the current task is scheduled to
    /// get a notification when the fifo does become writable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the fifo is writable again.
    ///
    /// Returns `true` if the CLOSED signal has been received.
    pub fn poll_write(&self, cx: &mut Context<'_>) -> Poll<Result<bool, zx::Status>> {
        self.handle.poll_write(cx)
    }

    /// Writes entries to the fifo and registers this `Fifo` as
    /// needing a write on receiving a `zx::Status::SHOULD_WAIT`.
    ///
    /// Returns the number of elements processed.
    pub fn try_write(&self, cx: &mut Context<'_>, entries: &[W]) -> Poll<Result<usize, zx::Status>> {
        let clear_closed = ready!(self.poll_write(cx)?);
        let elem_size = ::std::mem::size_of::<W>();
        let elembuf = unsafe {
            ::std::slice::from_raw_parts(entries.as_ptr() as *const u8, elem_size * entries.len())
        };
        match self.as_ref().write(elem_size, elembuf) {
            Err(e) => {
                if e == zx::Status::SHOULD_WAIT {
                    self.handle.need_write(cx, clear_closed)?;
                    Poll::Pending
                } else {
                    Poll::Ready(Err(e))
                }
            }
            Ok(count) => Poll::Ready(Ok(count)),
        }
    }

    /// Test whether this fifo is ready to be read or not.
    ///
    /// If the fifo is *not* readable then the current task is scheduled to
    /// get a notification when the fifo does become readable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the fifo is readable again.
    ///
    /// Returns `true` if the CLOSED signal has been received.
    pub fn poll_read(&self, cx: &mut Context<'_>) -> Poll<Result<bool, zx::Status>> {
        self.handle.poll_read(cx)
    }

    /// Reads an entry from the fifo and registers this `Fifo` as
    /// needing a read on receiving a `zx::Status::SHOULD_WAIT`.
    pub fn try_read(&self, cx: &mut Context<'_>) -> Poll<Result<Option<R>, zx::Status>> {
        let clear_closed = ready!(self.handle.poll_read(cx)?);
        let mut element = unsafe { ::std::mem::uninitialized() };
        let elembuf = unsafe {
            ::std::slice::from_raw_parts_mut(
                &mut element as *mut R as *mut u8,
                ::std::mem::size_of::<R>(),
            )
        };

        match self.as_ref().read(::std::mem::size_of::<R>(), elembuf) {
            Err(e) => {
                // Ensure `drop` isn't called on uninitialized memory.
                ::std::mem::forget(element);
                if e == zx::Status::SHOULD_WAIT {
                    self.handle.need_read(cx, clear_closed)?;
                    return Poll::Pending;
                }
                if e == zx::Status::PEER_CLOSED {
                    return Poll::Ready(Ok(None));
                }
                return Poll::Ready(Err(e));
            }
            Ok(count) => {
                debug_assert_eq!(1, count);
                return Poll::Ready(Ok(Some(element)));
            }
        }
    }

    /// Reads multiple entires from the fifo and registers this `Fifo` as needing a read on
    /// receiving a `zx::Status::SHOULD_WAIT`.
    ///
    /// On success the actual number of `entries` read into will be returned.
    // Due to the specifics of dealing with uninitialized memory it is actually non-trivial to
    // refactor this and try_read to have much common code.
    pub fn try_readn(
        &self,
        cx: &mut Context<'_>,
        entries: &mut [R],
    ) -> Poll<Result<Option<usize>, zx::Status>> {
        let clear_closed = ready!(self.handle.poll_read(cx)?);
        let elembuf = unsafe {
            ::std::slice::from_raw_parts_mut(
                entries.as_mut_ptr() as *mut u8,
                ::std::mem::size_of::<R>() * entries.len(),
            )
        };

        match self.as_ref().read(::std::mem::size_of::<R>(), elembuf) {
            Err(zx::Status::SHOULD_WAIT) => {
                self.handle.need_read(cx, clear_closed)?;
                Poll::Pending
            }
            Err(zx::Status::PEER_CLOSED) => Poll::Ready(Ok(None)),
            Err(e) => Poll::Ready(Err(e)),
            Ok(count) => Poll::Ready(Ok(Some(count))),
        }
    }
}

impl<R: FifoEntry, W: FifoEntry> FifoReadable<R> for Fifo<R, W> {
    fn read(&self, cx: &mut Context<'_>) -> Poll<Result<Option<R>, zx::Status>> {
        self.try_read(cx)
    }
    fn readn(
        &self,
        cx: &mut Context<'_>,
        entries: &mut [R],
    ) -> Poll<Result<Option<usize>, zx::Status>> {
        self.try_readn(cx, entries)
    }
}

impl<R: FifoEntry, W: FifoEntry> FifoWritable<W> for Fifo<R, W> {
    fn write(&self, cx: &mut Context<'_>, entries: &[W]) -> Poll<Result<usize, zx::Status>> {
        self.try_write(cx, entries)
    }
}

impl<R: FifoEntry, W: FifoEntry> fmt::Debug for Fifo<R, W> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.handle.get_ref().fmt(f)
    }
}

/// WriteEntry represents the future of one or more writes.
pub struct WriteEntry<'a, F: 'a, W: 'a> {
    fifo: &'a F,
    entries: &'a [W],
}

impl<'a, F, W> Unpin for WriteEntry<'a, F, W> {}

impl<'a, F: FifoWritable<W>, W: FifoEntry> WriteEntry<'a, F, W> {
    /// Create a new WriteEntry, which borrows the `FifoWritable` type
    /// until the future completes.
    pub fn new(fifo: &'a F, entries: &'a [W]) -> Self {
        WriteEntry { fifo, entries }
    }
}

impl<'a, F: FifoWritable<W>, W: FifoEntry> Future for WriteEntry<'a, F, W> {
    type Output = Result<(), zx::Status>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = &mut *self;
        while !this.entries.is_empty() {
            let advance = ready!(this.fifo.write(cx, this.entries)?);
            this.entries = &this.entries[advance..];
        }
        Poll::Ready(Ok(()))
    }
}

/// ReadEntry represents the future of a single read.
pub struct ReadEntry<'a, F: 'a, R: 'a> {
    fifo: &'a F,
    read_marker: PhantomData<R>,
}

impl<'a, F, W> Unpin for ReadEntry<'a, F, W> {}

impl<'a, F: FifoReadable<R>, R: FifoEntry> ReadEntry<'a, F, R> {
    /// Create a new ReadEntry, which borrows the `FifoReadable` type
    /// until the future completes.
    pub fn new(fifo: &'a F) -> ReadEntry<F, R> {
        ReadEntry {
            fifo,
            read_marker: PhantomData,
        }
    }
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> Future for ReadEntry<'a, F, R> {
    type Output = Result<Option<R>, zx::Status>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.fifo.read(cx)
    }
}

/// EntryStream represents the stream of reads from the fifo.
pub struct EntryStream<'a, F: 'a, R: 'a> {
    fifo: &'a F,
    read_marker: PhantomData<R>,
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> EntryStream<'a, F, R> {
    /// Create a new EntryStream, which borrows the `FifoReadable` type
    /// until the stream completes.
    pub fn new(fifo: &'a F) -> EntryStream<F, R> {
        EntryStream { fifo, read_marker: PhantomData }
    }
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> Stream for EntryStream<'a, F, R> {
    type Item = Result<R, zx::Status>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.fifo.read(cx).map(|v| v.transpose())
    }
}

/// Represents a stream of reads from the fifo that may be batched and buffered.
///
/// Items may be read from the underlying fifo in batches, and will then be returned as successive
/// items on the stream. The [`take`](BufferedEntryStream::take) method can be used to close the
/// stream without losing any buffered items.
pub struct BufferedEntryStream<'a, F: FifoReadable<R>, R: FifoEntry> {
    fifo: &'a F,
    buffer: Vec<R>,
    index: usize,
    items: usize,
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> Unpin for BufferedEntryStream<'a, F, R> {}

impl<'a, F: FifoReadable<R>, R: FifoEntry> Drop for BufferedEntryStream<'a, F, R> {
    fn drop(&mut self) {
        // Pull any remaining items out of the buffer so they get dropped.
        while let Some(_) = self.pop_item() {}
        // Set the buffer length back to 0 so that items, including uninitialized ones, don't get
        // dropped again as at this point we know every item in the vec has been dropped.
        unsafe {
            self.buffer.set_len(0);
        }
    }
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> BufferedEntryStream<'a, F, R> {
    /// Create a new BufferedEntryStream, which borrows the `FifoReadable` type until the stream
    /// completes.
    pub fn new(fifo: &'a F, buffer_depth: usize) -> BufferedEntryStream<'a, F, R> {
        let mut buffer = Vec::with_capacity(buffer_depth);
        // Set the length of the buffer to our desired depth to ensure underlying memory is
        // allocated
        unsafe {
            buffer.set_len(buffer_depth);
        }
        BufferedEntryStream { fifo, buffer, index: 0, items: 0 }
    }

    /// Destroy, returning anything remaining in the buffer.
    pub fn take(mut self) -> Vec<R> {
        let mut v = Vec::with_capacity(self.items - self.index);
        while let Some(item) = self.pop_item() {
            v.push(item);
        }
        v
    }

    fn pop_item(&mut self) -> Option<R> {
        if self.index < self.items {
            let ret = unsafe { self.buffer.as_ptr().add(self.index).read() };
            self.index = self.index + 1;
            Some(ret)
        } else {
            None
        }
    }
}

impl<'a, F: FifoReadable<R>, R: FifoEntry> Stream for BufferedEntryStream<'a, F, R> {
    type Item = Result<R, zx::Status>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = &mut *self;
        if let Some(item) = this.pop_item() {
            return Poll::Ready(Some(Ok(item)));
        }
        // to get here the buffer must be fully empty
        debug_assert_eq!(self.index, self.items);
        self.fifo
            .readn(cx, self.buffer.as_mut_slice())
            .map_ok(|value| {
                value.map(|count| {
                    debug_assert_ne!(0, count);
                    self.items = count;
                    self.index = 0;
                    // must succeed as count was not zero
                    self.pop_item().unwrap()
                })
            })
            .map(|r| r.transpose())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DurationExt, Executor, TimeoutExt, Timer};
    use fuchsia_zircon::prelude::*;
    use futures::future::try_join;
    use futures::prelude::*;

    #[derive(Clone, Debug, PartialEq, Eq)]
    #[repr(C)]
    struct entry {
        a: u32,
        b: u32,
    }
    unsafe impl FifoEntry for entry {}

    #[derive(Clone, Debug, PartialEq, Eq)]
    #[repr(C)]
    struct wrong_entry {
        a: u16,
    }
    unsafe impl FifoEntry for wrong_entry {}

    #[test]
    fn can_read_write() {
        let mut exec = Executor::new().expect("failed to create executor");
        let elements: &[entry; 1] = &[entry { a: 10, b: 20 }];

        let (tx, rx) = zx::Fifo::create(2, ::std::mem::size_of::<entry>())
            .expect("failed to create zx fifo");
        let (tx, rx) = (
            Fifo::<entry>::from_fifo(tx).expect("failed to create async tx fifo"),
            Fifo::<entry>::from_fifo(rx).expect("failed to create async rx fifo"),
        );

        let receive_future = rx.read_entry().map_ok(|entry| {
            assert_eq!(elements[0], entry.expect("peer closed"));
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let receiver = receive_future.on_timeout(300.millis().after_now(), || panic!("timeout"));

        // Sends an entry after the timeout has passed
        let sender = Timer::new(10.millis().after_now()).then(|()| tx.write_entries(elements));

        let done = try_join(receiver, sender);
        exec.run_singlethreaded(done)
            .expect("failed to run receive future on executor");
    }

    #[test]
    fn read_wrong_size() {
        let mut exec = Executor::new().expect("failed to create executor");
        let elements: &[entry; 1] = &[entry { a: 10, b: 20 }];

        let (tx, rx) = zx::Fifo::create(2, ::std::mem::size_of::<entry>())
            .expect("failed to create zx fifo");
        let (tx, rx) = (
            Fifo::<entry>::from_fifo(tx).expect("failed to create async tx fifo"),
            Fifo::<wrong_entry>::from_fifo(rx).expect("failed to create async rx fifo"),
        );

        let receive_future = rx
            .read_entry()
            .map_ok(|_entry| panic!("read should have failed"));

        // add a timeout to receiver so if test is broken it doesn't take forever
        let receiver = receive_future.on_timeout(300.millis().after_now(), || panic!("timeout"));

        // Sends an entry after the timeout has passed
        let sender = Timer::new(10.millis().after_now()).then(|()| tx.write_entries(elements));

        let done = try_join(receiver, sender);
        let res = exec.run_singlethreaded(done);
        match res {
            Err(zx::Status::OUT_OF_RANGE) => (),
            _ => panic!("did not get out-of-range error"),
        }
    }

    #[test]
    fn write_wrong_size() {
        let mut exec = Executor::new().expect("failed to create executor");
        let elements: &[wrong_entry; 1] = &[wrong_entry { a: 10 }];

        let (tx, rx) = zx::Fifo::create(2, ::std::mem::size_of::<entry>())
            .expect("failed to create zx fifo");
        let (tx, _rx) = (
            Fifo::<wrong_entry>::from_fifo(tx).expect("failed to create async tx fifo"),
            Fifo::<entry>::from_fifo(rx).expect("failed to create async rx fifo"),
        );

        let sender = Timer::new(10.millis().after_now()).then(|()| tx.write_entries(elements));

        let res = exec.run_singlethreaded(sender);
        match res {
            Err(zx::Status::OUT_OF_RANGE) => (),
            _ => panic!("did not get out-of-range error"),
        }
    }

    #[test]
    fn write_into_full() {
        use std::sync::atomic::{AtomicUsize, Ordering};

        let mut exec = Executor::new().expect("failed to create executor");
        let elements: &[entry; 3] = &[
            entry { a: 10, b: 20 },
            entry { a: 30, b: 40 },
            entry { a: 50, b: 60 },
        ];

        let (tx, rx) = zx::Fifo::create(2, ::std::mem::size_of::<entry>())
            .expect("failed to create zx fifo");
        let (tx, rx) = (
            Fifo::<entry>::from_fifo(tx).expect("failed to create async tx fifo"),
            Fifo::<entry>::from_fifo(rx).expect("failed to create async rx fifo"),
        );

        // Use `writes_completed` to verify that not all writes
        // are transmitted at once, and the last write is actually blocked.
        let writes_completed = AtomicUsize::new(0);
        let sender = async {
            await!(tx.write_entries(&elements[..2]))?;
            writes_completed.fetch_add(1, Ordering::SeqCst);
            await!(tx.write_entries(&elements[2..]))?;
            writes_completed.fetch_add(1, Ordering::SeqCst);
            Ok::<(), zx::Status>(())
        };

        // Wait 10 ms, then read the messages from the fifo.
        let receive_future = async {
            await!(Timer::new(10.millis().after_now()));
            let entry = await!(rx.read_entry())?;
            assert_eq!(writes_completed.load(Ordering::SeqCst), 1);
            assert_eq!(elements[0], entry.expect("peer closed"));
            let entry = await!(rx.read_entry())?;
            // At this point, the last write may or may not have
            // been written.
            assert_eq!(elements[1], entry.expect("peer closed"));
            let entry = await!(rx.read_entry())?;
            assert_eq!(writes_completed.load(Ordering::SeqCst), 2);
            assert_eq!(elements[2], entry.expect("peer closed"));
            Ok::<(), zx::Status>(())
        };

        // add a timeout to receiver so if test is broken it doesn't take forever
        let receiver = receive_future.on_timeout(300.millis().after_now(), || panic!("timeout"));

        let done = try_join(receiver, sender);

        exec.run_singlethreaded(done)
            .expect("failed to run receive future on executor");
    }

    #[test]
    fn write_more_than_full() {
        let mut exec = Executor::new().expect("failed to create executor");
        let elements: &[entry; 3] = &[
            entry { a: 10, b: 20 },
            entry { a: 30, b: 40 },
            entry { a: 50, b: 60 },
        ];

        let (tx, rx) = zx::Fifo::create(2, ::std::mem::size_of::<entry>())
            .expect("failed to create zx fifo");
        let (tx, rx) = (
            Fifo::<entry>::from_fifo(tx).expect("failed to create async tx fifo"),
            Fifo::<entry>::from_fifo(rx).expect("failed to create async rx fifo"),
        );

        let sender = tx.write_entries(elements);

        // Wait 10 ms, then read the messages from the fifo.
        let receive_future = async {
            await!(Timer::new(10.millis().after_now()));
            let entry = await!(rx.read_entry())?;
            assert_eq!(elements[0], entry.expect("peer closed"));
            let entry = await!(rx.read_entry())?;
            assert_eq!(elements[1], entry.expect("peer closed"));
            let entry = await!(rx.read_entry())?;
            assert_eq!(elements[2], entry.expect("peer closed"));
            Ok::<(), zx::Status>(())
        };

        // add a timeout to receiver so if test is broken it doesn't take forever
        let receiver = receive_future.on_timeout(300.millis().after_now(), || panic!("timeout"));

        let done = try_join(receiver, sender);

        exec.run_singlethreaded(done)
            .expect("failed to run receive future on executor");
    }
}
