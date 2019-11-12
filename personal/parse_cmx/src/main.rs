mod graph;

use failure::{Error, Fail};
use graph::Graph;
use reqwest;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use structopt::StructOpt;

#[derive(Debug, PartialEq, Eq)]
enum ComponentType {
    Binary(String),
    Data(String),
    Unknown,
}

#[derive(Debug, PartialEq, Eq)]
struct Component {
    filename: String,
    type_: ComponentType,
    required_services: Vec<String>,
    provided_services: Vec<String>,
    features: HashSet<String>,
}

#[derive(Debug, PartialEq, Eq)]
struct Service {
    name: String,
    package: String,
}

#[derive(Debug, Fail)]
enum ParseError {
    #[fail(display = "Could not parse CMX file: {}", details)]
    Generic { details: String },
}

// Convert "fuchsia-pkg://.../x.cmx" or "/a/b/c/x.cmx" into "x.cmx".
fn guess_package_name(name: &str) -> String {
    name.rsplit('/').next().unwrap().to_string()
}

fn parse_cmx_string(filename: &str, data: &str) -> Result<Component, Error> {
    let json: serde_json::Value = serde_json::from_str(data)?;
    parse_cmx_json(filename, &json)
}

fn parse_cmx_json(filename: &str, json: &serde_json::Value) -> Result<Component, Error> {
    // Determine component type.
    let type_ = if let Value::String(binary) = &json["program"]["binary"] {
        ComponentType::Binary(binary.to_string())
    } else if let Value::String(data) = &json["program"]["data"] {
        ComponentType::Data(data.to_string())
    } else {
        ComponentType::Unknown
    };

    // Parse input services
    let no_services = vec![];
    let required_services = match &json["sandbox"]["services"] {
        Value::Array(values) => values,
        _ => &no_services,
    }
    .iter()
    .filter_map(|v| match v {
        Value::String(s) => Some(s.to_string()),
        _ => None,
    })
    .collect::<Vec<String>>();

    // Parse features.
    let no_features = vec![];
    let features = match &json["sandbox"]["features"] {
        Value::Array(values) => values,
        _ => &no_features,
    }
    .iter()
    .filter_map(|v| match v {
        Value::String(s) => Some(s.to_string()),
        _ => None,
    })
    .collect::<HashSet<String>>();

    Ok(Component {
        filename: guess_package_name(filename),
        type_,
        required_services,
        provided_services: vec![],
        features,
    })
}

fn parse_cmx_file(filename: &str) -> Result<Component, Error> {
    let contents = std::fs::read_to_string(filename)?;
    parse_cmx_string(filename, &contents)
}

struct ServiceConfig {
    services: HashMap<String, String>,
    startup_services: Vec<String>,
}

fn parse_services_content(data: &str) -> Result<ServiceConfig, Error> {
    let json: serde_json::Value = serde_json::from_str(data)?;

    // Parse input services
    let no_services = serde_json::map::Map::new();
    let services = match &json["services"] {
        Value::Object(values) => values,
        _ => &no_services,
    };
    let services = services
        .iter()
        .filter_map(|v| match v {
            (k, Value::String(v)) => Some((k.to_string(), guess_package_name(v))),
            _ => None,
        })
        .collect::<HashMap<String, String>>();

    // Parse startup services.
    let no_startup_services = Vec::new();
    let startup_services = match &json["startup_services"] {
        Value::Array(values) => values,
        _ => &no_startup_services,
    };
    let startup_services =
        startup_services.iter().map(|x| x.as_str().unwrap().to_string()).collect::<Vec<String>>();

    Ok(ServiceConfig { services, startup_services })
}

fn parse_services_file(filename: &str) -> Result<ServiceConfig, Error> {
    let contents = std::fs::read_to_string(filename)?;
    parse_services_content(&contents)
}

#[derive(StructOpt, Debug)]
#[structopt(name = "basic")]
struct Opt {
    /// Just print the names of reachable CMX files.
    #[structopt(long = "print-reachable")]
    print_reachable: bool,

    /// Don't show interface names; just CMX files.
    #[structopt(long = "no-interfaces")]
    no_interfaces: bool,

    /// Services files
    #[structopt(long = "service", name = "service")]
    services: Vec<String>,

    /// ".cmx" files to process
    #[structopt(long = "cmx", name = "cmx")]
    files: Vec<String>,

    /// Component server to connect to.
    #[structopt(short = "s", long = "server", name = "server")]
    server: Option<String>,

