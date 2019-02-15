// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::ast::{self, BanjoAst, Constant, Method},
    crate::backends::{c, Backend},
    failure::{format_err, Error},
    heck::{CamelCase, SnakeCase},
    std::io,
};

type DeclIter<'a> = std::slice::Iter<'a, ast::Decl>;

pub struct RustBackend<'a, W: io::Write> {
    w: &'a mut W,
}

impl<'a, W: io::Write> RustBackend<'a, W> {
    pub fn new(w: &'a mut W) -> Self {
        RustBackend { w }
    }
}

fn namespace_to_use(name: &str) -> String {
    name.split(".").last().unwrap().to_snake_case()
}

fn get_first_param<'a>(ast: &BanjoAst, method: &'a ast::Method) -> Option<&'a (String, ast::Ty)> {
    // Return parameter if a primitive type.
    if let Some(param) = method.out_params.get(0) {
        if param.1.is_primitive(ast) {
            return Some(param);
        }
    }
    None
}

fn to_rust_type(ast: &ast::BanjoAst, ty: &ast::Ty) -> Result<String, Error> {
    match ty {
        ast::Ty::Bool => Ok(String::from("bool")),
        ast::Ty::Int8 => Ok(String::from("i8")),
        ast::Ty::Int16 => Ok(String::from("i16")),
        ast::Ty::Int32 => Ok(String::from("i32")),
        ast::Ty::Int64 => Ok(String::from("i64")),
        ast::Ty::UInt8 => Ok(String::from("u8")),
        ast::Ty::UInt16 => Ok(String::from("u16")),
        ast::Ty::UInt32 => Ok(String::from("u32")),
        ast::Ty::UInt64 => Ok(String::from("u64")),
        ast::Ty::Float32 => Ok(String::from("f32")),
        ast::Ty::Float64 => Ok(String::from("f64")),
        ast::Ty::USize => Ok(String::from("usize")),
        ast::Ty::Array { .. } => Ok(String::from("*mut libc::c_void /* Array */ ")),
        ast::Ty::Voidptr => Ok(String::from("*mut libc::c_void /* Voidptr */ ")),
        ast::Ty::Enum { .. } => Ok(String::from("*mut libc::c_void /* Enum not right*/")),
        ast::Ty::Str { .. } => Ok(String::from("*mut libc::c_void /* String */")),
        ast::Ty::Vector { ref ty, size: _, nullable: _ } => to_rust_type(ast, ty),
        ast::Ty::Identifier { id, reference } => {
            let ptr = if *reference { "*mut "} else { "" };
            if id.is_base_type() {
                Ok(format!("zircon::sys::zx_{}_t", id.name()))
            } else {
                match ast.id_to_type(id) {
                    ast::Ty::Interface => return Ok(c::to_c_name(id.name())),
                    ast::Ty::Struct => {
                        let name = c::to_c_name(id.name());
                        return Ok(format!("{ptr}{name}_t", ptr = ptr, name = name))
                    }
                    t => to_rust_type(ast, &t),
                }
            }
        }
        ast::Ty::Handle { .. } => Ok(String::from("zircon::sys::zx_handle_t")),
        t => Err(format_err!("unknown type in to_rust_type {:?}", t)),
    }
}

