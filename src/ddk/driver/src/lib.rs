#[no_mangle]
pub extern fn driver_printf() { }

#[no_mangle]
pub extern fn device_get_protocol() { }

#[no_mangle]
pub extern fn device_remove() { }

#[no_mangle]
pub extern fn device_rebind() { }

#[no_mangle]
pub extern fn device_make_visible() { }

#[no_mangle]
pub extern fn device_get_parent() { }

#[no_mangle]
pub extern fn device_get_name() { }

#[no_mangle]
pub extern fn device_state_clr_set() { }

#[no_mangle]
pub extern fn device_get_size() { }

#[no_mangle]
pub extern fn device_add_from_driver() { }

#[no_mangle]
pub extern fn device_get_metadata() { }

#[no_mangle]
pub extern fn device_add_metadata() { }

#[no_mangle]
pub extern fn device_add_composite() { }

#[no_mangle]
pub extern fn device_publish_metadata() { }

#[no_mangle]
pub extern fn device_get_metadata_size() { }

#[no_mangle]
pub extern fn load_firmware() { }

#[no_mangle]
pub extern fn device_get_profile() { }

#[no_mangle]
pub extern fn get_root_resource() { }
