# zx_clock_get_details

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Fetch all of the low level details of the clock's current status.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_clock_get_details(zx_handle_t handle,
                                 zx_clock_details_t* details);
```

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_CLOCK** and have **ZX_RIGHT_READ**.

## DESCRIPTION

TODO(johngro) describe the contents of the details structure.

## RETURN VALUE

On success, returns **ZX_OK** along with clock details stored in the *details*
out parameter.

## ERRORS

 - **ZX_ERR_BAD_HANDLE** : *handle* is either an invalid handle, or a handle to
   an object type which is not **ZX_OBJ_TYPE_CLOCK**.
 - **ZX_ERR_ACCESS_DENIED** : *handle* lack the **ZX_RIGHT_READ** right.

## SEE ALSO

 - [clock transformations](clock_transformations.md)
 - [clocks](../objects/clock.md)
 - [`zx_clock_create()`]
 - [`zx_clock_read()`]
 - [`zx_clock_update()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_clock_create()`]: clock_create.md
[`zx_clock_read()`]: clock_read.md
[`zx_clock_update()`]: clock_update.md
