# Clock

## NAME

clock - Kernel object used to track the progress of time.

## SYNOPSIS

A clock is a one dimensional affine transformation of the clock monotonic
timeline which may be atomically adjusted by a clock maintainer, and observed by
clients.

## DESCRIPTION

### Properties

The properties of a clock are established when the clock is created and cannot
be changed afterwards.  Currently, two clock properties are defined.

#### **ZX_CLOCK_OPT_MONOTONIC**

When set, the clock is guaranteed to have monotonic behavior.  This is to say
that any sequence of observations of the clock is guaranteed to produce a
sequence of times which are always greater than or equal to the previous
observations.  A monotonic clock can never go backwards, although it _can_ jump
forwards.  Formally...

Given a clock _C_, Let C(x) be the function which maps from the reference
timeline _C's_ timeline.  C(x) is a piecewise linear function made up of all the
affine transformation segments over all time as determined  by _C's_ maintainer.
_C_ is monotonic if and only if...

for all _R<sub>1</sub>_, _R<sub>2</sub>_ : _R<sub>2</sub> >= R<sub>1</sub>_

_C(R<sub>2</sub>) >= C(R<sub>2</sub>)_

#### **ZX_CLOCK_OPT_CONTINUOUS**

When set, the clock is guaranteed to have continuous behavior.  This is to say
that any update to the clock transformation is guaranteed to be first order
continuous with the previous transformation segment.  Formally...

Let _C<sub>i</sub>(x)_ be the _i<sub>th</sub>_ affine transformation segment of
_C(x)_.  Let _R<sub>i</sub>_ be the first point in time on the reference timeline
for which _C<sub>i</sub>(x)_ is defined.  A clock _C_ is continuous if and only
if: for all _i_...

_C<sub>i</sub>(R<sub>i + 1</sub>x) = C<sub>i + 1</sub>(R<sub>i + 1</sub>x)_

### Implied properties

+ The reference clock for all clock objects in the system is clock monotonic.
+ The nominal units of all clock objects are specified to be nanoseconds.  This
  property is not configurable.
+ The units of frequency adjustment for all clock objects are specified to be
  parts per million, or PPM.
+ The maximum permissible range of frequency adjustment of a clock object is
  specified to be [-1000, +1000] PPM.  This property is not configurable.

### Reading the clock

Given a clock handle, users may query the current time given by that clock using
the `zx_clock_read()` syscall.  Clock reads **ZX_RIGHT_READ** permissions.  Clock
reads are guaranteed to be coherent for all observers.  This is to say that, if
two observers query the clock at exactly the same reference time _R_, that they
will always see the same value _C(R)_.

### Fetching the clock's details

In addition to simply reading the current value of the clock, advanced users who
possess **ZX_RIGHT_READ** permissions may also read the clock and get extended
details in the process using `zx_clock_get_details()`.  Upon a successful call,
the details structure returned to callers will include...

+ The current clock monotonic to clock transformation.
+ The current tick-counter to clock transformation.
+ The current symmetric error estimate (if any) for the clock.
+ A generation nonce.
+ The last time the clock was updated as defined by the clock monotonic
  reference timeline.
+ An observation of the system tick counter which was taken during the
  observation of the clock.

Advanced users may use these details to not only compute a recent `now` value
for the clock (by running the tick counter through the ticks-to-clock
transformation), but to also...

+ Know whether the clock transformation has been changed since the last
  `zx_clock_get_details()` operation (using the generation nonce).
+ Compose the clock transformation with other clocks' transformations to reason
  about the relationship between two clocks.
+ Know the clock maintainer's best estimate of absolute error.
+ Reason about the range of possible future values of the clock relative to the
  reference clock based on the last correction time, the current transformation,
  and the maximum permissible correction factor for the clock (see the maximum
  permissive range of frequency adjustment described in the |Implied properties|
  section above.

### Maintaining a clock.

Users who possible **ZX_RIGHT_WRITE** permissions for a clock object may act as a
maintainer of the clock using the `zx_clock_update()` syscall.  Three parameters
of the clock may be adjusted during each call to `zx_clock_update()`, but not
all three need to be adjusted each time.  These values include...

+ The clock's absolute value.
+ The frequency adjustment of the clock (deviation from nominal expressed in
  ppm)
+ The absolute error estimate of the clock (expressed in nanoseconds)

Changes to a clocks transformation occur during the syscall itself.  The
specific reference time of the adjustment may not be specified by the user.  In
addition, any change to the absolute value of a clock with the
**ZX_CLOCK_OPT_MONOTONIC** property set on it which would result in non-monotonic
behavior will fail with a return code of **ZX_ERR_INVALID_ARGS**.  In addition,
aside from the very first set operation, all attempts to set the absolute value
of a clock with the **ZX_CLOCK_OPT_CONTINUOUS** property set on it will fail with
a return code of **ZX_ERR_INVALID_ARGS**

## SYSCALLS

 - [clock transformations](clock_transformations.md)
 - [`zx_clock_create()`] - create a clock
 - [`zx_clock_read()`] - read the time of the clock
 - [`zx_clock_get_details()`] - fetch the details of a clock's relationship to clock monotonic
 - [`zx_clock_update()`] - adjust the current relationship of a clock to the clock monotonic reference.

## SEE ALSO

[`zx_clock_create()`]: ../syscalls/clock_create.md
[`zx_clock_read()`]: ../syscalls/clock_read.md
[`zx_clock_get_details()`]: ../syscalls/clock_get_details.md
[`zx_clock_update()`]: ../syscalls/clock_update.md
