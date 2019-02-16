// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![no_std]
#![allow(non_camel_case_types, non_snake_case)]

use fuchsia_zircon as zx;

#[repr(C)]
#[derive(Default, Debug, Copy, Clone)]
pub struct zx_device_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_device_prop_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct fidl_msg_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct fidl_txn_t {
    _unused: [u8; 0],
}

// Max device name length, not including a null-terminator
pub const ZX_DEVICE_NAME_MAX: u8 = 31;

// Current Version
// echo -n "zx_device_ops_v0.51" | sha256sum | cut -c1-16
pub const DEVICE_OPS_VERSION: u64 = 0xc4640f7115d2ee49;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_protocol_device_t {
    pub version: u64,
    pub get_protocol: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            proto_id: u32,
            protocol: *mut libc::c_void,
        ) -> zx::sys::zx_status_t,
    >,
    pub open: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            dev_out: *mut *mut zx_device_t,
            flags: u32,
        ) -> zx::sys::zx_status_t,
    >,
    pub open_at: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            dev_out: *mut *mut zx_device_t,
            path: *const libc::c_char,
            flags: u32,
        ) -> zx::sys::zx_status_t,
    >,
    pub close: ::core::option::Option<
        unsafe extern "C" fn(ctx: *mut libc::c_void, flags: u32) -> zx::sys::zx_status_t,
    >,
    pub unbind: ::core::option::Option<unsafe extern "C" fn(ctx: *mut libc::c_void)>,
    pub release: ::core::option::Option<unsafe extern "C" fn(ctx: *mut libc::c_void)>,
    pub read: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            buf: *mut libc::c_void,
            count: usize,
            off: zx::sys::zx_off_t,
            actual: *mut usize,
        ) -> zx::sys::zx_status_t,
    >,
    pub write: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            buf: *const libc::c_void,
            count: usize,
            off: zx::sys::zx_off_t,
            actual: *mut usize,
        ) -> zx::sys::zx_status_t,
    >,
    pub get_size:
        ::core::option::Option<unsafe extern "C" fn(ctx: *mut libc::c_void) -> zx::sys::zx_off_t>,
    pub ioctl: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            op: u32,
            in_buf: *const libc::c_void,
            in_len: usize,
            out_buf: *mut libc::c_void,
            out_len: usize,
            out_actual: *mut usize,
        ) -> zx::sys::zx_status_t,
    >,
    pub suspend: ::core::option::Option<
        unsafe extern "C" fn(ctx: *mut libc::c_void, flags: u32) -> zx::sys::zx_status_t,
    >,
    pub resume: ::core::option::Option<
        unsafe extern "C" fn(ctx: *mut libc::c_void, flags: u32) -> zx::sys::zx_status_t,
    >,
    pub rxrpc: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            channel: zx::sys::zx_handle_t,
        ) -> zx::sys::zx_status_t,
    >,
    pub message: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            msg: *mut fidl_msg_t,
            txn: *mut fidl_txn_t,
        ) -> zx::sys::zx_status_t,
    >,
}

