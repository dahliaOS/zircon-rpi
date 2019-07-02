//! Expectations for the Bluetooth Host Driver (bt-host)

use super::Predicate;
use fidl_fuchsia_bluetooth::Bool;
use fidl_fuchsia_bluetooth_control::AdapterState;

pub fn name(expected_name: &str) -> Predicate<AdapterState> {
    let name = Some(expected_name.to_string());
    Predicate::<AdapterState>::new(
        move |host_driver| host_driver.local_name == name,
        format!("name == {}", expected_name),
    )
}
pub fn discovering(discovering: bool) -> Predicate<AdapterState> {
    Predicate::<AdapterState>::new(
        move |host_driver| {
            host_driver.discovering == Some(Box::new(Bool { value: discovering }))
        },
        format!("discovering == {}", discovering),
    )
}
pub fn discoverable(discoverable: bool) -> Predicate<AdapterState> {
    Predicate::<AdapterState>::new(
        move |host_driver| {
            host_driver.discoverable == Some(Box::new(Bool { value: discoverable }))
        },
        format!("discoverable == {}", discoverable),
    )
}
