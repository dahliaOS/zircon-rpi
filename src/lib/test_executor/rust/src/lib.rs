// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This crate runs and collects results from a test which implements fuchsia.test.Suite protocol.

use {
    anyhow::Context as _,
    fidl::handle::AsHandleRef,
    fidl_fuchsia_test::{
        CaseListenerRequest::Finished,
        CaseListenerRequestStream, Invocation,
        RunListenerRequest::{OnFinished, OnTestCaseStarted},
    },
    fidl_fuchsia_test_manager::{HarnessProxy, LaunchOptions},
    fuchsia_zircon_status as zx_status,
    futures::{
        channel::mpsc,
        future::join_all,
        future::BoxFuture,
        io::{self, AsyncRead},
        prelude::*,
        ready,
        task::{Context, Poll},
    },
    glob,
    log::*,
    std::{cell::RefCell, marker::Unpin, pin::Pin},
};

#[cfg(target_os = "fuchsia")]
use fidl_fuchsia_sys::LauncherProxy;

/// Defines the result of a test case run.
#[derive(PartialEq, Debug, Eq, Hash, Ord, PartialOrd, Copy, Clone)]
pub enum TestResult {
    /// Test case passed.
    Passed,
    /// Test case failed.
    Failed,
    /// Test case skipped.
    Skipped,
    /// Test case did not communicate the result.
    Error,
}

/// Event to send to caller of `run_test_component`
#[derive(PartialEq, Debug, Eq, Hash, Ord, PartialOrd)]
pub enum TestEvent {
    /// A new test case is started.
    TestCaseStarted { test_case_name: String },

    /// Test case finished.
    TestCaseFinished { test_case_name: String, result: TestResult },

    /// Test case produced a log message.
    LogMessage { test_case_name: String, msg: String },

    /// Test finishes successfully.
    Finish,
}

impl TestEvent {
    pub fn test_case_started(s: &str) -> TestEvent {
        TestEvent::TestCaseStarted { test_case_name: s.to_string() }
    }

    pub fn log_message(name: &str, log: &str) -> TestEvent {
        TestEvent::LogMessage { test_case_name: name.to_string(), msg: log.to_string() }
    }

    pub fn test_case_finished(name: &str, result: TestResult) -> TestEvent {
        TestEvent::TestCaseFinished { test_case_name: name.to_string(), result: result }
    }

    pub fn test_finished() -> TestEvent {
        TestEvent::Finish
    }
}

#[must_use = "futures/streams"]
pub struct LoggerStream {
    socket: fidl::AsyncSocket,
}
impl Unpin for LoggerStream {}

thread_local! {
    pub static BUFFER:
        RefCell<[u8; 2048]> = RefCell::new([0; 2048]);
}

impl LoggerStream {
    /// Creates a new `LoggerStream` for given `socket`.
    pub fn new(socket: fidl::Socket) -> Result<LoggerStream, anyhow::Error> {
        let l = LoggerStream {
            socket: fidl::AsyncSocket::from_socket(socket).context("Invalid zircon socket")?,
        };
        Ok(l)
    }
}

fn process_log_bytes(bytes: &[u8]) -> String {
    // TODO(anmittal): Change this to consider break in logs and handle it.
    let log = std::str::from_utf8(bytes).unwrap();
    log.to_string()
}

impl Stream for LoggerStream {
    type Item = io::Result<String>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        BUFFER.with(|b| {
            let mut b = b.borrow_mut();
            let len = ready!(Pin::new(&mut self.socket).poll_read(cx, &mut *b)?);
            if len == 0 {
                return Poll::Ready(None);
            }
            Poll::Ready(Some(process_log_bytes(&b[0..len])).map(Ok))
        })
    }
}

struct TestCaseProcessor {
    f: Option<BoxFuture<'static, Result<(), anyhow::Error>>>,
}

impl TestCaseProcessor {
    /// This will start processing of logs and events in the background. The owner of this object
    /// should call `wait_for_finish` method  to make sure all the background task completed.
    pub fn new(
        test_case_name: String,
        listener: CaseListenerRequestStream,
        logger_socket: fidl::Socket,
        sender: mpsc::Sender<TestEvent>,
    ) -> Self {
        let log_fut =
            Self::collect_and_send_logs(test_case_name.clone(), logger_socket, sender.clone());
        let f = Self::process_run_event(test_case_name, listener, log_fut, sender);

        let (remote, remote_handle) = f.remote_handle();
        fuchsia_async::spawn(remote);

        TestCaseProcessor { f: Some(remote_handle.boxed()) }
    }

