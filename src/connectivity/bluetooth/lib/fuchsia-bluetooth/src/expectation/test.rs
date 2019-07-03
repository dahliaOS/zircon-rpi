use crate::{assert_satisfies, over};
use crate::expectation::*;
use crate::expectation::Predicate as P;
use fidl_fuchsia_bluetooth_control::{Appearance, RemoteDevice, TechnologyType};

const TEST_PEER_NAME: &'static str = "TestPeer";
const TEST_PEER_ADDRESS: &'static str = "00:00:00:00:00:01";
const INCORRECT_PEER_NAME: &'static str = "IncorrectPeer";
const INCORRECT_PEER_ADDRESS: &'static str = "00:00:00:00:00:02";

fn correct_name() -> Predicate<RemoteDevice> {
    peer::name(TEST_PEER_NAME)
}
fn incorrect_name() -> Predicate<RemoteDevice> {
    peer::name(INCORRECT_PEER_NAME)
}
fn correct_address() -> Predicate<RemoteDevice> {
    peer::address(TEST_PEER_ADDRESS)
}
fn incorrect_address() -> Predicate<RemoteDevice> {
    peer::address(INCORRECT_PEER_ADDRESS)
}

fn test_peer() -> RemoteDevice {
    RemoteDevice {
        name: Some(TEST_PEER_NAME.into()),
        address: TEST_PEER_ADDRESS.into(),
        technology: TechnologyType::LowEnergy,
        connected: false,
        bonded: false,
        appearance: Appearance::Unknown,
        identifier: "".into(),
        rssi: None,
        tx_power: None,
        service_uuids: vec![],
    }
}

#[test]
fn test() -> Result<(),AssertionText> {
    correct_name().assert_satisfied(&test_peer())
}

#[test]
fn simple_predicate_succeeds() {
    let predicate = Predicate::equal(Some(TEST_PEER_NAME.to_string())).over(
            |dev: &RemoteDevice| &dev.name,
            ".name");
    assert!(predicate.satisfied(&test_peer()));
}
#[test]
fn simple_incorrect_predicate_fail() {
    let predicate = Predicate::equal(Some(INCORRECT_PEER_NAME.to_string())).over(
            |dev: &RemoteDevice| &dev.name,
            ".name");
    assert!(!predicate.satisfied(&test_peer()));
}

#[test]
fn predicate_and_both_true_succeeds() {
    let predicate = correct_name().and(correct_address());
    assert!(predicate.satisfied(&test_peer()));
}

#[test]
fn predicate_and_one_or_more_false_fails() {
    let predicate = correct_name().and(incorrect_address());
    assert!(!predicate.satisfied(&test_peer()));

    let predicate = incorrect_name().and(correct_address());
    assert!(!predicate.satisfied(&test_peer()));

    let predicate = incorrect_name().and(incorrect_address());
    assert!(!predicate.satisfied(&test_peer()));
}

#[test]
fn predicate_or_both_false_fails() {
    let predicate = incorrect_name().or(incorrect_address());
    assert!(!predicate.satisfied(&test_peer()));
}

#[test]
fn predicate_or_one_or_more_true_succeeds() {
    let predicate = correct_name().or(correct_address());
    assert_satisfies!(&test_peer(), predicate);

    let predicate = incorrect_name().or(correct_address());
    assert_satisfies!(&test_peer(), predicate);

    let predicate = correct_name().or(incorrect_address());
    assert_satisfies!(&test_peer(), predicate);
}

#[test]
fn predicate_not_incorrect_succeeds() {
    let predicate = incorrect_name().not();
    assert_satisfies!(&test_peer(), predicate);
}

#[test]
fn predicate_not_correct_fails() {
    let predicate = correct_name().not();
    assert!(!predicate.satisfied(&test_peer()));
}

#[test]
fn incorrect_over_predicate_fails() {
    let predicate = over!(RemoteDevice:name, P::equal(Some("INCORRECT_NAME".to_string())));

    //assert_satisfies!(&test_peer(), predicate);
    let expected_msg = vec![
        "FAILED EXPECTATION",
        "  .name == Some(\"INCORRECT_NAME\")",
        "FALSIFIED BY",
        "  .name Some(\"TestPeer\") != Some(\"INCORRECT_NAME\")"
    ].join("\n");

    assert_eq!(predicate.assert_satisfied(&test_peer()), Err(AssertionText(expected_msg)))
}

#[test]
fn incorrect_not_predicate_fails() {
    let predicate = over!(RemoteDevice:name, P::not_equal(Some(TEST_PEER_NAME.to_string())));

    let expected_msg = vec![
        "FAILED EXPECTATION",
        "  .name NOT == Some(\"TestPeer\")",
        "FALSIFIED BY",
        "  .name NOT == Some(\"TestPeer\")"
    ].join("\n");

    assert_eq!(predicate.assert_satisfied(&test_peer()), Err(AssertionText(expected_msg)))
}

#[derive(Debug, PartialEq, Clone)]
struct Person {
    name: String,
    age: u64,
}

#[derive(Debug, PartialEq, Clone)]
struct Group {
    persons: Vec<Person>,
}

#[test]
fn incorrect_compound_all_predicate_fails() {
    let test_group = Group {
        persons: vec![Person{ name: "Alice".to_string(), age: 40 },
                      Person{ name: "Bob".to_string(), age: 41 }]
    };

    let predicate =
        over!(Group:persons,
            P::all(
                over!(Person:name, P::not_equal("Bob".to_string()))
                .and(
                over!(Person:age, P::new(|age: &u64| *age < 50, "< 50")))));

    let expected_msg = vec![
        "FAILED EXPECTATION",
        "  .persons ALL (.name NOT == \"Bob\") AND (.age < 50)",
        "FALSIFIED BY",
        "  .persons ELEM Person { name: \"Bob\", age: 41 } FAILS .name NOT == \"Bob\","
    ].join("\n");

    assert_eq!(predicate.assert_satisfied(&test_group), Err(AssertionText(expected_msg)));
}

#[test]
fn incorrect_compound_any_predicate_fails() {
    let test_group = Group {
        persons: vec![Person{ name: "Alice".to_string(), age: 40 },
                      Person{ name: "Bob".to_string(), age: 41 },
                      Person{ name: "Bob".to_string(), age: 39 }]
    };

    let predicate =
        over!(Group:persons,
            P::any(
                over!(Person:name, P::not_equal("Bob".to_string()))
                .and(
                over!(Person:age, P::new(|age: &u64| *age > 40, "> 40")))));

    assert_satisfies!(&test_group, predicate);

    /*

    let expected_msg = vec![
        "FAILED EXPECTATION",
        "  .persons ALL (.name NOT == \"Bob\") AND (.age < 50)",
        "FALSIFIED BY",
        "  .persons ELEM Person { name: \"Bob\", age: 41 } FAILS .name NOT == \"Bob\""
    ].join("\n");

    assert_eq!(predicate.assert_satisfied(&test_group), Err(AssertionText(expected_msg)));
    */
}
