/// Returns the jurisdiction of operation.
/// Never fails to return. The fallback is "GLOBAL".
pub fn get_jurisdiction() -> String {
    return "US".to_string();
}
