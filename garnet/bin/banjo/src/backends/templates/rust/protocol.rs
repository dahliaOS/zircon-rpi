#[repr(C)]
pub struct {protocol_name}_ops_t {{
{protocol_fns}
}}

#[repr(C)]
pub struct {protocol_name}Protocol {{
    pub ops: *mut {protocol_name}_ops_t,
    pub ctx: *mut u8,
}}

impl Default for {protocol_name}Protocol {{
    fn default() -> Self {{
        {protocol_name}Protocol {{
            ops: core::ptr::null_mut(),
            ctx: core::ptr::null_mut(),
        }}
    }}
}}

impl {protocol_name}Protocol {{
    pub fn from_device(parent_device: &ddk::Device) -> Result<Self, ()> {{ // TODO error type
        let mut ret = Self::default();
        unsafe {{
            let resp = ddk::device_get_protocol(
                parent_device.get_ptr(),
                ddk::protocols::ZX_PROTOCOL_{protocol_name_upper},
                &mut ret as *mut _ as *mut ::std::os::raw::c_void);
            if resp != fuchsia_zircon::sys::ZX_OK {{
                return Err(());
            }}
            Ok(ret)
        }}
    }}

    {safe_protocol_fns}
}}
