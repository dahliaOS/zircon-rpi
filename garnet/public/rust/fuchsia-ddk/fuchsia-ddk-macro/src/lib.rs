// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Procedural macro crate for writting driver entry points
//!
//! This crate should not be depended upon directly, but should be used through
//! the fuchsia_ddk crate, which re-exports all these symbols.
//!
//! This crate defines attributes that allow for writing the above as just
//!
//! ```
//! #[fuchsia_ddk::bind_entry_point]
//! fn bind_init() -> Result<(), zx::Status> {
//!     // Contents of a DDK bind function
//!     Ok(())
//! }
//! ```

extern crate proc_macro;

use {
    proc_macro::TokenStream,
    quote::{quote, quote_spanned},
    syn::{
        parse::{Error}, //, Parse, ParseStream},
        parse_macro_input,
    },
};

#[proc_macro_attribute]
pub fn bind_entry_point(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let item = parse_macro_input!(item as syn::ItemFn);
    let syn::ItemFn {
        attrs,
        vis: _,
        constness,
        unsafety,
        asyncness,
        abi,
        ident,
        decl,
        block,
    } = item;
    if let Err(e) = (|| {
        // Disallow const, unsafe or abi linkage, generics etc
        if let Some(c) = constness {
            return Err(Error::new(c.span, "bind may not be 'const'"));
        }
        if let Some(u) = unsafety {
            return Err(Error::new(u.span, "bind may not be 'unsafe'"));
        }
        if let Some(abi) = abi {
            return Err(Error::new(
                abi.extern_token.span,
                "bind may not have custom linkage",
            ));
        }
        if let Some(abi) = asyncness {
            return Err(Error::new(
                abi.span,
                "bind may not be async",
            ));
        }
        if !decl.generics.params.is_empty() || decl.generics.where_clause.is_some() {
            return Err(Error::new(
                decl.fn_token.span,
                "bind may not have generics",
            ));
        }
        if decl.inputs.len() != 1 {
            // TODO(bwb): check type
            return Err(Error::new(
                decl.paren_token.span,
                "bind takes one argument of type Device<OpaqueCtx>",
            ));
        }
        if let Some(dot3) = decl.variadic {
            return Err(Error::new(dot3.spans[0], "bind may not be variadic"));
        }

        // Only allow on 'main' or 'test' functions
        //if ident.to_string() != "main"
//      //      && !test
        //    && !attrs.iter().any(|a| {
        //        a.parse_meta()
        //            .map(|m| {
        //                if let syn::Meta::Word(w) = m {
        //                    w == "test"
        //                } else {
        //                    false
        //                }
        //            })
        //            .unwrap_or(false)
        //    }) {
        //    return Err(Error::new(
        //        ident.span(),
        //        "bind must a 'main' or '#[test]'.",
        //    ));
        //}
        Ok(())
    })() {
        return e.to_compile_error().into();
    }

    let no_mangle = quote!{#[no_mangle]};
    let input = decl.inputs;
    let ret_type = decl.output; // TODO validate type is Result<(), zx::Status>;
    let span = ident.span();
    let output = quote_spanned!{span=>
        // Preserve any original attributes.
        #(#attrs)* #no_mangle
        pub extern "C" fn #ident(ctx: *mut libc::c_void,
                                 parent_device: *mut zx_device_t) -> fuchsia_zircon::sys::zx_status_t {
            let parent_device = unsafe {
                Device::<OpaqueCtx>::from_raw_ptr(parent_device)
            };
            let _bind = |#input| #ret_type {
                #block
            };
            match _bind(parent_device) {
                Ok(_) => fuchsia_zircon::sys::ZX_OK,
                Err(e) => e.into_raw()
            }
        }
    };
    output.into()
}
