pub mod channel;
pub mod country;
pub mod device_cap;
pub mod operclass;
pub mod power;
pub mod regulation;
pub mod utils;

#[macro_export]
macro_rules! vec_string {
    ($($x:expr),*) => (vec![$($x.to_string()),*]);
}
