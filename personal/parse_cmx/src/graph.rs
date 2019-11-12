use failure::Fail;
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
    pub edges: HashSet<Key>,
    pub back_edges: HashSet<Key>,
    pub data: Data,
}

#[derive(Debug, Clone)]
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
        self.nodes
            .insert(key, Node { edges: HashSet::new(), back_edges: HashSet::new(), data: value });
        Ok(())
    }

    pub fn remove_node(&mut self, key: &K) -> Result<(), GraphError> {
        let node = match self.nodes.remove(key) {
            Some(v) => v,
            None => return Err(GraphError::NoSuchKey),
        };
        for edge in node.edges {
            self.nodes.get_mut(&edge).unwrap().back_edges.remove(key);
        }
        for back_edge in node.back_edges {
            self.nodes.get_mut(&back_edge).unwrap().edges.remove(key);
        }
        Ok(())
    }

    /// Remove a node, joining all source edges to all dest edges.
    pub fn collapse_node(&mut self, key: &K) -> Result<(), GraphError> {
        let node = self.nodes.get(key).ok_or(GraphError::NoSuchKey)?.clone();
        self.remove_node(key).unwrap();
        for src in node.back_edges {
            for dest in node.edges.iter() {
                self.add_edge(&src, &dest)?;
            }
        }
        Ok(())
    }

    pub fn add_edge(&mut self, src: &K, dest: &K) -> Result<(), GraphError> {
        if !self.nodes.contains_key(dest) {
            return Err(GraphError::UnknownDestKey);
        }
        if !self.nodes.contains_key(src) {
            return Err(GraphError::UnknownSrcKey);
        }
        self.nodes.get_mut(dest).unwrap().back_edges.insert(src.clone());
        self.nodes.get_mut(src).unwrap().edges.insert(dest.clone());
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

    pub fn edges<'a>(&'a self, key: &K) -> Option<impl Iterator<Item = &'a K>> {
        self.nodes.get(key).map(|x| x.edges.iter())
    }

    pub fn keys<'a>(&'a self) -> impl Iterator<Item = &'a K> {
        self.nodes.iter().map(|x| x.0)
    }
}

/*
/// Collapse uninteresting nodes.
///
/// We consider a node "interesting" if it is reachable from the root set.
///
/// We consider nodes "uninteresting" if they are in the input uninteresting
/// set, or a descendant of such a node (and not a descendant of an interesting
/// node).
pub fn collapse<K, V>(
    graph: &Graph<K, V>,
    roots: &[K],
    uninteresting: &[K],
) -> Result<Graph<K, V>, GraphError>
where
    K: Eq + Clone + Hash + std::fmt::Debug,
    V: Clone + std::fmt::Debug,
{
    // Find out which uninteresting nodes are reachable from one of the roots.
    let all_reachable_keys =
        reachable(graph, roots)?.keys().map(|x| x.clone()).collect::<HashSet<_>>();

    // Remove all uninteresting nodes, and find out what is still intersting.
    let mut purged_graph = graph.clone();
    for k in uninteresting {
        purged_graph.remove_node(k);
    }
    let interesting_keys =
        reachable(&purged_graph, roots)?.keys().map(|x| x.clone()).collect::<HashSet<_>>();

    // Find all nodes reachable from the set of unintersting nodes.
    let mut reachable_nodes: HashMap<&K, HashSet<K>> = HashMap::new();
    for n in uninteresting {
        if all_reachable_keys.contains(n) {
            let r = reachable(graph, &vec![n.clone()])?
                .keys()
                .map(|x| x.clone())
                .collect::<HashSet<K>>();
            reachable_nodes.insert(n, r);
        }
    }

    // Copy all interesting nodes.
    let mut new_graph = reachable(&purged_graph, roots)?;

    // Add in the reachable uninteresting nodes and edges.
    for (k, reachable) in reachable_nodes {
            Some(node) => {
                let node = node.clone();
                graph.remove_node(&name);
                graph.add_node(name.clone(), NodeType::IgnoredComponent);
                for src in node.back_edges {
                    graph.add_edge(&src, &name);
                }
            }

    }


    Ok(new_graph)
}
*/

/// Given a graph and a set of roots, return a new graph containing only
/// nodes reachable from the set of roots.
pub fn reachable<K, V>(graph: &Graph<K, V>, roots: &[K]) -> Result<Graph<K, V>, GraphError>
where
    K: Eq + Clone + Hash + std::fmt::Debug,
    V: Clone + std::fmt::Debug,
{
    let mut active = roots.to_vec();
    let mut processed = active.iter().cloned().collect::<HashSet<_>>();
    let mut result = Graph::new();

    // Copy set of input nodes.
    for node_name in roots.iter() {
        match graph.node(node_name) {
            Some(node) => {
                result.add_node(node_name.clone(), node.data.clone())?;
            }
            None => panic!("Node key {:?} does not exist.", node_name),
        }
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
                result.add_edge(&src_name, dest_name).unwrap_or_else(|err| {
                    panic!(
                        "Couldn't add an edge between two nodes expected to exist: {:#?} -> {:#?}: Err {:?}",
                        &src_name, &dest_name, err
                    )
                });
            }
        }
    }

    Ok(result)
}
