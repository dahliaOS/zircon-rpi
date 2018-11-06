use std::fmt::Display;

/// A Host Identifier
#[derive(PartialEq,Eq,Hash)]
pub struct HostId{
    uuid: String
}

impl From<String> for HostId {
    fn from(uuid: String) -> HostId {
        HostId{ uuid }
    }
}

impl Display for HostId {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        self.uuid.fmt(f)
    }
}

/// A RemoteDevice Identifier
pub struct DeviceId( String );

impl From<String> for DeviceId {
    fn from(s: String) -> DeviceId {
        DeviceId( s ) 
    }
}

pub mod bt {
    pub enum Error {
        NoAdapter,
        BadResponseFromAdapter
    }

    pub type Result<T> = std::result::Result<T,Error>;
}

#[derive(PartialEq)]
pub enum DiscoveryState {
    Discovering,
    NotDiscovering
}
