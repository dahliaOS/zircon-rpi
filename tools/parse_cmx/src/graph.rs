use failure::{Error, Fail};
use std::collections::{HashMap, HashSet};
use std::hash::Hash;

#[derive(Debug, Fail)]
pub enum GraphError {
    #[fail(display = "Key already exists")]
    KeyAlreadyExists,
    #[fail(display = "Node with key not found")]
    NoSuchKey,
    #[fail(display = "Source key unknown")]
    UnknownSrcKey,
    #[fail(display = "Dest key unknown")]
    UnknownDestKey,
}

#[derive(Debug, Clone)]
pub struct Node<Key: Eq + Clone + Hash, Data: Clone> {
    pub edges: Vec<Key>,
    pub data: Data,
}

#[derive(Debug)]
pub struct Graph<Key: Eq + Clone + Hash, Data: Clone> {
    nodes: HashMap<Key, Node<Key, Data>>,
}

impl<K, V> Graph<K, V>
where
    K: Eq + Clone + Hash,
    V: Clone,
{
    pub fn new() -> Graph<K, V> {
        Graph { nodes: HashMap::new() }
    }

    pub fn add_node(&mut self, key: K, value: V) -> Result<(), GraphError> {
        if self.nodes.contains_key(&key) {
            return Err(GraphError::KeyAlreadyExists);
        }
        self.nodes.insert(key, Node { edges: vec![], data: value });
        Ok(())
    }

    pub fn add_edge(&mut self, src: &K, dest: K) -> Result<(), GraphError> {
        if !self.nodes.contains_key(&dest) {
            return Err(GraphError::UnknownDestKey);
        }
        let src_node = match self.nodes.get_mut(src) {
            Some(src_node) => src_node,
            _ => return Err(GraphError::UnknownSrcKey),
        };
        src_node.edges.push(dest);
        Ok(())
    }

    pub fn node(&self, key: &K) -> Option<&Node<K, V>> {
        self.nodes.get(key)
    }

    pub fn nodes(&self) -> std::collections::hash_map::Iter<K, Node<K, V>> {
        self.nodes.iter()
    }

    pub fn node_exists(&self, key: &K) -> bool {
        self.nodes.contains_key(key)
    }

    pub fn edges(&self, key: &K) -> Option<std::slice::Iter<K>> {
        self.nodes.get(key).map(|x| x.edges.iter())
    }
}

pub fn reachable<K, V>(graph: &Graph<K, V>, roots: &[K]) -> Graph<K, V>
where
    K: Eq + Clone + Hash + std::fmt::Debug,
    V: Clone + std::fmt::Debug,
{
    let mut active = roots.to_vec();
    let mut processed = active.iter().cloned().collect::<HashSet<_>>();
    let mut result = Graph::new();

    // Copy set of input nodes.
    for node_name in roots.iter() {
        result
            .add_node(
                node_name.clone(),
                graph.node(node_name).expect("root node does not exist.").data.clone(),
            )
            .expect("Duplicate root node given.");
    }

    // Walk the graph for other reachable nodes.
    while let Some(src_name) = active.pop() {
        if let Some(src_node) = graph.nodes.get(&src_name) {
            for dest_name in src_node.edges.iter() {
                if !processed.contains(&dest_name) {
                    // Copy the node to the destination graph.
                    result
                        .add_node(dest_name.clone(), graph.node(dest_name).unwrap().data.clone())
                        .expect("Unexpected: node already exists.");
                    // Add it to our list of nodes we need to process.
                    processed.insert(dest_name.clone());
                    active.push(dest_name.clone());
                }
                result.add_edge(&src_name, dest_name.clone()).unwrap_or_else(|err| {
                    panic!(
                        "Couldn't add an edge between two nodes expected to exist: {:#?} -> {:#?}: Err {:?}",
                        &src_name, &dest_name, err
                    )
                });
            }
        }
    }

    result
}
