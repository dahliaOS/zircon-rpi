// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io';

import 'package:io/ansi.dart';
import 'package:fxtest/fxtest.dart';
import 'package:meta/meta.dart';
import 'package:path/path.dart' as p;

/// Harness for the completely processed tests manifest from a Fuchsia build.
class ParsedManifest {
  /// Tests not matched by supplied arguments.
  final List<TestDefinition> skippedTests;

  /// The raw JSON of a test plopped into a class for structured analysis.
  final List<TestDefinition> testDefinitions;

  /// The runnable wrappers that encapsulate a Fuchsia test.
  final List<TestBundle> testBundles;

  /// Number of test entries in the manifest that would indicate duplicate work.
  final int numDuplicateTests;

  /// Number of test entries that contained data structured outside the bounds
  /// of this script's capabilities. This number should be 0.
  final int numUnparsedTests;

  /// Memoize helper for the derived value [hasDeviceTests].
  bool _hasDeviceTests;

  ParsedManifest({
    @required this.testDefinitions,
    @required this.testBundles,
    this.skippedTests = const [],
    this.numDuplicateTests,
    this.numUnparsedTests,
  });

  @override
  String toString() => '<ParsedManifest ${testBundles.length} matches, '
      '${skippedTests.length} skipped tests / ${testDefinitions.length} total />';

  bool get isEmpty => testBundles.isEmpty;
  bool get isNotEmpty => testBundles.isNotEmpty;
  bool get hasDeviceTests => _hasDeviceTests ??= testBundles
      .any((e) => !hostTestTypes.contains(e.testDefinition.testType));
}

class TestsManifestReader {
  final SingleTestMatcher matcher;

  TestsManifestReader() : matcher = SingleTestMatcher();

  /// Reads and parses the tests manifest file at [manifestFileName]
  Future<List<TestDefinition>> loadTestsJson({
    @required String buildDir,
    @required String fxLocation,
    @required String manifestFileName,
    bool usePackageHash = true,
  }) async {
    List<dynamic> testJson = await readManifest(
      p.join(buildDir, manifestFileName),
    );
    return parseManifest(
      testJson: testJson,
      buildDir: buildDir,
      fxLocation: fxLocation,
    );
  }

  /// Finishes loading the raw test manifest into a list of usable objects.
  List<TestDefinition> parseManifest({
    @required List<dynamic> testJson,
    @required String buildDir,
    @required String fxLocation,
  }) {
    List<TestDefinition> testDefinitions = [];
    for (var data in testJson) {
      TestDefinition testDefinition = TestDefinition.fromJson(
        Map<String, dynamic>.from(data),
        buildDir: buildDir,
      );
      testDefinitions.add(testDefinition);
    }
    return testDefinitions;
  }

  /// Reads the manifest file off disk and parses its content as JSON
  Future<List<dynamic>> readManifest(
    String manifestLocation,
  ) async {
    return jsonDecode(await File(manifestLocation).readAsString());
  }

  /// Handles tests which are unsupported due to never before seen problems.
  void _handleUnsupportedTest(
    TestDefinition testDefinition,
    TestsConfig testsConfig,
    Function(TestEvent) eventEmitter,
  ) {
    String redError = '${testsConfig.wrapWith("Error:", [red])} '
        'Could not parse test:\n$testDefinition';
    if (testsConfig.flags.shouldSilenceUnsupported) {
      if (testsConfig.flags.isVerbose) {
        eventEmitter(TestInfo(redError));
      }
    } else {
      String fxTest = wrapWith('fx test', [blue, styleBold]);
      String dashU = wrapWith('-u', [blue, styleBold]);
      redError += '\n\nThis is very likely a problem with the $fxTest script'
          ' or the test itself, and is not of any error on your part.'
          '\nPlease submit a bug to report this unparsed test to the'
          ' Fuchsia team.\n\nPass the $dashU flag if you would like to'
          ' continue with this error silenced.';
      throw UnparsedTestException(redError);
    }
  }