    /// ".cmx" files to use as roots. If none specified, show all.
    #[structopt(long = "roots", name = "roots")]
    roots: Vec<String>,

    /// ".cmx" files to ignore.
    #[structopt(long = "ignore", name = "ignore")]
    ignore: Vec<String>,
}

#[derive(Clone, Debug)]
enum NodeType {
    Component(std::rc::Rc<Component>),
    IgnoredComponent,
    UnknownComponent,
    Interface,
    UnknownInterface,
}

/// Merge items in "from" into "into". Return a vector of duplicate keys.
fn merge_into<K, V>(into: &mut HashMap<K, V>, mut from: HashMap<K, V>) -> Vec<(K, V)>
where
    K: PartialEq + Eq + std::hash::Hash + Clone + std::fmt::Debug,
    V: std::fmt::Debug,
{
    let mut duplicates = Vec::new();
    for (k, v) in from.drain() {
        match into.entry(k.clone()) {
            std::collections::hash_map::Entry::Occupied(_) => {
                duplicates.push((k, v));
            }
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(v);
            }
        }
    }
    duplicates
}

fn print_graph(graph: &Graph<String, NodeType>, roots: &[String]) {
    let roots = roots.iter().collect::<HashSet<_>>();

    println!("digraph G {{");
    println!("  rankdir=\"LR\";");
    for (name, node) in graph.nodes() {
        // Draw the node.
        let node_attributes = match (node.data.clone(), roots.contains(&name))  {
            (NodeType::Component(_), true) => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#4285F4\""
            },
            (NodeType::Component(_), false) => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#4285C4\""
            },
            (NodeType::IgnoredComponent, _) => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#F48542\""
            },
            (NodeType::UnknownComponent, _) => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#42F485\""
            },
            (NodeType::Interface, _) =>
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#34A853\"",
            (NodeType::UnknownInterface, _) =>
                "fontcolor=\"#202124\", color=white, style=filled, border=none, fillcolor=\"#FEF7E0\"",
        };
        println!("  \"{}\" [{}];", name, node_attributes);
        for dest in node.edges.iter() {
            println!("  \"{}\" -> \"{}\";", name, dest);
        }

        // If we use certain features, print those too.
        if let NodeType::Component(component) = &node.data {
            for feature in component.features.iter() {
                // Create a unique feature node for this parent.
                let feature_name = format!("{}_{}", name, feature);
                println!("  \"{}\" [label=\"{}\"];", feature_name, feature);
                println!("  \"{}\" -> \"{}\";", name, feature_name)
            }
        }
    }
    println!("}}");
}

struct ParsedResults {
    services: HashMap<String, String>,
    startup_services: HashSet<String>,
    cmx_files: Vec<Component>,
}

fn parse_from_files(service_configs: &Vec<String>, cmx_files: &Vec<String>) -> ParsedResults {
    // Parse the services directory.
    let mut services = HashMap::new();
    let mut startup_services = HashSet::new();
    for service_file in service_configs {
        match parse_services_file(service_file) {
            Ok(config) => {
                merge_into(&mut services, config.services);
                for s in config.startup_services {
                    startup_services.insert(s);
                }
            }
            Err(e) => {
                eprintln!("Could not parse {}: {}", service_file, e);
            }
        }
    }

    // Parse cmx files.
    let mut parsed_cmx_files = vec![];
    let mut seen_names: HashMap<String, String> = HashMap::new();
    for filename in cmx_files {
        let cmx = match parse_cmx_file(&filename) {
            Ok(cmx) => cmx,
            Err(e) => {
                eprintln!("Error processing {}: {}", filename, e);
                continue;
            }
        };
        if let Some(orig_name) = seen_names.get(&cmx.filename) {
            eprintln!("Duplicate CMX file: {}, {}", filename, orig_name);
            continue;
        }
        seen_names.insert(cmx.filename.to_string(), filename.to_string());
        parsed_cmx_files.push(cmx);
    }

    // Ensure we have all the services listed in the services file.
    let needed_cmx_files = services.values().collect::<HashSet<_>>();
    let available_cmx_files = parsed_cmx_files.iter().map(|x| &x.filename).collect::<HashSet<_>>();
    for file in needed_cmx_files.difference(&available_cmx_files) {
        eprintln!("Could not find expected file: {}", file);
    }

    ParsedResults { services, startup_services, cmx_files: parsed_cmx_files }
}

