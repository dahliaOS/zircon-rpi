// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Read debug logs, convert them to LogMessages and serve them.

use super::message::{Field, LogHierarchy, LogProperty, Message, Severity, METADATA_SIZE};
use async_trait::async_trait;
use byteorder::{ByteOrder, LittleEndian};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::stream::{unfold, Stream, TryStreamExt};
use log::error;

#[async_trait]
pub trait DebugLog {
    /// Reads a single entry off the debug log into `buffer`.  Any existing
    /// contents in `buffer` are overwritten.
    async fn read(&self, buffer: &'_ mut Vec<u8>) -> Result<(), zx::Status>;

    /// Returns a future that completes when there is another log to read.
    async fn ready_signal(&self) -> Result<(), zx::Status>;
}

pub struct KernelDebugLog {
    debuglogger: zx::DebugLog,
}

#[async_trait]
impl DebugLog for KernelDebugLog {
    async fn read(&self, buffer: &'_ mut Vec<u8>) -> Result<(), zx::Status> {
        self.debuglogger.read(buffer)
    }

    async fn ready_signal(&self) -> Result<(), zx::Status> {
        fasync::OnSignals::new(&self.debuglogger, zx::Signals::LOG_READABLE).await.map(|_| ())
    }
}

impl KernelDebugLog {
    pub fn new() -> Result<Self, zx::Status> {
        let resource = zx::Resource::from(zx::Handle::invalid());
        Ok(KernelDebugLog {
            debuglogger: zx::DebugLog::create(&resource, zx::DebugLogOpts::READABLE)?,
        })
    }
}

pub struct DebugLogBridge<K: DebugLog> {
    debug_log: K,
    buf: Vec<u8>,
}

impl<K: DebugLog> DebugLogBridge<K> {
    pub fn create(debug_log: K) -> Self {
        DebugLogBridge { debug_log, buf: Vec::with_capacity(zx::sys::ZX_LOG_RECORD_MAX) }
    }

    async fn read_log(&mut self) -> Result<Message, zx::Status> {
        loop {
            self.debug_log.read(&mut self.buf).await?;
            if let Some(message) = convert_debuglog_to_log_message(self.buf.as_slice()) {
                return Ok(message);
            }
        }
    }

    pub async fn existing_logs<'a>(&'a mut self) -> Result<Vec<Message>, zx::Status> {
        unfold(self, move |klogger| async move {
            match klogger.read_log().await {
                Err(zx::Status::SHOULD_WAIT) => None,
                x => Some((x, klogger)),
            }
        })
        .try_collect::<Vec<_>>()
        .await
    }

    pub fn listen(self) -> impl Stream<Item = Result<Message, zx::Status>> {
        unfold((true, self), move |(mut is_readable, mut klogger)| async move {
            loop {
                if !is_readable {
                    if let Err(e) = klogger.debug_log.ready_signal().await {
                        break Some((Err(e), (is_readable, klogger)));
                    }
                }
                is_readable = true;
                match klogger.read_log().await {
                    Err(zx::Status::SHOULD_WAIT) => {
                        is_readable = false;
                        continue;
                    }
                    x => break Some((x, (is_readable, klogger))),
                }
            }
        })
    }
}