    async fn process_run_event(
        name: String,
        mut listener: CaseListenerRequestStream,
        mut log_fut: Option<BoxFuture<'static, Result<(), anyhow::Error>>>,
        mut sender: mpsc::Sender<TestEvent>,
    ) -> Result<(), anyhow::Error> {
        while let Some(result) = listener.try_next().await.context("waiting for listener")? {
            match result {
                Finished { result, control_handle: _ } => {
                    // get all logs before sending finish event.
                    if let Some(ref mut log_fut) = log_fut.take().as_mut() {
                        log_fut.await?;
                    }

                    let result = match result.status {
                        Some(status) => match status {
                            fidl_fuchsia_test::Status::Passed => TestResult::Passed,
                            fidl_fuchsia_test::Status::Failed => TestResult::Failed,
                            fidl_fuchsia_test::Status::Skipped => TestResult::Skipped,
                        },
                        // This will happen when test protocol is not properly implemented
                        // by the test and it forgets to set the result.
                        None => TestResult::Error,
                    };
                    sender.send(TestEvent::test_case_finished(&name, result)).await?;
                    return Ok(());
                }
            }
        }
        if let Some(ref mut log_fut) = log_fut.take().as_mut() {
            log_fut.await?;
        }
        Ok(())
    }

    /// Internal method that put a listener on `logger_socket`, process and send logs asynchronously
    /// in the background.
    fn collect_and_send_logs(
        name: String,
        logger_socket: fidl::Socket,
        mut sender: mpsc::Sender<TestEvent>,
    ) -> Option<BoxFuture<'static, Result<(), anyhow::Error>>> {
        if logger_socket.as_handle_ref().is_invalid() {
            return None;
        }

        let mut ls = match LoggerStream::new(logger_socket) {
            Err(e) => {
                error!("Logger: Failed to create fuchsia async socket: {:?}", e);
                return None;
            }
            Ok(ls) => ls,
        };

        let f = async move {
            while let Some(log) = ls.try_next().await.context("reading log msg")? {
                sender.send(TestEvent::log_message(&name, &log)).await.context("sending log msg")?
            }
            Ok(())
        };

        let (remote, remote_handle) = f.remote_handle();
        fuchsia_async::spawn(remote);
        Some(remote_handle.boxed())
    }

    /// This will wait for all the logs and events to be collected
    pub async fn wait_for_finish(&mut self) -> Result<(), anyhow::Error> {
        if let Some(ref mut f) = self.f.take().as_mut() {
            return Ok(f.await?);
        }
        Ok(())
    }
}

/// Runs the test component using `suite` and collects logs and results.
pub async fn run_and_collect_results(
    suite: fidl_fuchsia_test::SuiteProxy,
    sender: mpsc::Sender<TestEvent>,
    test_filter: Option<&str>,
) -> Result<(), anyhow::Error> {
    debug!("enumerating tests");
    let (case_iterator, server_end) = fidl::endpoints::create_proxy()?;
    let () = suite.get_tests(server_end).map_err(suite_error).context("getting test cases")?;

    let mut invocations = Vec::<Invocation>::new();
    let pattern = test_filter
        .map(|filter| {
            glob::Pattern::new(filter)
                .map_err(|e| anyhow::anyhow!("Bad test filter pattern: {}", e))
        })
        .transpose()?;
    loop {
        let cases =
            case_iterator.get_next().await.map_err(suite_error).context("getting test cases")?;
        if cases.is_empty() {
            break;
        }
        for case in cases {
            let case_name = case.name.unwrap();
            if !pattern.as_ref().map_or(true, |p| p.matches(&case_name)) {
                continue;
            }
            invocations.push(Invocation { name: Some(case_name), tag: None });
        }
    }
    debug!("invocations: {:#?}", invocations);
    run_and_collect_results_for_invocations(suite, sender, invocations).await
}

/// Runs the test component using `suite` and collects logs and results.
pub async fn run_and_collect_results_for_invocations(
    suite: fidl_fuchsia_test::SuiteProxy,
    mut sender: mpsc::Sender<TestEvent>,
    invocations: Vec<Invocation>,
) -> Result<(), anyhow::Error> {
    debug!("running tests");
    let mut successful_completion = true; // will remain true, if there are no tests to run.
    let mut invocations_iter = invocations.into_iter();
    loop {
        const INVOCATIONS_CHUNK: usize = 50;
        let chunk = invocations_iter.by_ref().take(INVOCATIONS_CHUNK).collect::<Vec<_>>();
        if chunk.is_empty() {
            break;
        }
        successful_completion &=
            run_invocations(&suite, chunk, &mut sender).await.context("running test cases")?;
    }
    if successful_completion {
        let () =
            sender.send(TestEvent::test_finished()).await.context("sending TestFinished event")?;
    }
    Ok(())
}

/// Runs the test component using `suite` and collects logs and results.
async fn run_invocations(
    suite: &fidl_fuchsia_test::SuiteProxy,
    invocations: Vec<Invocation>,
    sender: &mut mpsc::Sender<TestEvent>,
) -> Result<bool, anyhow::Error> {
    let (run_listener_client, mut run_listener) =
        fidl::endpoints::create_request_stream::<fidl_fuchsia_test::RunListenerMarker>()
            .context("creating request stream")?;
    suite.run(
        &mut invocations.into_iter().map(|i| i.into()),
        fidl_fuchsia_test::RunOptions {},
        run_listener_client,
    )?;

    let mut test_case_processors = Vec::new();
    let mut successful_completion = false;

    while let Some(result_event) = run_listener.try_next().await.context("waiting for listener")? {
        match result_event {
            OnTestCaseStarted { invocation, primary_log, listener, control_handle: _ } => {
                let name =
                    invocation.name.ok_or(anyhow::anyhow!("cannot find name in invocation"))?;
                sender.send(TestEvent::test_case_started(&name)).await?;
                let test_case_processor = TestCaseProcessor::new(
                    name,
                    listener.into_stream()?,
                    primary_log,
                    sender.clone(),
                );
                test_case_processors.push(test_case_processor);
            }
            OnFinished { .. } => {
                successful_completion = true;
                break;
            }
        }
    }

    // await for all invocations to complete for which test case never completed.
    join_all(test_case_processors.iter_mut().map(|i| i.wait_for_finish()))
        .await
        .into_iter()
        .fold(Ok(()), |acc, r| acc.and_then(|_| r))?;
    Ok(successful_completion)
}

