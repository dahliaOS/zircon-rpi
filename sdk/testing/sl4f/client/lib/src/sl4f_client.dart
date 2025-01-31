// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is named sl4f_client.dart since the top-level dart file is names
// sl4f.dart, which would cause problems for src files trying to import this
// file.

import 'dart:async';
import 'dart:convert';
import 'dart:core';
import 'dart:io';

import 'package:http/http.dart' as http;
import 'package:logging/logging.dart';
import 'package:pedantic/pedantic.dart';

import 'dump.dart';
import 'exceptions.dart';
import 'ssh.dart';

final _log = Logger('sl4f_client');

/// Diagnostics have been known to hang in some failure cases. In those cases, log
/// and move on.
const _diagnosticTimeout = Duration(minutes: 2);

bool _isNullOrEmpty(String str) => str == null || str.isEmpty;

/// Handles the SL4F server and communication with it.
class Sl4f {
  static const diagnostics = {
    'kstats': 'kstats -c -m -n 1',
    'net-if': 'net if list',
    'ps': 'ps -T',
    'top': 'top -n 1',
    'wlan': 'wlan status',
  };
  static const _sl4fComponentUrl =
      'fuchsia-pkg://fuchsia.com/sl4f#meta/sl4f.cmx';
  static const _sl4fComponentName = 'sl4f.cmx';
  static const _sl4fHttpDefaultPort = 80;
  static final _portSuffixRe = RegExp(r':\d+$');

  final _client = http.Client();

  /// Authority (IP, hostname, etc.) of the device under test.
  final String target;

  /// TCP port that the SL4F HTTP server is listening on the target device
  final int port;
  final Ssh ssh;

  Sl4f(this.target, this.ssh, [this.port = _sl4fHttpDefaultPort])
      : assert(target != null && target.isNotEmpty) {
    if (_portSuffixRe.hasMatch(target)) {
      throw Sl4fException('Target argument cannot contain a port. '
          'Use the port argument instead.');
    }

    _log.info('Target device: $target');
  }

  /// Constructs an SL4F client from the following environment variables:
  /// * `FUCHSIA_DEVICE_ADDR`, `FUCHSIA_IPV4_ADDR`, or `FUCHSIA_IPV6_ADDR`:
  ///       IPV4 or IPV6 address of the target address to be tested (required).
  /// * `SL4F_HTTP_PORT`: TCP port that the SL4F HTTP server is listening on
  ///       the target device (optional, defaults to $_sl4fHttpDefaultPort)
  /// * `FUCHSIA_SSH_PORT`: SSH port of the target device (optional).
  /// * `FUCHSIA_SSH_KEY`: Path of the SSH private key (required if
  ///       `SSH_AUTH_SOCK` is not set).
  ///
  /// The environment variables specifying the IP address will be checked in
  /// the order `FUCHSIA_DEVICE_ADDR`, then `FUCHSIA_IPV4_ADDR`, then
  /// `FUCHSIA_IPV6_ADDR`.
  ///
  /// If `FUCHSIA_SSH_KEY` is not set but `SSH_AUTH_SOCK` is, then it's
  /// assumed that ssh-agent can provide the credentials to connect to the
  /// device. Otherwise an [Sl4fException] is thrown.
  factory Sl4f.fromEnvironment({Map<String, String> environment}) {
    environment ??= Platform.environment;
    final address = environment['FUCHSIA_DEVICE_ADDR'] ??
        environment['FUCHSIA_IPV4_ADDR'] ??
        environment['FUCHSIA_IPV6_ADDR'];
    if (_isNullOrEmpty(address)) {
      throw Sl4fException(
          'No FUCHSIA_DEVICE_ADDR, FUCHSIA_IPV4_ADDR, or FUCHSIA_IPV6_ADDR '
          'provided when starting SL4F from env');
    }

    Ssh ssh;
    int sshPort;
    if (!_isNullOrEmpty(environment['FUCHSIA_SSH_PORT'])) {
      sshPort = int.tryParse(environment['FUCHSIA_SSH_PORT']);
      if (sshPort == null || sshPort <= 0) {
        throw Sl4fException('Invalid FUCHSIA_SSH_PORT: $sshPort');
      }
    }
    if (!_isNullOrEmpty(environment['FUCHSIA_SSH_KEY'])) {
      ssh = Ssh(address, environment['FUCHSIA_SSH_KEY'], sshPort);
    } else if (!_isNullOrEmpty(environment['SSH_AUTH_SOCK'])) {
      ssh = Ssh.useAgent(address, sshPort);
    } else {
      throw Sl4fException(
          'No FUCHSIA_SSH_KEY provided and SSH_AUTH_SOCK is not defined. '
          'Cannot start sl4f.');
    }

    String host = address;
    // This same code exists in the dart/sdk/lib/_http/http_impl.dart.
    if (host.contains(':')) {
      host = '[$host]';
    }

    int port = _sl4fHttpDefaultPort;
    if (!_isNullOrEmpty(environment['SL4F_HTTP_PORT'])) {
      port = int.parse(environment['SL4F_HTTP_PORT']);
    }

    return Sl4f(host, ssh, port);
  }