extern "C" {
    pub fn device_get_name(dev: *mut zx_device_t) -> *const libc::c_char;
    pub fn device_get_parent(dev: *mut zx_device_t) -> *mut zx_device_t;
    pub fn device_get_protocol(
        dev: *const zx_device_t,
        proto_id: u32,
        protocol: *mut libc::c_void,
    ) -> zx::sys::zx_status_t;
    pub fn device_read(
        dev: *mut zx_device_t,
        buf: *mut libc::c_void,
        count: usize,
        off: zx::sys::zx_off_t,
        actual: *mut usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_write(
        dev: *mut zx_device_t,
        buf: *const libc::c_void,
        count: usize,
        off: zx::sys::zx_off_t,
        actual: *mut usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_get_size(dev: *mut zx_device_t) -> zx::sys::zx_off_t;
    pub fn device_ioctl(
        dev: *mut zx_device_t,
        op: u32,
        in_buf: *const libc::c_void,
        in_len: usize,
        out_buf: *mut libc::c_void,
        out_len: usize,
        out_actual: *mut usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_get_metadata(
        dev: *mut zx_device_t,
        type_: u32,
        buf: *mut libc::c_void,
        buflen: usize,
        actual: *mut usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_get_metadata_size(
        dev: *mut zx_device_t,
        type_: u32,
        out_size: *mut usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_add_metadata(
        dev: *mut zx_device_t,
        type_: u32,
        data: *const libc::c_void,
        length: usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_publish_metadata(
        dev: *mut zx_device_t,
        path: *const libc::c_char,
        type_: u32,
        data: *const libc::c_void,
        length: usize,
    ) -> zx::sys::zx_status_t;
    pub fn device_state_clr_set(
        dev: *mut zx_device_t,
        clearflag: zx::sys::zx_signals_t,
        setflag: zx::sys::zx_signals_t,
    );

}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_bind_inst_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_binding_t {
    _unused: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_ops_t {
    pub version: u64,
    pub init: ::core::option::Option<
        unsafe extern "C" fn(out_ctx: *mut *mut libc::c_void) -> zx::sys::zx_status_t,
    >,
    pub bind: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            device: *mut zx_device_t,
        ) -> zx::sys::zx_status_t,
    >,
    pub create: ::core::option::Option<
        unsafe extern "C" fn(
            ctx: *mut libc::c_void,
            parent: *mut zx_device_t,
            name: *const libc::c_char,
            args: *const libc::c_char,
            rpc_channel: zx::sys::zx_handle_t,
        ) -> zx::sys::zx_status_t,
    >,
    pub release: ::core::option::Option<unsafe extern "C" fn(ctx: *mut libc::c_void)>,
}

// echo -n "device_add_args_v0.5" | sha256sum | cut -c1-16
pub const DEVICE_ADD_ARGS_VERSION: u64 = 0x96a64134d56e88e3;

// echo -n "zx_driver_ops_v0.5" | sha256sum | cut -c1-16
pub const DRIVER_OPS_VERSION: u64 = 0x2b3490fa40d9f452;

pub const DEVICE_ADD_NON_BINDABLE: u32 = 1;
pub const DEVICE_ADD_INSTANCE: u32 = 2;
pub const DEVICE_ADD_MUST_ISOLATE: u32 = 4;
pub const DEVICE_ADD_INVISIBLE: u32 = 8;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct device_add_args_t {
    pub version: u64,
    pub name: *const libc::c_char,
    pub ctx: *mut libc::c_void,
    pub ops: *mut zx_protocol_device_t,
    pub props: *mut zx_device_prop_t,
    pub prop_count: u32,
    pub proto_id: u32,
    pub proto_ops: *mut libc::c_void,
    pub proxy_args: *const libc::c_char,
    pub flags: u32,
    pub client_remote: zx::sys::zx_handle_t,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_driver_rec_t {
    pub ops: *const zx_driver_ops_t,
    pub driver: *mut zx_driver_t,
    pub log_flags: u32,
}
extern "C" {
    #[link_name = "\u{1}__zircon_driver_rec__"]
    pub static mut __zircon_driver_rec__: zx_driver_rec_t;
    pub fn device_add_from_driver(
        drv: *mut zx_driver_t,
        parent: *mut zx_device_t,
        args: *mut device_add_args_t,
        out: *mut *mut zx_device_t,
    ) -> zx::sys::zx_status_t;
    pub fn device_remove(device: *mut zx_device_t) -> zx::sys::zx_status_t;
    pub fn device_rebind(device: *mut zx_device_t) -> zx::sys::zx_status_t;
    pub fn device_make_visible(device: *mut zx_device_t);
    pub fn get_root_resource() -> zx::sys::zx_handle_t;
    pub fn load_firmware(
        device: *mut zx_device_t,
        path: *const libc::c_char,
        fw: *mut zx::sys::zx_handle_t,
        size: *mut usize,
    ) -> zx::sys::zx_status_t;
}

pub const ZX_PROTOCOL_BLOCK: u32 = 1883393099;
pub const ZX_PROTOCOL_BLOCK_IMPL: u32 = 1883392835;
pub const ZX_PROTOCOL_BLOCK_PARTITION: u32 = 1883392848;
pub const ZX_PROTOCOL_BLOCK_VOLUME: u32 = 1883392854;
pub const ZX_PROTOCOL_CONSOLE: u32 = 1883459406;
pub const ZX_PROTOCOL_DEVICE: u32 = 1883522390;
pub const ZX_PROTOCOL_DISPLAY_CONTROLLER: u32 = 1883525955;
pub const ZX_PROTOCOL_DISPLAY_CONTROLLER_IMPL: u32 = 1883521865;
pub const ZX_PROTOCOL_ETHERNET: u32 = 1883591752;
pub const ZX_PROTOCOL_ETHMAC: u32 = 1883589953;
pub const ZX_PROTOCOL_FRAMEBUFFER: u32 = 1883656770;
pub const ZX_PROTOCOL_GPIO: u32 = 1883721807;
pub const ZX_PROTOCOL_GPIO_IMPL: u32 = 1883721801;
pub const ZX_PROTOCOL_HIDBUS: u32 = 1883785540;
pub const ZX_PROTOCOL_I2C: u32 = 1883845187;
pub const ZX_PROTOCOL_I2C_IMPL: u32 = 1883845193;
pub const ZX_PROTOCOL_INPUT: u32 = 1883852368;
pub const ZX_PROTOCOL_ROOT: u32 = 1883324737;
pub const ZX_PROTOCOL_MISC: u32 = 1884115779;
pub const ZX_PROTOCOL_MISC_PARENT: u32 = 1884115792;
pub const ZX_PROTOCOL_ACPI: u32 = 1883325264;
pub const ZX_PROTOCOL_PCI: u32 = 1884308297;
pub const ZX_PROTOCOL_PCIROOT: u32 = 1884312148;
pub const ZX_PROTOCOL_TPM: u32 = 1884573773;
pub const ZX_PROTOCOL_USB: u32 = 1884640066;
pub const ZX_PROTOCOL_USB_BUS: u32 = 1884635731;
pub const ZX_PROTOCOL_USB_COMPOSITE: u32 = 1884635715;
pub const ZX_PROTOCOL_USB_DCI: u32 = 1884636227;
pub const ZX_PROTOCOL_USB_DEVICE: u32 = 1884640068;
pub const ZX_PROTOCOL_USB_PERIPHERAL: u32 = 1884640080;
pub const ZX_PROTOCOL_USB_FUNCTION: u32 = 1884640070;
pub const ZX_PROTOCOL_USB_HCI: u32 = 1884637257;
pub const ZX_PROTOCOL_USB_MODE_SWITCH: u32 = 1884638547;
pub const ZX_PROTOCOL_USB_DBC: u32 = 1884636226;
pub const ZX_PROTOCOL_USB_TESTER: u32 = 1884640338;
pub const ZX_PROTOCOL_USB_TEST_FWLOADER: u32 = 1884640326;
pub const ZX_PROTOCOL_BT_HCI: u32 = 1883392067;
pub const ZX_PROTOCOL_BT_TRANSPORT: u32 = 1883395154;
pub const ZX_PROTOCOL_BT_HOST: u32 = 1883395144;
pub const ZX_PROTOCOL_BT_GATT_SVC: u32 = 1883391827;
pub const ZX_PROTOCOL_AUDIO: u32 = 1883329860;
pub const ZX_PROTOCOL_MIDI: u32 = 1884113220;
pub const ZX_PROTOCOL_SDHCI: u32 = 1884505160;
pub const ZX_PROTOCOL_SDMMC: u32 = 1884505165;
pub const ZX_PROTOCOL_SDIO: u32 = 1884505161;
pub const ZX_PROTOCOL_WLANPHY: u32 = 1884769360;
pub const ZX_PROTOCOL_WLANPHY_IMPL: u32 = 1884770377;
pub const ZX_PROTOCOL_WLANIF: u32 = 1884769353;
pub const ZX_PROTOCOL_WLANIF_IMPL: u32 = 1884768585;
pub const ZX_PROTOCOL_WLANMAC: u32 = 1884769357;
pub const ZX_PROTOCOL_AUDIO_INPUT: u32 = 1883329865;
pub const ZX_PROTOCOL_AUDIO_OUTPUT: u32 = 1883329871;
pub const ZX_PROTOCOL_CAMERA: u32 = 1883455821;
pub const ZX_PROTOCOL_MEDIA_CODEC: u32 = 1884111686;
pub const ZX_PROTOCOL_BATTERY: u32 = 1883390292;
pub const ZX_PROTOCOL_POWER: u32 = 1884313426;
pub const ZX_PROTOCOL_THERMAL: u32 = 1884571725;
pub const ZX_PROTOCOL_GPU_THERMAL: u32 = 1883721812;
pub const ZX_PROTOCOL_PTY: u32 = 1884312665;
pub const ZX_PROTOCOL_IHDA: u32 = 1883784257;
pub const ZX_PROTOCOL_IHDA_CODEC: u32 = 1883850819;
pub const ZX_PROTOCOL_IHDA_DSP: u32 = 1885947972;
pub const ZX_PROTOCOL_AUDIO_CODEC: u32 = 1883329859;
pub const ZX_PROTOCOL_TEST: u32 = 1884574548;
pub const ZX_PROTOCOL_TEST_PARENT: u32 = 1884574544;
pub const ZX_PROTOCOL_PBUS: u32 = 1884308053;
pub const ZX_PROTOCOL_PDEV: u32 = 1884308566;
pub const ZX_PROTOCOL_PLATFORM_PROXY: u32 = 1884311634;
pub const ZX_PROTOCOL_I2C_HID: u32 = 1883850820;
pub const ZX_PROTOCOL_SERIAL: u32 = 1884513650;
pub const ZX_PROTOCOL_SERIAL_IMPL: u32 = 1884516969;
pub const ZX_PROTOCOL_CLK: u32 = 1883458635;
pub const ZX_PROTOCOL_INTEL_GPU_CORE: u32 = 1883850563;
pub const ZX_PROTOCOL_IOMMU: u32 = 1883852621;
pub const ZX_PROTOCOL_NAND: u32 = 1884180036;
pub const ZX_PROTOCOL_RAW_NAND: u32 = 1884442180;
pub const ZX_PROTOCOL_BAD_BLOCK: u32 = 1883390540;
pub const ZX_PROTOCOL_MAILBOX: u32 = 1884112981;
pub const ZX_PROTOCOL_SCPI: u32 = 1884504912;
pub const ZX_PROTOCOL_BACKLIGHT: u32 = 1883392844;
pub const ZX_PROTOCOL_AMLOGIC_CANVAS: u32 = 1883455822;
pub const ZX_PROTOCOL_SKIP_BLOCK: u32 = 1884506946;
pub const ZX_PROTOCOL_ETH_BOARD: u32 = 1883591746;
pub const ZX_PROTOCOL_ETH_MAC: u32 = 1883591757;
pub const ZX_PROTOCOL_QMI_TRANSPORT: u32 = 1884376393;
pub const ZX_PROTOCOL_MIPI_CSI: u32 = 1884113232;
pub const ZX_PROTOCOL_LIGHT: u32 = 1884047687;
pub const ZX_PROTOCOL_ISP_IMPL: u32 = 1883852876;
pub const ZX_PROTOCOL_ISP: u32 = 1883853648;
pub const ZX_PROTOCOL_GPU: u32 = 1883721813;
pub const ZX_PROTOCOL_RTC: u32 = 1884443715;
pub const ZX_PROTOCOL_TEE: u32 = 1884570949;
pub const ZX_PROTOCOL_VSOCK: u32 = 1884705611;
pub const ZX_PROTOCOL_SYSMEM: u32 = 1884518733;
pub const ZX_PROTOCOL_MLG: u32 = 1884113991;
