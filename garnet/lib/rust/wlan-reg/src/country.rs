/// Returns the jurisdiction of operation.
/// Never fails to return. The fallback is "GLOBAL".
pub fn get_jurisdiction() -> String {
    return "US".to_string();
}

pub fn get_active_operating_classes() -> Vec<u8> {
    [1, 2, 3, 4, 5, 12, 22, 23, 24, 26, 27, 28, 29, 31, 32, 34, 128, 129, 130].to_vec()
}