  /// Get the IP address that the device under test will use to reach the test
  /// runner (the 'Host').
  ///
  /// This similar to running `fx netaddr --local` because fx is not available
  /// in all test environments.
  ///
  /// Throws a [Sl4fException] if the host IP address cannot be determined.
  Future<String> hostIpAddress() async {
    // TODO(fxb/52924): Replace the body of this function with just the host ip
    // env variable (which could be set in Sl4f.fromEnvironment(), above). Or,
    // try that env var and use the following as a fallback.

    final result =
        await ssh.runWithOutput('echo \$SSH_CLIENT~\$SSH_CONNECTION');
    final strings = await result.stdout?.split('~');
    if (strings == null || strings.length < 2) {
      throw Sl4fException('Unable to retrieve host IP address strings.');
    }
    var ipAddr = strings[0].split(' ')[0];
    if (ipAddr.isEmpty) {
      ipAddr = strings[1].split(' ')[0];
    }
    if (ipAddr.isEmpty) {
      throw Sl4fException('Unable to retrieve host IP address.');
    }
    return ipAddr;
  }

  /// Closes the underlying HTTP client.
  ///
  /// If clients remain unclosed, the dart process might not terminate.
  void close() {
    _client.close();
  }

  /// Sends a JSON-RPC request to SL4F.
  ///
  /// Throws a [JsonRpcException] if the SL4F server replied with a non-null
  /// error string.
  Future<dynamic> request(String method, [dynamic params]) async {
    // Although params is optional, this will pass a null params if it is
    // omitted. This is actually required by our SL4F server (although it is
    // not required in JSON RPC:
    // https://www.jsonrpc.org/specification#request_object).
    final httpRequest = http.Request('GET', Uri.http('$target:$port', ''))
      ..body = jsonEncode({'id': '', 'method': method, 'params': params});

    final httpResponse =
        await http.Response.fromStream(await _client.send(httpRequest));
    Map<String, dynamic> response = jsonDecode(httpResponse.body);
    final dynamic error = response['error'];
    if (error != null) {
      throw JsonRpcException(httpRequest.body, error);
    }

    return response['result'];
  }

  /// Starts the SL4F server on the target using ssh.
  ///
  /// It will attempt to connect [tries] times, waiting at least [delay]
  /// between each attempt.
  /// Throws a [Sl4fException] if after this SL4F failed to start.
  Future<void> startServer(
      {int tries = 150, Duration delay = const Duration(seconds: 1)}) async {
    if (tries <= 0) {
      throw ArgumentError.value(tries, 'tries', 'Must be a positive integer');
    }

    for (var attempt = 0; attempt < tries; attempt++) {
      if (attempt > 0) {
        // TODO(isma): We should limit the wait time to as much delay.
        await Future.delayed(delay);
      }

      // Check if it's already started.
      if (await isRunning()) {
        _log.info('SL4F has started.');
        return;
      }

      _log.info('Try $attempt at starting sl4f.');
      // We run sl4f with `-d` to make sure that it keeps running even
      // if sshd dies or the connection somehow breaks.
      //
      // We apparently cannot rely on the ssh connection to stay open
      // indefinitely for as long as sl4f is running.
      //
      // This has the consequence that we won't get an error in the logs
      // if sl4f.cmx isn't available. This shouldn't be an issue if the
      // users start the test with the given instructions, especially
      // using the `end_to_end_deps` bundle, but it could still happen
      // if something gets misconfigured in the product build config.
      //
      // The run program launches the specified component with stdout and stderr
      // cloned from its own environment, even when -d is specified. Ssh hooks
      // into those standard file descriptors and waits for those hooks to close
      // before exiting, so with a simple run command it effectively waits on
      // the daemonized component, which would cause a hang here if we awaited
      // it. By redirecting stdout and stderr to /dev/null, we make it so that
      // ssh does not wait for them to close, and thus we can await here safely.
      await ssh.run('run -d $_sl4fComponentUrl > /dev/null 2> /dev/null');

      if (await isRunning(tries: 3, delay: Duration(seconds: 2))) {
        _log.info('SL4F has started.');
        return;
      }
    }
    throw Sl4fException('Sl4f has not started.');
  }

  /// SSHs into the device and starts the SL4F server.
  ///
  /// If no ssh key path is given, it's taken from the FUCHSIA_SSH_KEY env var.
  Future<void> stopServer() async {
    if ((await ssh.run('killall $_sl4fComponentName')).exitCode != 0) {
      _log.warning('Could not stop sl4f. Continuing.');
    }
  }