/// Parses a raw debug log read from the kernel.  Returns the parsed message and
/// its size in memory on success, and None if parsing fails.
pub fn convert_debuglog_to_log_message(buf: &[u8]) -> Option<Message> {
    if buf.len() < 32 {
        return None;
    }
    let data_len = LittleEndian::read_u16(&buf[4..6]) as usize;
    if buf.len() != 32 + data_len {
        return None;
    }

    let time = zx::Time::from_nanos(LittleEndian::read_i64(&buf[8..16]));
    let pid = LittleEndian::read_u64(&buf[16..24]);
    let tid = LittleEndian::read_u64(&buf[24..32]);

    let mut contents = match String::from_utf8(buf[32..(32 + data_len)].to_vec()) {
        Err(e) => {
            error!("logger: invalid log record: {:?}", e);
            return None;
        }
        Ok(s) => s,
    };
    if let Some(b'\n') = contents.bytes().last() {
        contents.pop();
    }

    // TODO(fxb/32998): Once we support structured logs we won't need this
    // hack to match a string in klogs.
    const MAX_STRING_SEARCH_SIZE: usize = 100;
    let last = contents
        .char_indices()
        .nth(MAX_STRING_SEARCH_SIZE)
        .map(|(i, _)| i)
        .unwrap_or(contents.len());

    // Don't look beyond the 100th character in the substring to limit the cost
    // of the substring search operation.
    let early_contents = &contents[..last];

    let severity = if early_contents.contains("ERROR:") {
        Severity::Error
    } else if early_contents.contains("WARNING:") {
        Severity::Warn
    } else {
        Severity::Info
    };

    Some(Message {
        size: METADATA_SIZE + 5 /*'klog' tag*/ + contents.len() + 1,
        time,
        severity,
        contents: LogHierarchy::new(
            "root",
            vec![
                LogProperty::Uint(Field::ProcessId, pid),
                LogProperty::Uint(Field::ThreadId, tid),
                LogProperty::String(Field::Tag, "klog".to_string()),
                LogProperty::String(Field::Msg, contents),
            ],
            vec![],
        ),
    })
}

#[cfg(test)]
pub mod tests {
    use super::*;

    use futures::stream::TryStreamExt;
    use parking_lot::Mutex;
    use std::collections::VecDeque;

    type ReadResponse = Result<Vec<u8>, zx::Status>;

    /// A fake reader that returns enqueued responses on read.
    pub struct TestDebugLog {
        read_responses: Mutex<VecDeque<ReadResponse>>,
    }

    #[async_trait]
    impl DebugLog for TestDebugLog {
        async fn read(&self, buffer: &'_ mut Vec<u8>) -> Result<(), zx::Status> {
            let next_result = self
                .read_responses
                .lock()
                .pop_front()
                .expect("Got more read requests than enqueued");
            let buf_contents = next_result?;
            buffer.clear();
            buffer.extend_from_slice(&buf_contents);
            Ok(())
        }

        async fn ready_signal(&self) -> Result<(), zx::Status> {
            if self.read_responses.lock().is_empty() {
                // ready signal should never complete if we have no logs left.
                futures::future::pending().await
            }
            Ok(())
        }
    }

    impl TestDebugLog {
        pub fn new() -> Self {
            TestDebugLog { read_responses: Mutex::new(VecDeque::new()) }
        }

        pub fn enqueue_read(&self, response: Vec<u8>) {
            self.read_responses.lock().push_back(Ok(response));
        }

        pub fn enqueue_read_entry(&self, entry: &TestDebugEntry) {
            self.enqueue_read(entry.to_vec());
        }

        pub fn enqueue_read_fail(&self, error: zx::Status) {
            self.read_responses.lock().push_back(Err(error))
        }
    }

    const TEST_KLOG_HEADER: u32 = 29;
    const TEST_KLOG_FLAGS: u16 = 47;
    const TEST_KLOG_TIMESTAMP: i64 = 12345i64;
    const TEST_KLOG_PID: u64 = 0xad01u64;
    const TEST_KLOG_TID: u64 = 0xbe02u64;

    pub struct TestDebugEntry {
        pub header: u32,
        pub flags: u16,
        pub timestamp: i64,
        pub pid: u64,
        pub tid: u64,
        pub log: Vec<u8>,
    }

    impl TestDebugEntry {
        pub fn new(log: &[u8]) -> Self {
            TestDebugEntry {
                header: TEST_KLOG_HEADER,
                flags: TEST_KLOG_FLAGS,
                timestamp: TEST_KLOG_TIMESTAMP,
                pid: TEST_KLOG_PID,
                tid: TEST_KLOG_TID,
                log: log.to_vec(),
            }
        }