impl<'a, W: io::Write> RustBackend<'a, W> {
    fn codegen_enum_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = String::new();
        for decl in namespace {
            if let ast::Decl::Enum { ref name, ref ty, attributes: _, ref variants } = *decl {
                let mut enum_defines = Vec::new();
                for v in variants {
                    let Constant(ref size) = v.size;
                    enum_defines.push(format!(
                        "    {c_name} = {val},",
                        c_name = v.name.as_str().to_uppercase(),
                        val = size
                    ));
                }
                accum.push_str(
                    format!(
                        include_str!("templates/rust/enum.rs"),
                        name = c::to_c_name(name) + "_t",
                        ty = to_rust_type(ast, ty)?,
                        enum_decls = enum_defines.join("\n")
                    )
                    .as_str(),
                );
            }
        }
        Ok(accum)
    }

    fn codegen_protocol_fn_decl(
        &self,
        methods: &Vec<Method>,
        ast: &BanjoAst,
    ) -> Result<String, Error> {
        let mut fns = Vec::new();
        for method in methods {
            let mut params: Vec<String> = method
                .in_params
                .iter()
                .map(|(name, ty)| {
                    format!("{}: {}", c::to_c_name(name), to_rust_type(ast, ty).unwrap())
                })
                .collect();

            let mut out_params_iter = method.out_params.iter();
            let return_param = match get_first_param(ast, method) {
                Some(p) => {
                    out_params_iter.next();
                    to_rust_type(ast, &p.1)?
                }
                None => "()".to_string(),
            };

            let out_params: Vec<String> = out_params_iter
                .map(|(name, ty)| {
                    match ty {
                        ast::Ty::Identifier { id, reference: _ } => match ast.id_to_type(id) {
                            ast::Ty::Interface => format!(
                                "{}: *mut {}",
                                c::to_c_name(name),
                                to_rust_type(ast, ty).unwrap()
                            ),
                            ast::Ty::Struct => format!(
                                "{}: *mut {}",
                                c::to_c_name(name),
                                to_rust_type(ast, ty).unwrap()
                            ),
                            ref ty => format!(
                                "{}: {}",
                                c::to_c_name(id.name()),
                                to_rust_type(ast, ty).unwrap()
                            ),
                        },
                        // TODO struct implementation
                        ast::Ty::Struct => panic!("implement struct"),
                        ast::Ty::Vector {..} => {
                            let ty_name = to_rust_type(ast, ty).unwrap();
                            //if vec_ty.is_reference() {
                            //    format!("{ty}** out_{name}_{buffer}, size_t* {name}_{size}",
                            //            buffer = c::name_buffer(&name),
                            //            size = c::name_size(&name),
                            //            ty = ty_name,
                            //            name = c::to_c_name(name))
                            //} else {
                            format!("out_{name}_{buffer}: {ty}, {name}_{size}: libc::size_t, \
                                    out_{name}_actual: *mut libc::size_t",
                                    buffer = c::name_buffer(&ty_name),
                                    size = c::name_size(&name),
                                    ty = ty_name,
                                    name = c::to_c_name(name))
                        },
                        _ => format!("{}: {}", c::to_c_name(name), to_rust_type(ast, ty).unwrap()),
                    }
                })
                .collect();
            params.extend(out_params);


            fns.push(format!(
                include_str!("templates/rust/protocol_fn.rs"),
                fn_name = c::to_c_name(method.name.as_str()),
                return_param = return_param,
                params = params.join(", "),
            ));
        }
        Ok(fns.join("\n"))
    }

    fn codegen_protocol_fn_decl_safe(
        &self,
        methods: &Vec<Method>,
        ast: &BanjoAst,
    ) -> Result<String, Error> {
        let mut fns = Vec::new();
        for method in methods {
            let mut params: Vec<String> = method
                .in_params
                .iter()
                .map(|(name, ty)| {
                    format!("{}: {}", c::to_c_name(name), to_rust_type(ast, ty).unwrap())
                })
                .collect();

            let mut out_params_iter = method.out_params.iter();
            let (return_param, return_param_name) = match get_first_param(ast, method) {
                Some(p) => {
                    out_params_iter.next();
                    (to_rust_type(ast, &p.1)?, c::to_c_name(&p.0))
                }
                None => ("()".to_string(), "no_ret".to_string())
            };

            let out_params: Vec<String> = out_params_iter
                .map(|(name, ty)| {
                    match ty {
                        ast::Ty::Identifier { id, reference: _ } => {
                            match ast.id_to_type(id) {
                                ast::Ty::Interface => format!(
                                    "{}: *mut {}",
                                    c::to_c_name(name),
                                    to_rust_type(ast, ty).unwrap()
                                ),
                                ast::Ty::Struct => format!(
                                    "{}: *mut {}",
                                    c::to_c_name(name),
                                    to_rust_type(ast, ty).unwrap()
                                ),
                                // TODO struct implementation
                                ref ty => format!(
                                    "{}: {}",
                                    c::to_c_name(id.name()),
                                    to_rust_type(ast, ty).unwrap()
                                ),
                            }
                        }
                        // TODO struct implementation
                        ast::Ty::Vector {..} => {
                            let ty_name = to_rust_type(ast, ty).unwrap();
                            //if vec_ty.is_reference() {
                            //    format!("{ty}** out_{name}_{buffer}, size_t* {name}_{size}",
                            //            buffer = c::name_buffer(&name),
                            //            size = c::name_size(&name),
                            //            ty = ty_name,
                            //            name = c::to_c_name(name))
                            //} else {
                            format!("out_{name}_{buffer}: {ty}, {name}_{size}: libc::size_t, \
                                    out_{name}_actual: *mut libc::size_t",
                                    buffer = c::name_buffer(&ty_name),
                                    size = c::name_size(&name),
                                    ty = ty_name,
                                    name = c::to_c_name(name))
                                //}
                        },
                        ast::Ty::Struct => panic!("implement struct"),
                        _ => format!("{}: {}", c::to_c_name(name), to_rust_type(ast, ty).unwrap()),
                    }
                })
                .collect();
            params.extend(out_params);

            // params without types for interacting with C ABI
            // TODO out params
            let mut out_params_iter = method.out_params.iter();
            if get_first_param(ast, method).is_some() {
                out_params_iter.next();
            }

            let raw_params: Vec<String> = method
                .in_params
                .iter()
                .chain(out_params_iter)
                .map(|(name, ty)| {
                    match ty {
                        ast::Ty::Vector {..} => {
                            let ty_name = to_rust_type(ast, ty).unwrap();
                            format!("out_{name}_{buffer}, {name}_{size}, out_{name}_actual",
                                    buffer = c::name_buffer(&ty_name),
                                    size = c::name_size(&name),
                                    name = c::to_c_name(name))
                        },
                        _ => format!("{}", c::to_c_name(name))
                    }
                })
                .collect();

            fns.push(format!(
                include_str!("templates/rust/protocol_fn_safe.rs"),
                fn_name = c::to_c_name(method.name.as_str()),
                return_param = return_param,
                return_param_name = return_param_name,
                params = params.join(", "),
                raw_params = raw_params.join(", ")
            ));
        }
        Ok(fns.join("\n"))
    }
    fn codegen_protocol_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = String::new();
        for decl in namespace {
            if let ast::Decl::Interface { ref name, ref methods, attributes: _ } = *decl {
                let pfns = self.codegen_protocol_fn_decl(methods, ast)?;
                let safe_pfns = self.codegen_protocol_fn_decl_safe(methods, ast)?;
                accum.push_str(
                    format!(
                        include_str!("templates/rust/protocol.rs"),
                        protocol_fns = pfns,
                        safe_protocol_fns = safe_pfns,
                        protocol_name_upper = name.to_uppercase(),
                        protocol_name = name.to_camel_case(),
                    )
                    .as_str(),
                );
            }
        }
        Ok(accum)
    }

    fn codegen_const_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = Vec::new();
        for decl in namespace {
            if let ast::Decl::Constant { ref name, ref ty, ref value, attributes: _ } = *decl {
                let Constant(ref size) = value;
                accum.push(format!(
                    "pub const {name}: {ty} = {val};",
                    name = name.to_uppercase(),
                    ty = to_rust_type(ast, ty)?,
                    val = size,
                ));
            }
        }
        Ok(accum.join("\n"))
    }

    fn codegen_struct_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = Vec::new();
        for decl in namespace {
            if let ast::Decl::Struct { ref name, ref fields, attributes: _ } = *decl {
                let mut field_str = Vec::new();
                for field in fields {
                    field_str.push(format!(
                        "    pub {c_name}: {ty},",
                        c_name = c::to_c_name(field.ident.name()).as_str(),
                        ty = to_rust_type(ast, &field.ty)?
                    ));
                }
                accum.push(format!(
                    include_str!("templates/rust/struct.rs"),
                    name = c::to_c_name(name.as_str()) + "_t",
                    struct_fields = field_str.join("\n")
                ));
            }
        }
        Ok(accum.join("\n"))
    }

    fn codegen_includes(&self, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = String::new();
        for n in
            ast.namespaces.iter().filter(|n| *n.0 != "zx").filter(|n| *n.0 != ast.primary_namespace)
        {
            accum.push_str(
                format!(
                    "use banjo_ddk_{name} as {name};\n use {name}::*;\n",
                    name = namespace_to_use(n.0)
                )
                .as_str(),
            );
        }
        Ok(accum)
    }
}

impl<'a, W: io::Write> Backend<'a, W> for RustBackend<'a, W> {
    fn codegen(&mut self, ast: BanjoAst) -> Result<(), Error> {
        self.w.write_fmt(format_args!(
            include_str!("templates/rust/header.rs"),
            includes = self.codegen_includes(&ast)?,
            primary_namespace = ast.primary_namespace
        ))?;
        let namespace = &ast.namespaces[&ast.primary_namespace];
        self.w.write_fmt(format_args!(
            include_str!("templates/rust/body.rs"),
            enum_decls = self.codegen_enum_decl(namespace.iter(), &ast)?,
            constant_decls = self.codegen_const_decl(namespace.iter(), &ast)?,
            protocol_definitions = self.codegen_protocol_decl(namespace.iter(), &ast)?,
            struct_decls = self.codegen_struct_decl(namespace.iter(), &ast)?,
        ))?;
        Ok(())
    }
}
