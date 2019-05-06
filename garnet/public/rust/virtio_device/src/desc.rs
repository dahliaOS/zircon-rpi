// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::queue::{Desc, DescChain, DescChainIter, DriverMem, DriverNotify, Queue, MemoryNotFound},
    futures::{task::AtomicWaker, Stream},
    std::{
        pin::Pin,
        task::{Context, Poll},
    },
    zerocopy,
};

pub struct DescChainStream<N> {
    queue: Queue<N>,
    task: std::sync::Arc<AtomicWaker>,
}

impl<N> DescChainStream<N> {
    // Creates a stream for descriptor chains.
    // You are responsible for signaling the waker.
    pub fn new(queue: Queue<N>) -> DescChainStream<N> {
        DescChainStream { queue, task: std::sync::Arc::new(AtomicWaker::new()) }
    }

    pub fn waker(&self) -> std::sync::Arc<AtomicWaker> {
        self.task.clone()
    }
}

impl<N: DriverNotify> Stream for DescChainStream<N> {
    type Item = DescChain<N>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if let Some(desc) = self.queue.next_desc() {
            return Poll::Ready(Some(desc));
        }
        self.task.register(cx.waker());
        match self.queue.next_desc() {
            Some(desc) => Poll::Ready(Some(desc)),
            None => Poll::Pending,
        }
    }
}

pub struct ReadableDescChain<'a, N: DriverNotify, M: DriverMem> {
    iter: DescChainIter<'a, N, M>
}

impl<'a, N: DriverNotify, M: DriverMem> Iterator for ReadableDescChain<'a, N, M> {
    type Item = Result<&'a [u8], MemoryNotFound>;
    fn next(&mut self) -> Option<Result<&'a [u8], MemoryNotFound>> {
        match self.iter.next() {
            None => None,
            Some(Err(e)) => Some(Err(e)),
            Some(Ok(Desc::Readable(slice))) => Some(Ok(slice)),
            Some(Ok(Desc::Writable(_))) => None,
        }
    }
}

impl<'a, N: DriverNotify, M: DriverMem> ReadableDescChain<'a, N, M> {
    pub unsafe fn new(desc_chain: &DescChainIter<'a, N, M>) -> ReadableDescChain<'a, N, M> {
        ReadableDescChain {iter: desc_chain.clone() }
    }
}

impl<'a, N: DriverNotify, M: DriverMem> Clone for ReadableDescChain<'a, N, M> {
    fn clone(&self) -> ReadableDescChain<'a, N, M> {
        unsafe {ReadableDescChain {iter: self.iter.clone()}}
    }
}

pub struct WritableDescChain<'a, N: DriverNotify, M: DriverMem> {
    iter: DescChainIter<'a, N, M>
}

impl<'a, N: DriverNotify, M: DriverMem> Iterator for WritableDescChain<'a, N, M> {
    type Item = Result<&'a mut [u8], MemoryNotFound>;
    fn next(&mut self) -> Option<Result<&'a mut [u8], MemoryNotFound>> {
        let mut item = self.iter.next();
        while let Some(Ok(Desc::Readable(_))) = item {
            item = self.iter.next();
        }

        match item {
            None => None,
            Some(Err(e)) => Some(Err(e)),
            Some(Ok(Desc::Readable(_))) => panic!("Case will not happen due to prior loop"),
            Some(Ok(Desc::Writable(slice))) => Some(Ok(slice)),
        }
    }
}

impl<'a, N: DriverNotify, M: DriverMem> WritableDescChain<'a, N, M> {
    pub fn new(desc_chain: DescChainIter<'a, N, M>) -> WritableDescChain<'a, N, M> {
        WritableDescChain { iter: desc_chain }
    }
    pub fn set_written(&mut self, written: u32) {
        self.iter.get_chain().set_written(written);
    }
}

pub fn make_desc_chain_pair<'a, N: DriverNotify, M: DriverMem>(desc_chain: DescChainIter<'a, N, M>) -> (ReadableDescChain<'a, N, M>, WritableDescChain<'a, N, M>) {
    // TODO: explain why this is safe
    (unsafe {ReadableDescChain::new(&desc_chain)}, WritableDescChain::new(desc_chain))
}

pub struct DescChainBytes<'a, N: DriverNotify, M: DriverMem> {
    read_iter: ReadableDescChain<'a, N, M>,
    write_iter: WritableDescChain<'a, N, M>,
    read_state: Option<&'a [u8]>,
    write_state: Option<&'a mut [u8]>,
    written: u32,
}

pub enum ReadView<'a, T> {
    ZeroCopy(zerocopy::LayoutVerified<&'a [u8], T>),
    Copied(T),
}