        /// Creates a byte representation of the klog, following format in zircon
        /// https://fuchsia.googlesource.com/fuchsia/+/refs/heads/master/zircon/kernel/lib/debuglog/include/lib/debuglog.h#52
        pub fn to_vec(&self) -> Vec<u8> {
            let datalen = self.log.len() as u16;

            let mut klog = vec![0; 32];
            LittleEndian::write_u32(&mut klog[0..4], self.header);
            LittleEndian::write_u16(&mut klog[4..6], datalen);
            LittleEndian::write_u16(&mut klog[6..8], self.flags);
            LittleEndian::write_i64(&mut klog[8..16], self.timestamp);
            LittleEndian::write_u64(&mut klog[16..24], self.pid);
            LittleEndian::write_u64(&mut klog[24..32], self.tid);
            klog.extend_from_slice(&self.log);
            klog
        }
    }

    #[test]
    fn convert_debuglog_to_log_message_test() {
        let klog = TestDebugEntry::new("test log".as_bytes());
        let log_message = convert_debuglog_to_log_message(&klog.to_vec()).unwrap();
        assert_eq!(
            log_message,
            Message {
                size: METADATA_SIZE + 6 + "test log".len(),
                time: zx::Time::from_nanos(klog.timestamp),
                severity: Severity::Info,
                contents: LogHierarchy::new(
                    "root",
                    vec![
                        LogProperty::Uint(Field::ProcessId, klog.pid),
                        LogProperty::Uint(Field::ThreadId, klog.tid),
                        LogProperty::String(Field::Tag, "klog".to_string()),
                        LogProperty::String(Field::Msg, "test log".to_string())
                    ],
                    vec![]
                )
            }
        );

        // maximum allowed klog size
        let klog = TestDebugEntry::new(&vec!['a' as u8; zx::sys::ZX_LOG_RECORD_MAX - 32]);
        let log_message = convert_debuglog_to_log_message(&klog.to_vec()).unwrap();
        assert_eq!(
            log_message,
            Message {
                size: METADATA_SIZE + 6 + zx::sys::ZX_LOG_RECORD_MAX - 32,
                time: zx::Time::from_nanos(klog.timestamp),
                severity: Severity::Info,
                contents: LogHierarchy::new(
                    "root",
                    vec![
                        LogProperty::Uint(Field::ProcessId, klog.pid),
                        LogProperty::Uint(Field::ThreadId, klog.tid),
                        LogProperty::String(Field::Tag, "klog".to_string()),
                        LogProperty::String(
                            Field::Msg,
                            String::from_utf8(vec!['a' as u8; zx::sys::ZX_LOG_RECORD_MAX - 32])
                                .unwrap()
                        )
                    ],
                    vec![]
                )
            }
        );

        // empty message
        let klog = TestDebugEntry::new(&vec![]);
        let log_message = convert_debuglog_to_log_message(&klog.to_vec()).unwrap();
        assert_eq!(
            log_message,
            Message {
                size: METADATA_SIZE + 6,
                time: zx::Time::from_nanos(klog.timestamp),
                severity: Severity::Info,
                contents: LogHierarchy::new(
                    "root",
                    vec![
                        LogProperty::Uint(Field::ProcessId, klog.pid),
                        LogProperty::Uint(Field::ThreadId, klog.tid),
                        LogProperty::String(Field::Tag, "klog".to_string()),
                        LogProperty::String(Field::Msg, "".to_string())
                    ],
                    vec![]
                )
            }
        );

        // truncated header
        let klog = vec![3u8; 4];
        assert!(convert_debuglog_to_log_message(&klog).is_none());

        // invalid utf-8
        let klog = TestDebugEntry::new(&vec![0, 159, 146, 150]);
        assert!(convert_debuglog_to_log_message(&klog.to_vec()).is_none());

        // malformed
        let klog = vec![0xffu8; 64];
        assert!(convert_debuglog_to_log_message(&klog).is_none());
    }

