# zx_clock_update

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Make adjustments to the clock.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_clock_update(zx_handle_t handle,
                            const zx_clock_update_args_t* args);
```

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_CLOCK** and have **ZX_RIGHT_WRITE**.

## DESCRIPTION

TODO(johngro) describe the contents and effects of the args structure.

## RETURN VALUE

On success, returns **ZX_OK**.

## ERRORS

 - **ZX_ERR_BAD_HANDLE** : *handle* is either an invalid handle, or a handle to
   an object type which is not **ZX_OBJ_TYPE_CLOCK**.
 - **ZX_ERR_ACCESS_DENIED** : *handle* lack the **ZX_RIGHT_WRITE** right.
 - **ZX_ERR_INVALID_ARGS** : The update request made is incompatible with the
   properties of the clock.  See the **DESCRIPTION** section for details of
   permissible clock update operations.

## SEE ALSO

 - [clocks](../objects/clock.md)
 - [`zx_clock_create()`]
 - [`zx_clock_get_details()`]
 - [`zx_clock_read()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_clock_create()`]: clock_create.md
[`zx_clock_get_details()`]: clock_get_details.md
[`zx_clock_read()`]: clock_read.md
