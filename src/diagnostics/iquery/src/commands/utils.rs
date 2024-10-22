// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        commands::{Command, ListCommand},
        types::Error,
    },
    fuchsia_inspect::testing::InspectDataFetcher,
    fuchsia_inspect_node_hierarchy::{
        serialization::{HierarchyDeserializer, RawJsonNodeHierarchySerializer},
        NodeHierarchy,
    },
    serde_json,
};

/// Returns the moniker in a json response or an error if one is not found
pub fn get_moniker_from_result(result: &serde_json::Value) -> Result<String, Error> {
    Ok(result
        .get("moniker")
        .ok_or(Error::archive_missing_property("moniker"))?
        .as_str()
        .ok_or(Error::ArchiveInvalidJson)?
        .to_string())
}

/// Returns the moniker in a json response or an error if one is not found
/// Returns the selectors for a component whose url contains the `manifest` string.
pub async fn get_selectors_for_manifest(
    manifest: &Option<String>,
    tree_selectors: &Vec<String>,
) -> Result<Vec<String>, Error> {
    match &manifest {
        None => Ok(tree_selectors.clone()),
        Some(manifest) => {
            let list_command = ListCommand { manifest: Some(manifest.clone()), with_url: false };
            let result = list_command
                .execute()
                .await?
                .into_iter()
                .map(|item| item.into_moniker())
                .flat_map(|moniker| {
                    tree_selectors
                        .iter()
                        .map(move |tree_selector| format!("{}:{}", moniker, tree_selector))
                })
                .collect();
            Ok(result)
        }
    }
}

/// Returns the component "moniker" and the hierarchy data for results of
/// reading from the archive using the given selectors.
pub async fn fetch_data(selectors: &[String]) -> Result<Vec<(String, NodeHierarchy)>, Error> {
    let mut fetcher = InspectDataFetcher::new().retry_if_empty(false);
    // We support receiving the moniker or a tree selector
    for selector in selectors {
        if selector.contains(":") {
            fetcher = fetcher.add_selector(selector.as_ref());
        } else {
            fetcher = fetcher.add_selector(format!("{}:*", selector));
        }
    }
    let mut results = fetcher.get_raw_json().await.map_err(|e| Error::Fetch(e))?;

    let mut data = vec![];
    for result in results.as_array_mut().ok_or(Error::ArchiveInvalidJson)? {
        let moniker = get_moniker_from_result(&result)?;
        if let Some(payload) = result.get_mut("payload") {
            if payload.is_object() {
                let mut hierarchy: NodeHierarchy<String> =
                    RawJsonNodeHierarchySerializer::deserialize(payload.take())
                        .map_err(|_| Error::ArchiveInvalidJson)?;
                hierarchy.sort();
                data.push((moniker, hierarchy))
            }
        }
    }
    Ok(data)
}
