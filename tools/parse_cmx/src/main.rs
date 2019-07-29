mod graph;

use failure::{Error, Fail};
use graph::{Graph, Node};
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use structopt::StructOpt;

#[derive(Debug, PartialEq, Eq)]
struct Component {
    filename: String,
    binary: String,
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

    // Parse binary
    let binary = match &json["program"]["binary"] {
        Value::String(s) => Ok(s),
        _ => Err(ParseError::Generic { details: "No `program.binary`.".to_string() }),
    }?
    .to_string();

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
        binary,
        required_services,
        provided_services: vec![],
        features,
    })
}

fn parse_cmx_file(filename: &str) -> Result<Component, Error> {
    let contents = std::fs::read_to_string(filename)?;
    parse_cmx_string(filename, &contents)
}

fn parse_services_content(data: &str) -> Result<HashMap<String, String>, Error> {
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

    Ok(services)
}

fn parse_services_file(filename: &str) -> Result<HashMap<String, String>, Error> {
    let contents = std::fs::read_to_string(filename)?;
    parse_services_content(&contents)
}

#[derive(StructOpt, Debug)]
#[structopt(name = "basic")]
struct Opt {
    /// Print the names of reachable CMX files.
    #[structopt(long = "print-reachable")]
    print_reachable: bool,

    /// Services file
    #[structopt(name = "SERVICES")]
    services: String,

    /// .cmx files to process
    #[structopt(name = "FILE")]
    files: Vec<String>,
}

#[derive(Clone, Debug)]
enum NodeType {
    Component(std::rc::Rc<Component>),
    UnknownComponent,
    Interface,
    UnknownInterface,
}

fn print_graph(graph: &Graph<String, NodeType>) {
    println!("digraph G {{");
    for (name, node) in graph.nodes() {
        // Draw the node.
        let node_attributes = match node.data {
            NodeType::Component(_) => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#4285F4\""
            },
            NodeType::UnknownComponent => {
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#4285F4\""
            },
            NodeType::Interface =>
                "fontcolor=white, color=white, style=filled, border=none, fillcolor=\"#34A853\"",
            NodeType::UnknownInterface =>
                "fontcolor=\"#202124\", color=white, style=filled, border=none, fillcolor=\"#FEF7E0\"",
        };
        println!("  \"{}\" [{}];", name, node_attributes);
        for dest in node.edges.iter() {
            println!("  \"{}\" -> \"{}\";", name, dest);
        }

        // If we use certain features, print those too.
        if let NodeType::Component(component) = &node.data {
            for feature in component.features.iter() {
                let feature_name = format!("{}_{}", name, feature);
                println!("  \"{}\" [label=\"{}\"];", feature_name, feature);
                println!("  \"{}\" -> \"{}\";", name, feature_name)
            }
        }
    }
    println!("}}");
}

fn main() -> Result<(), Error> {
    let opt = Opt::from_args();

    // Parse the services directory.
    let services = parse_services_file(&opt.services)?;

    // Parse cmx files.
    let mut cmx_files = vec![];
    let mut seen_names: HashMap<String, String> = HashMap::new();
    for filename in opt.files {
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
        seen_names.insert(cmx.filename.to_string(), filename);
        cmx_files.push(cmx);
    }

    // Ensure we have all the services listed in the services file.
    let needed_cmx_files = services.values().collect::<HashSet<_>>();
    let available_cmx_files = cmx_files.iter().map(|x| &x.filename).collect::<HashSet<_>>();
    for file in needed_cmx_files.difference(&available_cmx_files) {
        eprintln!("Could not find expected file: {}", file);
    }

    // Generate a graph.
    let mut graph = Graph::new();

    // Add service nodes (but not edges).
    for service in services.iter() {
        graph
            .add_node(service.0.clone(), NodeType::Interface)
            .unwrap_or_else(|_| panic!("Duplicate service found: {}", service.0));
    }

    // Add component nodes and edges.
    for node in cmx_files {
        let node = std::rc::Rc::new(node);
        graph
            .add_node(node.filename.to_string(), NodeType::Component(node.clone()))
            .unwrap_or_else(|_| panic!("Duplicate key: {}", &node.filename));
        for service in node.required_services.iter() {
            if !graph.node_exists(service) {
                graph.add_node(service.clone(), NodeType::UnknownInterface).unwrap();
            }
            graph.add_edge(&node.filename, service.to_string()).unwrap();
        }
    }

    // Add interface edges.
    for (service, provider) in services.iter() {
        if !graph.node_exists(provider) {
            graph.add_node(provider.to_string(), NodeType::UnknownComponent).unwrap();
        }
        graph.add_edge(service, provider.to_string()).unwrap();
    }

    // Only show reachable
    let graph = graph::reachable(&graph, &["sessionmgr.cmx".to_string()]);

    // Print
    if opt.print_reachable {
        for node in graph.nodes() {
            println!("{}", node.0);
        }
    } else {
        print_graph(&graph);
    }

    Ok(())
}
