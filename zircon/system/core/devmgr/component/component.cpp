// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "component.h"

#include <ddk/debug.h>
#include <fbl/algorithm.h>

#include <memory>

#include "proxy-protocol.h"

namespace component {

Component::Component(zx_device_t* parent)
        : ComponentBase(parent) {

    // These protocols are all optional, so no error checking is necessary here.
    device_get_protocol(parent, ZX_PROTOCOL_AMLOGIC_CANVAS, &canvas_);
    device_get_protocol(parent, ZX_PROTOCOL_CLOCK, &clock_);
    device_get_protocol(parent, ZX_PROTOCOL_GPIO, &gpio_);
    device_get_protocol(parent, ZX_PROTOCOL_I2C, &i2c_);
    device_get_protocol(parent, ZX_PROTOCOL_PDEV, &pdev_);
    device_get_protocol(parent, ZX_PROTOCOL_POWER, &power_);
    device_get_protocol(parent, ZX_PROTOCOL_SYSMEM, &sysmem_);
}

zx_status_t Component::Bind(void* ctx, zx_device_t* parent) {
    auto dev = std::make_unique<Component>(parent);
    // The thing before the comma will become the process name, if a new process
    // is created
    const char* proxy_args = "composite-device,";
    auto status = dev->DdkAdd("component", DEVICE_ADD_NON_BINDABLE | DEVICE_ADD_MUST_ISOLATE,
                              nullptr /* props */, 0 /* prop count */, 0 /* proto id */,
                              proxy_args);
    if (status == ZX_OK) {
        // devmgr owns the memory now
        __UNUSED auto ptr = dev.release();
    }
    return status;
}

zx_status_t Component::DdkRxrpc(zx_handle_t raw_channel) {
    zx::unowned_channel channel(raw_channel);
    if (!channel->is_valid()) {
        // This driver is stateless, so we don't need to reset anything here
        return ZX_OK;
    }

    uint8_t req_buf[PROXY_MAX_TRANSFER_SIZE];
    uint8_t resp_buf[PROXY_MAX_TRANSFER_SIZE];
    auto* req_header = reinterpret_cast<proxy_req_t*>(&req_buf);
    auto* resp_header = reinterpret_cast<proxy_rsp_t*>(&resp_buf);
    uint32_t actual;
    zx_handle_t req_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    zx_handle_t resp_handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    uint32_t req_handle_count;
    uint32_t resp_handle_count = 0;

    auto status = zx_channel_read(raw_channel, 0, &req_buf, req_handles, sizeof(req_buf),
                                  fbl::count_of(req_handles), &actual, &req_handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_read failed %d\n", status);
        return status;
    }

    resp_header->txid = req_header->txid;
    uint32_t resp_len;

    switch (req_header->proto_id) {
    case ZX_PROTOCOL_PDEV: {
        if (pdev_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_pdev_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_pdev_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case PDEV_GET_MMIO:
            pdev_mmio_t mmio;
            status = pdev_get_mmio(&pdev_, req->index, &mmio);
            if (status == ZX_OK) {
                resp->offset = mmio.offset;
                resp->size = mmio.size;
                resp_handles[0] = mmio.vmo;
                resp_handle_count = 1;
            }
            break;
        case PDEV_GET_INTERRUPT:
            status = pdev_get_interrupt(&pdev_, req->index, req->flags, &resp_handles[0]);
            if (status == ZX_OK) {
                resp_handle_count = 1;
            }
            break;
        case PDEV_GET_BTI:
            status = pdev_get_bti(&pdev_, req->index, &resp_handles[0]);
            if (status == ZX_OK) {
                resp_handle_count = 1;
            }
            break;
        case PDEV_GET_SMC:
            status = pdev_get_smc(&pdev_, req->index, &resp_handles[0]);
            if (status == ZX_OK) {
                resp_handle_count = 1;
            }
            break;
        case PDEV_GET_DEVICE_INFO:
            status = pdev_get_device_info(&pdev_, &resp->device_info);
            break;
        case PDEV_GET_BOARD_INFO:
            status = pdev_get_board_info(&pdev_, &resp->board_info);
            break;
        default:
            zxlogf(ERROR, "%s: unknown pdev op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_GPIO: {
        if (gpio_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_gpio_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_gpio_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case GPIO_CONFIG_IN:
            status = gpio_config_in(&gpio_, req->flags);
            break;
        case GPIO_CONFIG_OUT:
            status = gpio_config_out(&gpio_, req->value);
            break;
        case GPIO_SET_ALT_FUNCTION:
            status = gpio_set_alt_function(&gpio_, req->alt_function);
            break;
        case GPIO_READ:
            status = gpio_read(&gpio_, &resp->value);
            break;
        case GPIO_WRITE:
            status = gpio_write(&gpio_, req->value);
            break;
        case GPIO_GET_INTERRUPT:
            status = gpio_get_interrupt(&gpio_, req->flags, &resp_handles[0]);
            if (status == ZX_OK) {
                resp_handle_count = 1;
            }
            break;
        case GPIO_RELEASE_INTERRUPT:
            status = gpio_release_interrupt(&gpio_);
            break;
        case GPIO_SET_POLARITY:
            status = gpio_set_polarity(&gpio_, req->polarity);
            break;
        default:
            zxlogf(ERROR, "%s: unknown GPIO op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_POWER: {
        if (power_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_power_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __FUNCTION__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }

        auto resp = reinterpret_cast<rpc_power_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);
        switch (req_header->op) {
        case POWER_ENABLE:
            status = power_enable_power_domain(&power_);
            break;
        case POWER_DISABLE:
            status = power_disable_power_domain(&power_);
            break;
        case POWER_GET_STATUS:
            status = power_get_power_domain_status(&power_, &resp->status);
            break;
        default:
            zxlogf(ERROR, "%s: unknown Power op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_I2C: {
       if (i2c_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_i2c_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_i2c_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case I2C_GET_MAX_TRANSFER:
            status = i2c_get_max_transfer_size(&i2c_, &resp->max_transfer);
            break;
/*
        case I2C_TRANSACT: {
            status = RpcI2cTransact(req_header->txid, req, raw_channel);
            if (status == ZX_OK) {
                // If platform_i2c_transact succeeds, we return immmediately instead of calling
                // zx_channel_write below. Instead we will respond in platform_i2c_complete().
                return ZX_OK;
            }
            break;
        }
*/
        case I2C_GET_INTERRUPT:
            status = i2c_get_interrupt(&i2c_, req->flags, &resp_handles[0]);
            if (status == ZX_OK) {
                resp_handle_count = 1;
            }
            break;
        default:
            zxlogf(ERROR, "%s: unknown I2C op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_CLOCK: {
       if (clock_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_clk_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        resp_len = sizeof(*resp_header);

        switch (req_header->op) {
        case CLK_ENABLE:
            status = clock_enable(&clock_, req->index);
            break;
        case CLK_DISABLE:
            status = clock_disable(&clock_, req->index);
            break;
        default:
            zxlogf(ERROR, "%s: unknown clk op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_SYSMEM: {
        if (sysmem_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<proxy_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        if (req_handle_count != 1) {
            zxlogf(ERROR, "%s received %u handles, expecting 1\n", __func__, req_handle_count);
            return ZX_ERR_INTERNAL;
        }
        resp_len = sizeof(*resp_header);

        switch (req_header->op) {
        case SYSMEM_CONNECT:
            status = sysmem_connect(&sysmem_, req_handles[0]);
            break;
        default:
            zxlogf(ERROR, "%s: unknown clk op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    case ZX_PROTOCOL_AMLOGIC_CANVAS: {
       if (canvas_.ops == nullptr) {
            return ZX_ERR_NOT_SUPPORTED;
        }
        auto req = reinterpret_cast<rpc_amlogic_canvas_req_t*>(&req_buf);
        if (actual < sizeof(*req)) {
            zxlogf(ERROR, "%s received %u, expecting %zu\n", __func__, actual, sizeof(*req));
            return ZX_ERR_INTERNAL;
        }
        if (req_handle_count != 1) {
            zxlogf(ERROR, "%s received %u handles, expecting 1\n", __func__, req_handle_count);
            return ZX_ERR_INTERNAL;
        }
        auto resp = reinterpret_cast<rpc_amlogic_canvas_rsp_t*>(&resp_buf);
        resp_len = sizeof(*resp);

        switch (req_header->op) {
        case AMLOGIC_CANVAS_CONFIG:
            status = amlogic_canvas_config(&canvas_, req_handles[0], req->offset, &req->info, &resp->canvas_idx);
            break;
        case AMLOGIC_CANVAS_FREE:
            status = amlogic_canvas_free(&canvas_, req->canvas_idx);
            break;
        default:
            zxlogf(ERROR, "%s: unknown clk op %u\n", __func__, req_header->op);
            return ZX_ERR_INTERNAL;
        }
        break;
    }
    default:
        zxlogf(ERROR, "%s: unknown protocol %u\n", __func__, req_header->proto_id);
        return ZX_ERR_INTERNAL;
    }

    // set op to match request so zx_channel_write will return our response
    resp_header->status = status;
    status = zx_channel_write(raw_channel, 0, resp_header, resp_len,
                              (resp_handle_count ? resp_handles : nullptr), resp_handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_write failed %d\n", status);
    }
    return status;
}

void Component::DdkUnbind() {
    DdkRemove();
}

void Component::DdkRelease() {
    delete this;
}

const zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Component::Bind;
    return ops;
}();

} // namespace component

ZIRCON_DRIVER_BEGIN(component, component::driver_ops, "zircon", "0.1", 1)
BI_MATCH() // This driver is excluded from the normal matching process, so this is fine.
ZIRCON_DRIVER_END(component)
