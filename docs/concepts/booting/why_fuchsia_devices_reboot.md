# Why Fuchsia devices reboot

This document lists why a Fuchsia device may reboot. Some are self-explanatory
while others require some additional context.

Outline:

-   [Terminology](#terminology)
-   [Reboot reasons listed](#reboot-reasons)
-   [Where to find reboot reasons](#where-to-find)

## Terminology {#terminology}

### Ungraceful reboot

An ungraceful reboot is a reboot that is initiated by either the kernel in
response to an error, such as a kernel panic, or performed by the hardware
without software intervention, such as a hardware watchdog timeout.

### Graceful reboot

A graceful reboot is a reboot that is initiated by a userspace process. The
process may initiate the reboot in response to an error, like when a device’s
temperature is too high, but Fuchsia should have the opportunity to undergo an
orderly shutdown.

## Reboot reasons listed {#reboot-reasons}

### Kernel panic

If the kernel is unable to recover from an internal error, that error is
considered fatal and the system will reboot.

### The system runs out of memory

If the kernel detects that the amount of free physical memory falls below an The
kernel does not kill processes in attempt to reclaim memory before rebooting,
meaning a single process could cause a system-wide shortage of memory and force
the device to reboot.

### Cold boot

If a device loses power for long enough between when it is shut down and it
boots back up, the system will determine this to be a cold boot.

### Brownout

A device browns out when its voltage dips below an acceptable threshold. This
should only occur when there is an issue with a device’s power supply or its
power related hardware.

### Hardware watchdog timeout

Zircon sets up a hardware watchdog timer that will reboot the device if it is
not reset within a specified period of time.

### Software watchdog timeout

A software watchdog timer may reboot the device if someone sets one up.

### Brief loss of power

If a device loses power for a short period of time, like when a user unplugs a
device and rapidly plugs it back in, it may be unable to determine that the
reboot was cold and will consider the reboot a result of a brief power loss. It
is important to note that there is _not_ a quantitative measure of what brief is
and is hardware dependent.

### Generic graceful

The platform can know whether the reboot was graceful, but cannot distinguish
between a software update, a user request or some higher-level component
detecting the device as overheating. All the platform knows is that the reboot
was graceful.

### Generic ungraceful

There are some scenarios in which a specific reboot reason cannot be determined,
i.e. we don’t know if it was a kernel panic or a watchdog timeout, but we still
know the reboot was ungraceful.

### Unknown

There are some scenarios in which the platform cannot determine the specific
reboot reason nor can it determine if the reboot was graceful or ungraceful.

## Where to find reboot reasons {#where-to-find}

Fuchsia exposes the reason a device last (re)booted through
[FIDL](/sdk/fidl/fuchsia.feedback/last_reboot_info.fidl) and tracks it on Cobalt
and the crash server.

Reboot reason                | __FIDL__                      | __Cobalt event__          | __Crash signature__
:--------------------------- | :---------------------------- | :------------------------ | :------------------
Kernel panic                 | `KERNEL_PANIC`                | `KernelPanic`             | `fuchsia-kernel-panic`
System running out of memory | `SYSTEM_OUT_OF_MEMORY`        | `SystemOutOfMemory`       | `fuchsia-oom`
Cold boot                    | `COLD`                        | `Cold`                    | N/A\*
Brownout                     | `BROWNOUT`                    | `Brownout`                | `fuchsia-brownout`
Hardware watchdog timeout    | `HARDWARE_WATCHDOG_TIMEOUT`   | `HardwareWatchdogTimeout` | `fuchsia-hw-watchdog-timeout`
Software watchdog timeout    | `SOFTWARE_WATCHDOG_TIMEOUT`   | `SoftwareWatchdogTimeout` | `fuchsia-sw-watchdog-timeout`
Brief power loss             | `BRIEF POWER LOSS`            | `BriefPowerLoss`          | `fuchsia-brief-power-loss`
Generic graceful             | *graceful* field set to true  | `GenericGraceful`         | N/A\*
Generic ungraceful           | *graceful* field set to false | `GenericUngraceful`       | N/A\*\*
Unknown                      | *graceful* field not set      | `Unknown`                 | N/A\*\*

\* Not a crash. \
\*\* Currently not implemented.
