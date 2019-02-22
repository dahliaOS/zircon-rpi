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

#![allow(warnings)]
extern crate proc_macro;

use {
    proc_macro::TokenStream,
    proc_macro2::{Ident, Span},
    quote::{quote, quote_spanned},
    syn::{parse::Error, parse_macro_input, spanned::Spanned, ImplItem::Method},
};

#[proc_macro_attribute]
/// Used to annotated a DDK protocol operations impl on a context
/// builds a static table used by the DDK that forwards implemented
/// operations to the safe rust functions.
pub fn device_ops(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let item_impl = parse_macro_input!(item as syn::ItemImpl);
    let syn::ItemImpl { self_ty, trait_, items, .. } = item_impl;

    // populate the static table with default values or calls to unsafe functions
    let mut static_table = syn::punctuated::Punctuated::new();
    'method: for method in [
        "close",
        "message",
        "get_protocol",
        "get_size",
        "release",
        "resume",
        "rxrpc",
        "suspend",
        "unbind",
    ]
    .iter()
    {
        let method_ident = Ident::new(method, Span::call_site());
        for item in items.iter() {
            if let Method(item_method) = item {
                if (method_ident == item_method.sig.ident) {
                    let unsafe_name = String::from(*method) + "_unsafe";
                    let unsafe_method_ident = Ident::new(unsafe_name.as_str(), Span::call_site());
                    static_table.push_value(
                        quote! {#method_ident: Some(fuchsia_ddk::#unsafe_method_ident::<#self_ty>)},
                    );
                    static_table.push_punct(quote! {,});
                    continue 'method;
                }
            }
        }
        static_table.push_value(quote! {#method_ident: None});
        static_table.push_punct(quote! {,});
    }

    // re-populate the methods for the trait
    let mut method_impls = syn::punctuated::Punctuated::new();
    for item in items.iter() {
        method_impls.push_value(quote! {#item});
        method_impls.push_punct(quote! {});
    }

    let span = (*self_ty).span();
    let name = quote!(#self_ty).to_string();
    let static_name = String::from("_DEVICE_OPS_") + name.as_str();
    let static_ident = Ident::new(static_name.as_str(), Span::call_site());
    let output = quote_spanned! {span=>
        pub static #static_ident : fuchsia_ddk::sys::zx_protocol_device_t = fuchsia_ddk::sys::zx_protocol_device_t {
            version: DEVICE_OPS_VERSION,
            open: None, // TODO(bwb): remove when DDK removes
            read: None, // TODO(bwb): remove when DDK removes
            write: None, // TODO(bwb): remove when DDK removes
            ioctl: None,  // TODO(bwb): remove when DDK removes
            open_at: None, // TODO(bwb): remove when DDK removes
            #static_table
        };

        impl fuchsia_ddk::DeviceOps for #self_ty {
            fn get_device_protocol() -> &'static fuchsia_ddk::sys::zx_protocol_device_t {
                &#static_ident
            }

            #method_impls
        }
    };
    output.into()
}

#[proc_macro_attribute]
/// Used to annotate the bind entry points for a driver
pub fn bind_entry_point(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let item = parse_macro_input!(item as syn::ItemFn);
    let syn::ItemFn { attrs, vis: _, constness, unsafety, asyncness, abi, ident, decl, block } =
        item;
    if let Err(e) = (|| {
        // Disallow const, unsafe or abi linkage, generics etc
        if let Some(c) = constness {
            return Err(Error::new(c.span, "bind may not be 'const'"));
        }
        if let Some(u) = unsafety {
            return Err(Error::new(u.span, "bind may not be 'unsafe'"));
        }
        if let Some(abi) = abi {
            return Err(Error::new(abi.extern_token.span, "bind may not have custom linkage"));
        }
        if let Some(abi) = asyncness {
            return Err(Error::new(abi.span, "bind may not be async"));
        }
        if !decl.generics.params.is_empty() || decl.generics.where_clause.is_some() {
            return Err(Error::new(decl.fn_token.span, "bind may not have generics"));
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

        Ok(())
    })() {
        return e.to_compile_error().into();
    }

    let no_mangle = quote! {#[no_mangle]};
    let input = decl.inputs;
    let ret_type = decl.output; // TODO validate type is Result<(), zx::Status>;
    let span = ident.span();
    let output = quote_spanned! {span=>
        // Preserve any original attributes.
        #(#attrs)* #no_mangle
        pub extern "C" fn #ident(ctx: *mut libc::c_void,
                                 parent_device: *mut zx_device_t) -> fuchsia_zircon::sys::zx_status_t {
            let parent_device: fuchsia_ddk::Device<fuchsia_ddk::OpaqueCtx> = unsafe {
                fuchsia_ddk::Device::<fuchsia_ddk::OpaqueCtx>::from_raw_ptr(parent_device)
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