  /// Loops over the provided list of [TestDefinition]s and, based on the
  /// results of all registered [Checker]s, returns a list of [TestBundle]s.
  ParsedManifest aggregateTests({
    @required TestBundle Function(TestDefinition, [double]) testBundleBuilder,
    @required List<TestDefinition> testDefinitions,
    @required void Function(TestEvent) eventEmitter,
    @required TestsConfig testsConfig,
    Comparer comparer,
    MatchLength matchLength = MatchLength.partial,
  }) {
    comparer ??= StrictComparer();
    List<TestBundle> testBundles = [];
    List<TestDefinition> skippedTests = [];
    Set<String> seenPackages = {};
    bool hasRaisedE2E = false;
    int numDuplicateTests = 0;
    int numUnparsedTests = 0;

    // This triple-loop may seem scary, but we:
    //  1. Always short-circuit once a test has been claimed, and
    //  2. Are dealing low upper-bounds loops
    //      - TestDefinitions (the outer loop) could be long for a
    //        large build, but
    //      - PermutatedFlags (the middle loop) will often be short
    //        (1 to 3 entries), and
    //      - Checkers (the innermost loop) is defined in code and unlikely to
    //        ever exceed a half-dozen
    bool testIsClaimed;
    for (var testDefinition in testDefinitions) {
      // This implies that we encountered a test definition with no code
      // to support its parsing and execution. It definitely implies a critical
      // failure that we should immediately correct.
      if (testDefinition.isUnsupported) {
        numUnparsedTests += 1;
        var testType = testDefinition.testType;
        if (testType == TestType.unsupported) {
          _handleUnsupportedTest(testDefinition, testsConfig, eventEmitter);
          continue;
        } else if (testType == TestType.unsupportedDeviceTest) {
          // Intentional no-op to avoid spammy output.
          // DeviceTests warnings are handled at runtime, meaning if none are
          // matched, a user doesn't have to think or worry about them.
        }
      }

      // TODO: Move this to after an optional `--limit` flag is applied.
      bool isE2E = testDefinition.testType == TestType.e2e;
      if (!isE2E && testsConfig.flags.onlyE2e) {
        continue;
      } else if (isE2E && !testsConfig.flags.e2e) {
        if (!hasRaisedE2E) {
          eventEmitter(TestInfo(
            testsConfig.wrapWith(
              'Found opt-in-only E2E tests. Use `--e2e` flag to enable them.',
              [magenta],
            ),
          ));
          hasRaisedE2E = true;
        }
        continue;
      }

      testIsClaimed = false;
      for (var permutatedTestConfig in testsConfig.permutations) {
        // If a previous TestFlag configuration claimed this test, we definitely
        // don't care whether another would, as well. We don't want to run tests
        // more than once.
        if (testIsClaimed) break;

        var comparisonResult = matcher.evaluateTestAgainstArguments(
          testDefinition,
          permutatedTestConfig,
          matchLength: matchLength,
          comparer: comparer,
        );

        if (comparisonResult.isMatch) {
          // Certain test definitions result in multiple entries in `tests.json`,
          // but invoking the test runner on their shared package name already
          // captures all tests. Therefore, any such sibling entry further down
          // `tests.json` will only result in duplicate work.
          var handle = testDefinition.createExecutionHandle().handle;
          if (seenPackages.contains(handle)) {
            numDuplicateTests += 1;
            testIsClaimed = true;
            break;
          } else {
            seenPackages.add(handle);
          }

          // Now that we know we're seeing this `packageName` for the first
          // time, we can add it to the queue
          testBundles.add(
            testBundleBuilder(
              testDefinition,
              comparisonResult.confidence,
            ),
          );

          // Setting this flag breaks out of the Tier 2 (PermutatedTestFlags)
          // loop
          testIsClaimed = true;
          // Break out of the Tier 3 (Checkers) loop
          break;
        }
      }
      skippedTests.add(testDefinition);
    }

    if (testsConfig.flags.shouldRandomizeTestOrder) {
      testBundles.shuffle();
    }

    return ParsedManifest(
      numDuplicateTests: numDuplicateTests,
      numUnparsedTests: numUnparsedTests,
      skippedTests: skippedTests,
      testDefinitions: testDefinitions,
      testBundles: testBundles,
    );
  }
}