#[derive(Fail, Debug)]
pub enum DescBytesError {
    #[fail(display = "Reached end of descriptors")]
    EndOfChain,
    #[fail(display = "{}", 0)]
    Memory(#[fail(cause)]MemoryNotFound),
}

impl From<MemoryNotFound> for DescBytesError {
    fn from(error: MemoryNotFound) -> Self {
        DescBytesError::Memory(error)
    }
}

impl<'a, N: DriverNotify, M: DriverMem> Drop for DescChainBytes<'a, N, M> {
    fn drop(&mut self) {
        self.write_iter.set_written(self.written);
    }
}

impl<'a, N: DriverNotify, M: DriverMem> DescChainBytes<'a, N, M> {
    pub fn end_of_readable(&self) -> bool {
        self.read_state.is_none()
    }
    pub fn end_of_writable(&self) -> bool {
        self.write_state.is_none()
    }
    pub fn new(iter: DescChainIter<'a, N, M>) -> Result<DescChainBytes<'a, N, M>, MemoryNotFound> {
        let (mut read_iter, mut write_iter) = make_desc_chain_pair(iter);
        let read_state = read_iter.next().transpose()?;
        let write_state = write_iter.next().transpose()?;
        Ok(DescChainBytes {
            read_iter, write_iter, read_state, write_state, written: 0})
    }
    fn next_read_state(&mut self) -> Result<(), MemoryNotFound> {
        self.read_state = self.read_iter.next().transpose()?;
        Ok(())
    }
    fn next_write_state(&mut self) -> Result<(), MemoryNotFound> {
        self.write_state = self.write_iter.next().transpose()?;
        Ok(())
    }
    pub fn next_readable<T: zerocopy::FromBytes>(&mut self) -> Result<ReadView<'a, T>, DescBytesError> {
        match self.read_state.take() {
            None => Err(DescBytesError::EndOfChain),
            Some(slice) => {
                // Try and take it out
                // TODO: no unwrap and handle need to do copy
                if let Some((header, data)) = zerocopy::LayoutVerified::new_from_prefix(slice) {
                    if data.len() == 0 {
                        self.next_read_state()?;
                    } else {
                        self.read_state = Some(data);
                    }
                    Ok(ReadView::ZeroCopy(header))
                } else {
                    panic!("unsupported")
                }
            },
        }
    }
    pub fn next_writable<T: zerocopy::AsBytes>(&mut self) -> Result<zerocopy::LayoutVerified<&'a mut [u8], T>, DescBytesError> {
        match self.write_state.take() {
            None => Err(DescBytesError::EndOfChain),
            Some(slice) => {
                let (header, data) = zerocopy::LayoutVerified::new_from_prefix(slice).unwrap();
                if data.len() == 0 {
                    self.next_write_state()?;
                } else {
                    self.write_state = Some(data);
                }
                Ok(header)
            },
        }
    }
    // TODO: this should support a Copy and write on drop style wrapper like read does (see doc)
    pub fn next_written<T: zerocopy::AsBytes>(&mut self) -> Result<zerocopy::LayoutVerified<&'a mut [u8], T>, DescBytesError> {
        let layout = self.next_writable()?;
        self.mark_written(std::mem::size_of::<T>() as u32);
        Ok(layout)
    }
    pub fn readable_remaining(&self) -> usize {
        // TODO: no unwrap
        if let Some(slice) = self.read_state {
            self.read_iter.clone().map(|x| x.unwrap().len()).sum::<usize>() + slice.len()
        } else {
            0
        }
    }
    pub fn skip_till_readable(&mut self) -> Result<(), ()> {
        unimplemented!()
    }
    pub fn next_bytes_readable(&mut self) -> Result<&'a[u8], DescBytesError> {
        self.next_bytes_readable_n(std::usize::MAX)
    }
    pub fn next_bytes_writable(&mut self) -> Result<&'a mut [u8], DescBytesError> {
        self.next_bytes_writable_n(std::usize::MAX)
    }
    pub fn next_bytes_readable_n(&mut self, limit: usize) -> Result<&'a[u8], DescBytesError> {
        match self.read_state.take() {
            None => Err(DescBytesError::EndOfChain),
            Some(slice) => {
                if slice.len() > limit {
                    let (ret, remain) = slice.split_at(limit);
                    self.read_state = Some(remain);
                    Ok(ret)
                } else {
                    let ret = slice;
                    self.next_read_state()?;
                    Ok(ret)
                }
            },
        }
    }
    pub fn next_bytes_writable_n(&mut self, limit: usize) -> Result<&'a mut [u8], DescBytesError> {
        match self.write_state.take() {
            None => Err(DescBytesError::EndOfChain),
            Some(slice) => {
                if slice.len() > limit {
                    let (ret, remain) = slice.split_at_mut(limit);
                    self.write_state = Some(remain);
                    Ok(ret)
                } else {
                    let ret = slice;
                    self.next_write_state()?;
                    Ok(ret)
                }
            },
        }
    }
    pub fn mark_written(&mut self, written: u32) {
        self.written = self.written + written as u32;
    }
}
impl<'a, N: DriverNotify, M: DriverMem> std::io::Read for DescChainBytes<'a, N, M> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self.next_bytes_readable_n(buf.len()) {
            Ok(slice) => {
                &mut buf[0..slice.len()].copy_from_slice(slice);
                Ok(slice.len())
            },
            Err(_) => Ok(0),
        }
    }
}
impl<'a, N: DriverNotify, M: DriverMem> std::io::Write for DescChainBytes<'a, N, M> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        match self.next_bytes_writable_n(buf.len()) {
            Ok(slice) => {
                slice.copy_from_slice(&buf[0..slice.len()]);
                self.mark_written(slice.len() as u32);
                Ok(slice.len())
            },
            Err(_) => Ok(0),
        }
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