/// Runs v1 test component defined by `test_url` and reports `TestEvent` to sender for each test case.
#[cfg(target_os = "fuchsia")]
pub async fn run_v1_test_component(
    launcher: LauncherProxy,
    test_url: String,
    sender: mpsc::Sender<TestEvent>,
) -> Result<(), anyhow::Error> {
    if !test_url.ends_with(".cmx") {
        return Err(anyhow::anyhow!(
            "Tried to run a component as a v1 test that doesn't have a .cmx extension"
        ));
    }

    debug!("launching test component {}", test_url);
    let app = fuchsia_component::client::launch(&launcher, test_url.clone(), None)
        .with_context(|| format!("Not able to launch v1 test:{}", test_url))?;

    debug!("connecting to test service");
    let suite = app
        .connect_to_service::<fidl_fuchsia_test::SuiteMarker>()
        .context("connecting to service")?;

    run_and_collect_results(suite, sender, None).await?;

    Ok(())
}

/// Runs v2 test component defined by `test_url` and reports `TestEvent` to sender for each test case.
pub async fn run_v2_test_component(
    harness: HarnessProxy,
    test_url: String,
    sender: mpsc::Sender<TestEvent>,
    test_filter: Option<&str>,
) -> Result<(), anyhow::Error> {
    if !test_url.ends_with(".cm") {
        return Err(anyhow::anyhow!(
            "Tried to run a component as a v2 test that doesn't have a .cm extension"
        ));
    }

    let (suite_proxy, suite_server_end) = fidl::endpoints::create_proxy().unwrap();
    let (_controller_proxy, controller_server_end) = fidl::endpoints::create_proxy().unwrap();

    debug!("Launching test component `{}`", test_url);
    let () = harness
        .launch_suite(&test_url, LaunchOptions {}, suite_server_end, controller_server_end)
        .await
        .context("launch_test call failed")?
        .map_err(|e: fidl_fuchsia_test_manager::LaunchError| {
            anyhow::anyhow!("error launching test: {:?}", e)
        })?;

    run_and_collect_results(suite_proxy, sender, test_filter).await
}

fn suite_error(err: fidl::Error) -> anyhow::Error {
    match err {
        // Could get `ClientWrite` or `ClientChannelClosed` error depending on whether the request
        // was sent before or after the channel was closed.
        fidl::Error::ClientWrite(zx_status::Status::PEER_CLOSED)
        | fidl::Error::ClientChannelClosed(_) => anyhow::anyhow!(
            "The test protocol was closed. This may mean `fuchsia.test.Suite` was not \
            configured correctly. Refer to \
            //docs/development/components/troubleshooting.md#troubleshoot-test"
        ),
        err => err.into(),
    }
}

/// The full test coverage of this library lives at //garnet/bin/sl4f/tests/test_framework
/// They test that this library is able to handle various kind of tests launches and able to collect
/// and pass back results.
/// TODO(anmittal): move some of those tests here as unit tests.
#[cfg(test)]
mod tests {
    use super::*;
    use fidl::HandleBased;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn collect_logs() {
        let (sock_server, sock_client) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).expect("Failed while creating socket");

        let name = "test_name";

        let (sender, mut recv) = mpsc::channel(1);

        let fut = TestCaseProcessor::collect_and_send_logs(name.to_string(), sock_client, sender)
            .expect("future should not be None");

        sock_server.write(b"test message 1").expect("Can't write msg to socket");
        sock_server.write(b"test message 2").expect("Can't write msg to socket");
        sock_server.write(b"test message 3").expect("Can't write msg to socket");

        let mut msg = recv.next().await;

        assert_eq!(
            msg,
            Some(TestEvent::log_message(&name, "test message 1test message 2test message 3"))
        );

        // can receive messages multiple times
        sock_server.write(b"test message 4").expect("Can't write msg to socket");
        msg = recv.next().await;

        assert_eq!(msg, Some(TestEvent::log_message(&name, "test message 4")));

        // messages can be read after socket server is closed.
        sock_server.write(b"test message 5").expect("Can't write msg to socket");
        sock_server.into_handle(); // this will drop this handle and close it.
        fut.await.expect("log collection should not fail");

        msg = recv.next().await;

        assert_eq!(msg, Some(TestEvent::log_message(&name, "test message 5")));

        // socket was closed, this should return None
        msg = recv.next().await;
        assert_eq!(msg, None);
    }
}
