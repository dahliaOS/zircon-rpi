use {
    crate::Driver,
    fuchsia_ddk_sys as sys,
    failure::Error,
    fidl::endpoints::RequestStream,
    fidl_fuchsia_device_manager::{DeviceControllerRequest, DeviceControllerRequestStream},
    fuchsia_async as fasync,
    fuchsia_zircon::{self as zx},
    futures::TryStreamExt,
    log::*,
    std::rc::Rc,
};

pub struct Device {
    name: String,
    //    protocol_id: u32,
    local_device_id: u64,
    zx_device: *mut sys::zx_device_t,
}

impl Device {
    pub fn new(name: &str, local_device_id: u64) -> Rc<Self> {
        let dev = Device {
            name: name.to_string(),
            //            protocol_id,
            local_device_id,
            zx_device: std::ptr::null_mut(),
        };
        info!("Created a device named {} with local id {}", name, local_device_id);
        Rc::new(dev)
    }

    pub async fn connect_controller(
        &self,
        channel: zx::Channel,
        _driver: Option<Rc<Driver>>,
    ) -> Result<(), Error> {
        info!("Connecting the Device Controller!");
        let mut stream =
            DeviceControllerRequestStream::from_channel(fasync::Channel::from_channel(channel)?);
        while let Some(request) = stream.try_next().await? {
            match request {
                DeviceControllerRequest::BindDriver { driver_path, driver, responder } => {
                    info!("Bind Driver {} to device", driver_path);
                    //TODO use driver cache somewhere
                    let driver = Driver::new(driver_path.clone(), driver)?;
                    if let Some(driver_bind_op) = driver.get_ops_table().bind {
                        // TODO figure out what context should be. BindContext used?
                        let resp = zx::Status::from_raw(unsafe {
                            (driver_bind_op)(std::ptr::null_mut(), self.zx_device)
                        });
                        info!("Bind Driver {} bind op response: {}", driver_path, resp);
                        responder.send(resp.into_raw(), None)?;
                    } else {
                        responder.send(zx::sys::ZX_ERR_NOT_SUPPORTED, None)?;
                    }
                }
                DeviceControllerRequest::Unbind { control_handle: _ } => {
                    info!("Unbind device");
                }
                DeviceControllerRequest::ConnectProxy_ { shadow: _, control_handle: _ } => {
                    info!("Connect device to it's Proxy");
                }
                DeviceControllerRequest::CompleteRemoval { control_handle: _ } => {
                    info!("Complete removal of unbind");
                }
                DeviceControllerRequest::RemoveDevice { control_handle: _ } => {
                    info!("Remove device");
                }
                DeviceControllerRequest::CompleteCompatibilityTests {
                    status: _,
                    control_handle: _,
                } => {
                    info!("Suspend device");
                }
                DeviceControllerRequest::Suspend { flags: _, responder: _ } => {
                    info!("Suspend device");
                }
            }
        }
        Ok(())
    }
}