fn parse_from_server(address: &str) -> Result<ParsedResults, Error> {
    // Parse services.
    let services_body: serde_json::map::Map<_, _> =
        reqwest::get(&format!("http://{}/api/component/services", address))?.json()?;
    let services = services_body
        .iter()
        .filter_map(|v| match v {
            (k, Value::String(v)) => Some((k.to_string(), guess_package_name(&v))),
            _ => None,
        })
        .collect::<HashMap<String, String>>();

    // Parse components.
    let components_body: Vec<serde_json::map::Map<_, _>> =
        reqwest::get(&format!("http://{}/api/component/packages", address))?.json()?;
    let mut components = Vec::new();
    for component in components_body {
        // If we have a manifest, parse that.
        if let (Some(serde_json::Value::String(url)), Some(value)) =
            (component.get("url"), component.get("manifest"))
        {
            components.push(parse_cmx_json(&guess_package_name(url), value)?);
            continue;
        }

        // Otherwise, match any CMX file we find inside.
        match &component["files"] {
            serde_json::Value::Object(map) => {
                for (filename, data) in map {
                    if filename.ends_with(".cmx") {
                        components.push(parse_cmx_json(filename, data)?);
                    }
                }
            }
            _ => {
                return Err(ParseError::Generic {
                    details: "Could not parse 'file' stanza.".to_string(),
                }
                .into())
            }
        }
    }

    // Return results.
    Ok(ParsedResults { services, startup_services: HashSet::new(), cmx_files: components })
}

fn main() -> Result<(), Error> {
    let opt = Opt::from_args();

    // Parse config files.
    let system = match opt.server {
        Some(server) => parse_from_server(&server)?,
        None => parse_from_files(&opt.services, &opt.files),
    };

    // Generate a graph.
    let mut graph = Graph::new();

    // Add service nodes (but not edges).
    for service in system.services.iter() {
        graph
            .add_node(service.0.clone(), NodeType::Interface)
            .unwrap_or_else(|_| panic!("Duplicate service found: {}", service.0));
    }

    // Add component nodes and edges.
    for node in system.cmx_files {
        let node = std::rc::Rc::new(node);
        graph
            .add_node(node.filename.to_string(), NodeType::Component(node.clone()))
            .unwrap_or_else(|_| panic!("Duplicate key: {}", &node.filename));
        for service in node.required_services.iter() {
            if !graph.node_exists(service) {
                graph.add_node(service.clone(), NodeType::UnknownInterface).unwrap();
            }
            graph.add_edge(&node.filename, service).unwrap();
        }
    }

    // Add fake "startup" node.
    eprintln!("Startup services: {:?}", &system.startup_services);
    graph.add_node("startup".to_string(), NodeType::UnknownComponent)?;
    for service in system.startup_services {
        match graph.add_edge(&"startup".to_string(), &service) {
            Ok(()) => (),
            Err(_) => eprintln!("Could not find startup service: {}", service),
        }
    }

    // Add interface edges.
    //
    // Even if we don't want to show them, we still need to track them to find CMX -> CMX
    // connections.
    for (service, provider) in system.services.iter() {
        if !graph.node_exists(provider) {
            graph.add_node(provider.to_string(), NodeType::UnknownComponent).unwrap();
        }
        graph.add_edge(service, provider).unwrap();
    }

    // If the user doesn't want to show interfaces, collapse down those nodes.
    if opt.no_interfaces {
        let mut collapsed_keys = HashSet::new();
        for (key, type_) in graph.nodes() {
            match type_.data {
                NodeType::Interface | NodeType::UnknownInterface => {
                    collapsed_keys.insert(key.clone());
                }
                _ => (),
            }
        }
        for key in collapsed_keys {
            graph.collapse_node(&key)?;
        }
    }

    // Remove any outgoing edges on nodes the user asked to ignore.
    for name in opt.ignore {
        match graph.node(&name) {
            Some(node) => {
                let node = node.clone();
                graph.remove_node(&name)?;
                graph.add_node(name.clone(), NodeType::IgnoredComponent)?;
                for src in node.back_edges {
                    graph.add_edge(&src, &name)?;
                }
            }
            None => eprintln!("Could not ignore unknown node: {}", &name),
        }
    }

    // Only show reachable
    if opt.roots.len() > 0 {
        graph = graph::reachable(&graph, &opt.roots)?;
    }

    // Print
    if opt.print_reachable {
        for node in graph.nodes() {
            println!("{}", node.0);
        }
    } else {
        print_graph(&graph, &opt.roots);
    }

    Ok(())
}
