# Triage codelab

Contributors: cphoenix@

This codelab explains the Triage utility:

* What it's for.
* How to run it, including command line options.
* How to add and test configuration rules to detect problems in Fuchsia
bugreports.

The source files and examples to which this document refers are available at:

* [//src/diagnostics/examples/triage/bugreport/inspect.json][triage-inspect-example].
* [//src/diagnostics/examples/triage/rules.triage][triage-rules-example].
* [//src/diagnostics/examples/triage/solution][triage-codelab-solution].

## What is Triage?

Triage allows you to scan bug dump files (bugreport.zip,
fuchsia_feedback_data.zip) for predefined conditions.

The Triage system makes it easy to configure new conditions, increasing the
usefulness of Triage for everyone.

## Prerequisites

Before you start on this codelab, make sure you have completed the following:

* [Getting started](/docs/getting_started.md).
* Made a Fuchsia build with `fx build`.

## Running Triage from the command line

* To run Triage:

```shell
fx triage
```

This command downloads a fresh `bugreport.zip` file using the `fx bugreport`
command. This command runs the default rules which are located in
`//src/diagnostics/config/triage/*.triage`.

To analyze a specific bugreport.zip or fuchsia_feedback_data.zip file, use
`--data`.

* You can specify at most one `--data` argument.
* The argument to `--data` can also be a path to a directory containing
an unzipped bugreport.
* If you run `fx triage` without specifying a `--data` option, it
runs a fresh `fx bugreport` and analyzes its `inspect.json` file.

```shell
$ fx triage --data my/foo/bugreport.zip
```

To use a specific configuration file or all `*.triage` files in a specific
directory, use `--config`.

* You can use multiple `--config` arguments.
* If a `--config` argument is used, the default rules will not be
automatically loaded.

```shell
fx triage --config my/directory --config my/file.triage
```

## Adding Triage rules

The rest of this codelab explains how to configure new behavior in Triage.

### Overview

Triage condenses the mass of Inspect data into useful information, through the
following steps:

1. Select values from the `inspect.json` file using _selector_ strings in the
`select` section of the config file.
1. Perform computations and comparisons to generate new values, as specified
in the `eval` section of the config file.
1. Take actions depending on Boolean values, according to entries in the `act`
section of the config file.
1. Support unit-testing the actions via entries in the `test` section.

### Find the codelab's sample files

Navigate to the `src/diagnostics/examples/triage` directory in your source tree.
The following command is intended to run from that directory:

```shell
fx triage --config . --data bugreport
```

Running this command in the sample directory with the unmodified codelab files
prints a line indicating that Triage is working properly:

```none
Warning: 'always_triggered' in 'rules' detected 'Triage is running': 'always_true' was true
```

#### inspect.json

This codelab includes an `inspect.json` file with Inspect data to make the
exercises work predictably. This file is in the sample directory under
`bugreport/inspect.json`.

Note: `inspect.json` files are normally packaged in the `bugreport.zip` file
produced by `fx bugreport`. Either use `unzip` to unpack these files
or give the .zip file as the argument to `--data`. For this codelab the
bugreport has already been unzipped.

#### rules.triage

The Triage program uses configuration loaded from one or more .triage files.
This codelab uses the `rules.triage` file located in the sample directory.

### Add selectors for the Inspect values

The `inspect.json` file in the sample directory indicates a couple of problems
with the system. You're going to configure the triage system to detect those
problems.

This step configures Triage to extract values from the data in the
`inspect.json` file.

The `rules.triage` file contains a key-value section called `select`.
Each key (name) can be used in the body of other config entries. Each value
is a selector string. In effect, each entry in the `select` section (and the
`eval` section, described below) defines a variable.

The selector string is a colon-separated
string that tells where in the Inspect data to find the value you need.

```json5
select: {
    // "global_dat" is an intentional typo to fix later in the codelab.
    disk_used: "INSPECT:global_dat/storage:root/stats:used_bytes",
}
```

Inspect data published by a component is organized as a tree of
nodes with values (properties) at the leaves. The `inspect.json` file is an
array of these trees, each with a moniker that identifies the source component.

The portion of the selector string between the `INSPECT:` and the second colon
should match one of the moniker strings in the `inspect.json` file.

The portion between the second and third colons is a `/`-separated list of
node names.

The portion after the last colon is the property name.

The above selector string indicates a component whose `moniker` contains the
string `global_dat/storage`. It also indicates the `used_bytes` property from
the `stats` subnode of the `root` node of that component's Inspect Tree.

1. Copy the above "disk_used" selector, and add it to the "select"
section of the rules.triage file.

2. Write and add another selector named "disk_total" to select the "total_bytes"
property at the same node in the Inspect data.

.triage files use JSON5 which is easier to write and read than JSON:

*    It's good style to put a comma after the last list item.
*    Most keys (including all valid Triage names) don't need to be
wrapped in quotation marks.
*    /* Multiline */ and // single comments can be used.

### Add a computation

In addition to selecting values from the `inspect.json` file, you need to do
some logic, and probably some arithmetic, to see whether those values indicate
a condition worth flagging.

Copy and add the following line to the `eval` section of the
`rules.triage` file to calculate how full the disk is:

```json5
eval: {
    ....
    disk_percentage: "disk_used / disk_total",
}
```

`eval` entries use ordinary infix math expressions. See the [Details](#details)
section for more information.

### Add an action

In the "act" part of the config file, add an action which prints a
warning when the disk is 98% full. Use the following lines:

```json
act: {
    ...
    "disk_full": {
        "trigger": "disk_percentage > 0.98",
        "print": "Disk is over 98% full",
    },
}
```

Note the following:

*   The "trigger" is an expression that evaluates to a Boolean value. This may be
    the name of a Boolean-type selector or computation, or any suitable math
    expression.
*   See the [Details](#details) section for more information about comparisons.
*   Currently, `print` is the only available action.

### Try it out

The following command will run Triage against the local config file.

```shell
fx triage --config . --data bugreport
```

You will get an error that looks like the following:

```none
[ERROR] In config 'rules': No value found matching selector global_dat/storage:root/stats:used_bytes
```

There was a typo in the selector rules.
Triage could not find values needed to evaluate a rule. In fact, the correct
selector is "global_data" not "global_dat." Fix it in your selector rules
and try again.

```shell
fx triage --config . --data bugreport
```

Now what happened? Nothing, right? So, how do you know whether there was no
problem in the `inspect.json` file, or a bug in your rule?

### Test your rule

You can (and should!) add tests for your actions. For each test, write a snippet
of Inspect-format content and specify whether it should or should not trigger
your rule.

To test the rule you've added, add the following to the `test` section of the
`rules.triage` file:

```json5
test: {
    ....
    is_full: {
        yes: ["disk_full"],
        no: [],
        inspect: [
            {
                moniker: "global_data/storage",
                payload: {root: {stats: {
                    total_bytes: 100, used_bytes: 98}}}
            }
        ]
    }
}
```

You can also test conditions in which actions should not trigger:

```json5
test: {
    ....
    not_full: {
        yes: [],
        no: ["disk_full"],
        inspect: [
            {
                moniker: "global_data/storage",
                payload: {root: {stats: {
                    total_bytes: 100, used_bytes: 97}}}
            }
        ]
    }
}
```

To run the test, just run Triage. It automatically self-tests each time it's
run.

```shell
fx triage --config . --data bugreport
```

Whoops! That should signal an error:

`Test is_full failed: trigger disk98 of action disk_full returned Bool(false), expected true`

### Fix your rule

You want to trigger when the disk is 98% or more full, but that's
not quite what you wrote, and your test caught the problem.
Modify the `>` in your action to be a `>=`:

```json5
        "trigger": "disk_percentage >= 0.98",
```

Run Triage again. The error should disappear, replaced by a warning that your
`inspect.json` file does in fact indicate a full disk.

`Warning: 'disk_full' in 'rules' detected 'Disk is 98% full': 'disk98' was true`

### Use multiple configuration files

You can add any number of Triage configuration files, and even use variables
defined in one file in another file. This has lots of applications:

* One file for disk-related variables and actions, and another for
network-related variables and actions.
* A file to define product-specific numbers.
* Separate files for particular engineers or teams.

Add a file "product.triage" containing the following:

```json5
{
    eval: {
        max_widgets: "4",
    },
}
```

Note the following:

*   Empty sections may be omitted from .triage files. This file
    contains no `select`, `act`, or `test` entries.
*   Although numeric values in JSON are not quoted, this is an expression string
    so it does need to be quoted.

Add the following entries to the rules.triage file:

```json5
select: {
    ...
    actual_widgets: "widget_maker.cmx:root:total_widgets",
}
```

That will extract how many widgets were active in the device.

```json5
eval: {
    ...
    too_many_widgets: "actual_widgets > product::max_widgets",
```

That compares the actual widgets with the theoretical maximum for the
product.

Note: To use variable names from another file, combine the file name, two
colons, and the variable name.

Finally, add an action:

```json5
act: {
    ...
    widget_overflow: {
        trigger: "too_many_widgets",
        print: "Too many widgets!",
    },
}
```

Unfortunately, this device tried to use 6 widgets, so this warning should
trigger when "fx triage" is run.

Note: The `trigger` of an action can also use `file::name` syntax to refer to a
variable from another file.

In a production environment, several "product.triage" files could be
maintained in different directories, and Triage could be directed to use any of
them with the "--config" command line argument. (Future versions of Triage
may be able to select the correct product file automatically.)

### Details {#details}

#### Names

Names (of selectors, expressions, actions, and tests, as well as the
basenames of config files) can be any letter or underscore, followed by any
number of letters, numbers, or underscores.

Names beginning with underscores may have special meaning
in future versions of Triage. They're not forbidden, but it's
best to avoid them.

The name of each .triage file establishes its namespace. Loading two .triage
files with the same name from different directories is not allowed.

#### Math expressions

* Variables can be 64-bit float, signed 64-bit int, or Boolean.
* Arithmetic expressions use `+ - * / //` operators, with ordinary order
and precedence of operations.
* The division operator `/` produces a float value.
* The division operator `//` produces an int value, truncating the result
toward 0, even with float arguments. (Note this is different
from Python 3 where // truncates downward.)
* `+ - *` preserve the type of their operands (mixed promotes to float).
* Comparison operators are `> >= < <= == !=`
* Comparisons have Boolean result type and can be used to trigger actions.
* You can combine computations and comparisons in a single `eval` rule.
* You can use parentheses.
* You can use the key names of `eval` and `select` entries as variables.
* Spaces are optional everywhere, and allowed everywhere except inside
`filename::variable` namespaced variables.

#### Predefined functions

Triage provides predefined functions for use in `eval` expressions:

* Max(value1, value2, value3...) returns the largest value, with type promotion
to float.
* Min(value1, value2, value3...) returns the smallest value, with type
promotion to float.
* And(value1, value2, value3...) takes Boolean arguments and returns the
logical AND of the values.
* Or(value1, value2, value3...) takes Boolean arguments and returns the
logical OR of the values.
* Not(value) takes one Boolean argument and returns the logical NOT of it.

## Further Reading

See
[Triage (fx triage)](/src/diagnostics/triage/README.md)
for the latest features and options - Triage will keep improving!

[triage-inspect-example]: /src/diagnostics/examples/triage/bugreport/inspect.json
[triage-rules-example]: /src/diagnostics/examples/triage/rules.triage
[triage-codelab-solution]: /src/diagnostics/examples/triage/solution

