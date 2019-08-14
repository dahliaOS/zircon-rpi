use {
    fuchsia_ddk_sys as ddk,
    log::*,
    fuchsia_zircon as zx,
};

#[no_mangle]
pub extern fn device_add_from_driver(
    drv: *mut ddk::zx_driver_t,
    parent: *mut ddk::zx_device_t,
    args: *mut ddk::device_add_args_t,
    out: *mut *mut ddk::zx_device_t) -> zx::sys::zx_status_t {

    warn!("CALLED RUST DEVHOST\n $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ \n !!!!!!\n");

    3
}