  /// Restarts the device under test.
  ///
  /// Throws an Sl4fException if it fails to reboot the device or if all the
  /// attempts at restarting SL4F fail.  Returns a duration showing how long
  /// it took to issue a command until a reboot was detected.
  Future<Duration> reboot() async {
    _log.info('Initiating reboot sequence.');
    // Kill SL4F first, we'll use it to try to guess when the reboot is done.
    await stopServer();
    // Issue a reboot command and wait.
    // TODO(DNO-621): trap errors
    final Stopwatch rebootStopwatch = Stopwatch()..start();
    await ssh.run('dm reboot');
    await Future.delayed(Duration(seconds: 20));

    // Try to restart SL4F
    await startServer();
    rebootStopwatch.stop();
    return rebootStopwatch.elapsed;
  }

  /// Dumps files with useful device diagnostics.
  Future<void> dumpDiagnostics(String dumpName, {Dump dump}) async {
    final dumper = dump ?? Dump();
    if (dumper.hasDumpDirectory) {
      await Future.wait(diagnostics.entries.map((diag) => _dumpDiagnostic(
          diag.value, '$dumpName-diagnostic-${diag.key}', dumper)));
    }
  }

  Future _dumpDiagnostic(String cmd, String dumpName, Dump dump) async {
    final proc = await ssh.start(cmd);
    await proc.stdin.close();

    // Pipe stdout directly to the file sink (it gets closed when done).
    final sink = dump.openForWrite(dumpName, 'txt');
    // Create completers to signal when we are done listening to stdout and
    // stderr. This is an unfortunate necessity because cancelling a
    // StreamSubscription does not create a done signal, so (for example)
    // stdoutSubscription.asFuture would never complete.
    final stdoutCompleter = Completer();
    final stderrCompleter = Completer();
    // We can't use Stream.pipe here because it creates a stream subscription
    // that does not get cancelled if proc.stdout stops producing events without
    // closing and we would have no way of cancelling it in our timeout
    // handling.
    final stdoutSubscription =
        proc.stdout.listen(sink.add, onDone: stdoutCompleter.complete);

    // Start decoding the stderr stream to ensure something consumes it,
    // otherwise it could cause dart to hang waiting for it to be consumed.
    // We can't use systemEncoding.decodeStream here because it creates a stream
    // subscription that we have no way to cancel in the case that the stream
    // stops producing values without closing.
    final stderrController = StreamController<List<int>>();
    final stderrSubscription = proc.stderr
        .listen(stderrController.add, onDone: stderrCompleter.complete);
    final stderr = systemEncoding.decodeStream(stderrController.stream);

    final finishDiagnostic = () async {
      final code = await proc.exitCode;
      await stdoutCompleter.future;
      await stderrCompleter.future;
      await stderrController.close();
      // Awaiting is not strictly necessary in the case where the process exits
      // with code 0, but this makes it clear that our intention is that the
      // stderr future completes here, and if that stops being true then we'll
      // observe a hang in the particular test where it fails rather than at the
      // end of the full run.
      final stderrData = await stderr;
      // Print something about the process in case it fails.
      if (code != 0) {
        _log..warning('$cmd; exit code: $code')..warning('stderr: $stderrData');
      }
      await sink.close();
    }();

    // Set up a future that will kill the ssh process if it takes too long.
    unawaited(finishDiagnostic.timeout(_diagnosticTimeout, onTimeout: () async {
      _log.warning('$cmd; did not complete after $_diagnosticTimeout');
      proc.kill(ProcessSignal.sigkill);
      await stdoutSubscription.cancel();
      stdoutCompleter.complete();
      await stderrSubscription.cancel();
      stderrCompleter.complete();
    }));

    return finishDiagnostic;
  }

  /// Sends an empty http request to the server to verify if it's listening on
  /// [port].
  ///
  /// By default it tries to connect just once, but that can be changed with
  /// [tries]. In which case it will wait [delay] time between tries.
  /// The server must respond within [timeout] before its considered to be
  /// unreachable.
  Future<bool> isRunning(
      {int tries = 1,
      Duration delay = const Duration(seconds: 2),
      Duration timeout = const Duration(seconds: 10)}) async {
    if (tries <= 0) {
      throw ArgumentError.value(tries, 'tries', 'Must be a positive integer');
    }
    // TODO(isma): We should refactor the retry logic into its own higher-order
    // function.
    for (var attempt = 0; attempt < tries; attempt++) {
      if (attempt > 0) {
        await Future.delayed(delay);
      }

      try {
        await http.get(Uri.http('$target:$port', '/')).timeout(timeout);
      } on IOException {
        continue;
      } on TimeoutException {
        continue;
      } on http.ClientException {
        continue;
      }
      return true;
    }
    return false;
  }
}
