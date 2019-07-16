# zx_clock_create

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Create a new clock object.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_clock_create(uint32_t options, zx_handle_t* out);
```

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

None.

## DESCRIPTION

Creates a new zircon clock object.  See [clocks] for an
overview of clock objects.

### Options

Two options are currently defined for clock objects.

+ **ZX_CLOCK_OPT_MONOTONIC** : When set, creates a clock object which is
  guaranteed to never run backwards.  Monotonic clocks must always move forward.
+ **ZX_CLOCK_OPT_CONTINUOUS** : When set, creates a clock which is guaranteed to
  never jump either forwards or backwards.  Continuous clocks may only be
  maintained using frequency adjustments and are, by definition, also monotonic.
  Attempting to create a clock object with the **ZX_CLOCK_OPT_CONTINUOUS** option
  specified, but without the **ZX_CLOCK_OPT_MONOTONIC** option specified is an
  error which will be signalled with **ZX_ERR_INVALID_ARGS**.

## RETURN VALUE

On success, returns **ZX_OK** along with a new clock object via the *out*
handle.  Handles to newly created clock objects will have the **ZX_RIGHT_READ**
and **ZX_RIGHT_WRITE** rights assigned to them.

## ERRORS

 - **ZX_ERR_INVALID_ARGS**  Either an invalid option flag was specified, or
   **ZX_CLOCK_OPT_CONTINUOUS** was specified without also specifying
   **ZX_CLOCK_OPT_MONOTONIC**.
 - **ZX_ERR_NO_MEMORY**  Failure due to lack of memory.

## SEE ALSO

 - [clocks](../objects/clock.md)
 - [`zx_clock_get_details()`]
 - [`zx_clock_read()`]
 - [`zx_clock_update()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_clock_get_details()`]: clock_get_details.md
[`zx_clock_read()`]: clock_read.md
[`zx_clock_update()`]: clock_update.md
