use {
    crate::Driver,
    failure::Error,
    fidl::endpoints::RequestStream,
    fidl_fuchsia_device_manager::{DeviceControllerRequest, DeviceControllerRequestStream},
    fuchsia_async as fasync,
    fuchsia_zircon::{self as zx, sys},
    futures::TryStreamExt,
    log::*,
    std::rc::Rc,
};

pub async fn connect(
    channel: zx::Channel,
    _local_device_id: u64,
    _driver: Option<Rc<Driver>>,
) -> Result<(), Error> {
    info!("Connecting the Device Controller!");
    let mut stream =
        DeviceControllerRequestStream::from_channel(fasync::Channel::from_channel(channel)?);
    while let Some(request) = stream.try_next().await? {
        match request {
            DeviceControllerRequest::BindDriver { driver_path, driver: _, responder } => {
                info!("Bind Driver {} to device", driver_path);
                responder.send(sys::ZX_OK, None)?;
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