    #[fasync::run_until_stalled(test)]
    async fn logger_existing_logs_test() {
        let debug_log = TestDebugLog::new();
        let klog = TestDebugEntry::new("test log".as_bytes());
        debug_log.enqueue_read_entry(&klog);
        debug_log.enqueue_read_fail(zx::Status::SHOULD_WAIT);
        let mut log_bridge = DebugLogBridge::create(debug_log);

        assert_eq!(
            log_bridge.existing_logs().await.unwrap(),
            vec![Message {
                size: METADATA_SIZE + 6 + "test log".len(),
                time: zx::Time::from_nanos(klog.timestamp),
                severity: Severity::Info,
                contents: LogHierarchy::new(
                    "root",
                    vec![
                        LogProperty::Uint(Field::ProcessId, klog.pid),
                        LogProperty::Uint(Field::ThreadId, klog.tid),
                        LogProperty::String(Field::Tag, "klog".to_string()),
                        LogProperty::String(Field::Msg, "test log".to_string())
                    ],
                    vec![]
                )
            }]
        );

        // unprocessable logs should be skipped.
        let debug_log = TestDebugLog::new();
        debug_log.enqueue_read(vec![]);
        debug_log.enqueue_read_fail(zx::Status::SHOULD_WAIT);
        let mut log_bridge = DebugLogBridge::create(debug_log);
        assert!(log_bridge.existing_logs().await.unwrap().is_empty());
    }

    #[fasync::run_until_stalled(test)]
    async fn logger_keep_listening_after_exhausting_initial_contents_test() {
        let debug_log = TestDebugLog::new();
        debug_log.enqueue_read_entry(&TestDebugEntry::new("test log".as_bytes()));
        debug_log.enqueue_read_fail(zx::Status::SHOULD_WAIT);
        debug_log.enqueue_read_entry(&TestDebugEntry::new("second test log".as_bytes()));
        let log_bridge = DebugLogBridge::create(debug_log);
        let mut log_stream = Box::pin(log_bridge.listen());
        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "test log");
        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "second test log");

        // unprocessable logs should be skipped.
        let debug_log = TestDebugLog::new();
        debug_log.enqueue_read(vec![]);
        debug_log.enqueue_read_entry(&TestDebugEntry::new("test log".as_bytes()));
        let log_bridge = DebugLogBridge::create(debug_log);
        let mut log_stream = Box::pin(log_bridge.listen());
        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "test log");
    }

    #[fasync::run_until_stalled(test)]
    async fn severity_parsed_from_log() {
        let debug_log = TestDebugLog::new();
        debug_log.enqueue_read_entry(&TestDebugEntry::new("ERROR: first log".as_bytes()));
        // We look for the string 'ERROR:' to label this as a Severity::Error.
        debug_log.enqueue_read_entry(&TestDebugEntry::new("first log error".as_bytes()));
        debug_log.enqueue_read_entry(&TestDebugEntry::new("WARNING: second log".as_bytes()));
        debug_log.enqueue_read_entry(&TestDebugEntry::new("INFO: third log".as_bytes()));
        debug_log.enqueue_read_entry(&TestDebugEntry::new("fourth log".as_bytes()));
        // Create a string padded with UTF-8 codepoints at the beginning so it's not labeled
        // as an error log.
        let long_padding = (0..100).map(|_| "\u{10FF}").collect::<String>();
        let long_log = format!("{}ERROR: fifth log", long_padding);
        debug_log.enqueue_read_entry(&TestDebugEntry::new(long_log.as_bytes()));

        let log_bridge = DebugLogBridge::create(debug_log);
        let mut log_stream = Box::pin(log_bridge.listen());

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "ERROR: first log");
        assert_eq!(log_message.severity, Severity::Error);

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "first log error");
        assert_eq!(log_message.severity, Severity::Info);

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "WARNING: second log");
        assert_eq!(log_message.severity, Severity::Warn);

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "INFO: third log");
        assert_eq!(log_message.severity, Severity::Info);

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), "fourth log");
        assert_eq!(log_message.severity, Severity::Info);

        let log_message = log_stream.try_next().await.unwrap().unwrap();
        assert_eq!(log_message.msg().unwrap(), &long_log);
        assert_eq!(log_message.severity, Severity::Info);
    }
}
