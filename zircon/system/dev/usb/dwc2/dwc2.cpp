// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dwc2.h"

#include <ddk/binding.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform-device-lib.h>
#include <ddktl/pdev.h>
#include <ddktl/protocol/composite.h>
#include <ddktl/protocol/platform/device.h>
#include <fbl/algorithm.h>
#include <usb/usb-request.h>

namespace dwc2 {

void Dwc2::HandleReset() {
    auto* mmio = get_mmio();

    zxlogf(LINFO, "\nRESET\n");

    ep0_state_ = Ep0State::DISCONNECTED;

	/* Clear the Remote Wakeup Signalling */
    DCTL::Get().ReadFrom(mmio).set_rmtwkupsig(0).WriteTo(mmio);

    for (int i = 0; i < MAX_EPS_CHANNELS; i++) {
        auto diepctl = DEPCTL::Get(i).ReadFrom(mmio);

        if (diepctl.epena()) {
            // disable all active IN EPs
            diepctl.set_snak(1);
            diepctl.set_epdis(1);
            diepctl.WriteTo(mmio);
        }

        DEPCTL::Get(i + 16).ReadFrom(mmio).set_snak(1).WriteTo(mmio);
    }

    /* Flush the NP Tx FIFO */
    FlushFifo(0);

    /* Flush the Learning Queue */
    GRSTCTL::Get().ReadFrom(mmio).set_intknqflsh(1).WriteTo(mmio);

    // EPO IN and OUT
    DAINTMSK::Get().FromValue((1 << DWC_EP0_IN) | (1 << DWC_EP0_OUT)).WriteTo(mmio);

    DOEPMSK::Get().FromValue(0).
        set_setup(1).
        set_stsphsercvd(1).
        set_sr(1).
        set_xfercompl(1).
        set_ahberr(1).
        set_epdisabled(1).
        set_bna(1).
        WriteTo(mmio);
    DIEPMSK::Get().FromValue(0).
        set_xfercompl(1).
        set_timeout(1).
        set_ahberr(1).
        set_bna(1).
        set_epdisabled(1).
        WriteTo(mmio);

    /* Reset Device Address */
    DCFG::Get().ReadFrom(mmio).set_devaddr(0).set_epmscnt(2).set_descdma(0).WriteTo(mmio);

    // TODO how to detect disconnect?
    // also notify later if dci_intf_ not set yet.
    if (dci_intf_.has_value()) {
        dci_intf_->SetConnected(true);
    }
}

void Dwc2::HandleSuspend() {
    zxlogf(INFO, "Dwc2::HandleSuspend\n");
}

void Dwc2::HandleEnumDone() {
    auto* mmio = get_mmio();

    zxlogf(INFO, "HandleEnumDone\n");

/*
    if (dwc->astro_usb.ops) {
        astro_usb_do_usb_tuning(&dwc->astro_usb, false, false);
    }
*/
    
    // ???
    ep0_state_ = Ep0State::IDLE;

    endpoints_[DWC_EP0_IN].max_packet_size = 64;
    endpoints_[DWC_EP0_OUT].max_packet_size = 64;
    endpoints_[DWC_EP0_IN].phys = (uint32_t)ep0_buffer_.phys();
    endpoints_[DWC_EP0_OUT].phys = (uint32_t)ep0_buffer_.phys();

    DEPCTL::Get(DWC_EP0_IN).ReadFrom(mmio).set_mps(DWC_DEP0CTL_MPS_64).WriteTo(mmio);
    DEPCTL::Get(DWC_EP0_OUT).ReadFrom(mmio).set_mps(DWC_DEP0CTL_MPS_64).WriteTo(mmio);

    DCTL::Get().ReadFrom(mmio).set_cgnpinnak(1).WriteTo(mmio);

    GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(metadata_.usb_turnaround_time).WriteTo(mmio);

    if (dci_intf_.has_value()) {
        dci_intf_->SetSpeed(USB_SPEED_HIGH);
    }
    StartEp0();
}

void Dwc2::HandleInEpInterrupt() {
    auto* mmio = get_mmio();
    uint8_t ep_num = 0;

    uint32_t ep_bits = DAINT::Get().ReadFrom(mmio).reg_value();
    ep_bits &= DAINTMSK::Get().ReadFrom(mmio).reg_value();
    ep_bits &= DWC_EP_IN_MASK;

    DAINT::Get().FromValue(DWC_EP_IN_MASK).WriteTo(mmio);

    while (ep_bits) {
        if (ep_bits & 1) {
            auto diepint = DIEPINT::Get(ep_num).ReadFrom(mmio);
            diepint.set_reg_value(diepint.reg_value() & DIEPMSK::Get().ReadFrom(mmio).reg_value());

            /* Transfer complete */
            if (diepint.xfercompl()) {
                DIEPINT::Get(ep_num).FromValue(0).set_xfercompl(1).WriteTo(mmio);
                if (ep_num == DWC_EP0_IN) {
                    HandleEp0TransferComplete();
                } else {
                    HandleTransferComplete(ep_num);
                    if (diepint.nak()) {
printf("diepint.nak ep_num %u\n", ep_num);
                        DIEPINT::Get(ep_num).ReadFrom(mmio).set_nak(1).WriteTo(mmio);
                    }
                }
            }
            if (diepint.bna()) {
zxlogf(LINFO, "HandleInEpInterrupt bna\n");
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_bna(1).WriteTo(mmio);
            }

            /* Endpoint disable  */
            if (diepint.epdisabled()) {
printf("HandleInEpInterrupt diepint.epdisabled\n");
                /* Clear the bit in DIEPINTn for this interrupt */
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_epdisabled(1).WriteTo(mmio);
            }
            /* AHB Error */
            if (diepint.ahberr()) {
printf("HandleInEpInterrupt diepint.ahberr\n");
                /* Clear the bit in DIEPINTn for this interrupt */
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_ahberr(1).WriteTo(mmio);
            }
            /* TimeOUT Handshake (non-ISOC IN EPs) */
            if (diepint.timeout()) {
printf("HandleInEpInterrupt diepint.timeout\n");
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_timeout(1).WriteTo(mmio);
            }
            /** IN Token received with TxF Empty */
            if (diepint.intktxfemp()) {
printf("HandleInEpInterrupt diepint.intktxfemp\n");
               DIEPINT::Get(ep_num).ReadFrom(mmio).set_intktxfemp(1).WriteTo(mmio);
            }
            /** IN Token Received with EP mismatch */
            if (diepint.intknepmis()) {
printf("HandleInEpInterrupt diepint.intknepmis\n");
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_intknepmis(1).WriteTo(mmio);
            }
            /** IN Endpoint NAK Effective */
            if (diepint.inepnakeff()) {
     printf("HandleInEpInterrupt diepint.inepnakeff\n");
                DIEPINT::Get(ep_num).ReadFrom(mmio).set_inepnakeff(1).WriteTo(mmio);
            }
        }
        ep_num++;
        ep_bits >>= 1;
    }
}

void Dwc2::HandleOutEpInterrupt() {
    auto* mmio = get_mmio();

    uint8_t ep_num = DWC_EP0_OUT;

    /* Read in the device interrupt bits */
    auto ep_bits = DAINT::Get().ReadFrom(mmio).reg_value();
    auto ep_mask = DAINTMSK::Get().ReadFrom(mmio).reg_value();
    ep_bits &= ep_mask;
    ep_bits &= DWC_EP_OUT_MASK;
    ep_bits >>= DWC_EP_OUT_SHIFT;

    /* Clear the interrupt */
    DAINT::Get().FromValue(DWC_EP_OUT_MASK).WriteTo(mmio);

    while (ep_bits) {
        if (ep_bits & 1) {
            auto doepint = DOEPINT::Get(ep_num).ReadFrom(mmio);
            doepint.set_reg_value(doepint.reg_value() & DOEPMSK::Get().ReadFrom(mmio).reg_value());

            if (doepint.sr()) {
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_sr(1).WriteTo(mmio);
            }

            if (doepint.stsphsercvd()) {
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_stsphsercvd(1).WriteTo(mmio);
            }
            if (doepint.bna()) {
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_bna(1).WriteTo(mmio);
            }

            if (doepint.setup()) {
// TODO:   On this interrupt, the application must read the DOEPTSIZn register to determine the
//         number of SETUP packets received and process the last received SETUP packet.          
//         check Back2BackSETup

                DOEPINT::Get(ep_num).ReadFrom(mmio).set_setup(1).WriteTo(mmio);
                memcpy(&cur_setup_, ep0_buffer_.virt(), sizeof(cur_setup_));
                zxlogf(LTRACE, "SETUP bmRequestType: 0x%02x bRequest: %u wValue: %u wIndex: %u wLength: %u\n",
                        cur_setup_.bmRequestType, cur_setup_.bRequest, cur_setup_.wValue, cur_setup_.wIndex,
                        cur_setup_.wLength);

// do this somewhere else?
ep0_buffer_.CacheFlushInvalidate(0, sizeof(cur_setup_));

                HandleEp0Setup();
            }
            if (doepint.xfercompl()) {
                /* Clear the bit in DOEPINTn for this interrupt */
                DOEPINT::Get(ep_num).FromValue(0).set_xfercompl(1).WriteTo(mmio);

                if (ep_num == DWC_EP0_OUT) {
                    if (!doepint.setup()) {
                        HandleEp0TransferComplete();
                    }
                } else {
                    HandleTransferComplete(ep_num);
                }
            }
            if (doepint.epdisabled()) {
zxlogf(LINFO, "HandleOutEpInterrupt epdisabled\n");
                /* Clear the bit in DOEPINTn for this interrupt */
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_epdisabled(1).WriteTo(mmio);
            }
            if (doepint.ahberr()) {
zxlogf(LINFO, "HandleOutEpInterrupt ahberr\n");
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_ahberr(1).WriteTo(mmio);
            }
        }
        ep_num++;
        ep_bits >>= 1;
    }
}

zx_status_t Dwc2::HandleSetup(size_t* out_actual) {
    zx_status_t status;

    auto* setup = &cur_setup_;
    auto* buffer = ep0_buffer_.virt();

    if (setup->bmRequestType == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
        // handle some special setup requests in this driver
        switch (setup->bRequest) {
        case USB_REQ_SET_ADDRESS:
            zxlogf(INFO, "SET_ADDRESS %d\n", setup->wValue);
            SetAddress(static_cast<uint8_t>(setup->wValue));
            *out_actual = 0;
            return ZX_OK;
        case USB_REQ_SET_CONFIGURATION:
            zxlogf(INFO, "SET_CONFIGURATION %d\n", setup->wValue);
            StopEndpoints();
                configured_ = true;
            if (dci_intf_.has_value()) {
                status = dci_intf_->Control(setup, nullptr, 0, nullptr, 0, nullptr);
            } else {
                status = ZX_ERR_NOT_SUPPORTED;
            }
            if (status == ZX_OK && setup->wValue) {
                StartEndpoints();
            } else {
                configured_ = false;
            }
            return status;
        default:
            // fall through to dci_intf_->Control()
            break;
        }
    } else if (setup->bmRequestType == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) &&
               setup->bRequest == USB_REQ_SET_INTERFACE) {
        zxlogf(INFO, "SET_INTERFACE %d\n", setup->wValue);
        StopEndpoints();
        configured_ = true;
        if (dci_intf_.has_value()) {
            status = dci_intf_->Control(setup, nullptr, 0, nullptr, 0, nullptr);
        } else {
            status = ZX_ERR_NOT_SUPPORTED;
        }
        if (status == ZX_OK) {
            StartEndpoints();
        } else {
            configured_ = false;
        }
        return status;
    }

    bool is_in = ((setup->bmRequestType & USB_DIR_MASK) == USB_DIR_IN);
    auto length = le16toh(setup->wLength);

    if (dci_intf_.has_value()) {
        if (length == 0) {
            status = dci_intf_->Control(setup, nullptr, 0, nullptr, 0, nullptr);
        } else if (is_in) {
            status = dci_intf_->Control(setup, nullptr, 0, buffer, length, out_actual);
        } else {
            status = -1;
        }
    } else {
        status = ZX_ERR_NOT_SUPPORTED;
    }
    if (status == ZX_OK) {
        auto* ep = &endpoints_[DWC_EP0_OUT];
        ep->req_offset = 0;
        if (is_in) {
            ep->req_length = static_cast<uint32_t>(*out_actual);
        }
    }
    return status;
}

void Dwc2::SetAddress(uint8_t address) {
    auto* mmio = get_mmio();

    DCFG::Get().ReadFrom(mmio).set_devaddr(address).WriteTo(mmio);
}

uint32_t Dwc2::ReadTransfered(Endpoint* ep) {
    auto* mmio = get_mmio();
    return ep->req_xfersize -  DEPTSIZ::Get(ep->ep_num).ReadFrom(mmio).xfersize();
}

void Dwc2::StartEp0() {
    auto* mmio = get_mmio();
    auto* ep = &endpoints_[DWC_EP0_OUT];
    ep->req_offset = 0;
    ep->send_zlp = 0;
    ep->req_xfersize = 3 * sizeof(usb_setup_t);

    ep0_buffer_.CacheFlushInvalidate(0, sizeof(cur_setup_));


    DEPDMA::Get(DWC_EP0_OUT).FromValue(0).set_addr((uint32_t)ep0_buffer_.phys()).WriteTo(get_mmio());

    auto doeptsize0 = DEPTSIZ0::Get(DWC_EP0_OUT).FromValue(0);

    doeptsize0.set_supcnt(3);
    doeptsize0.set_pktcnt(1);
    doeptsize0.set_xfersize(ep->req_xfersize);
    doeptsize0.WriteTo(mmio);

    DEPCTL::Get(DWC_EP0_OUT).ReadFrom(mmio).set_epena(1).WriteTo(mmio);
}

void Dwc2::EpQueueNextLocked(Endpoint* ep) {
    std::optional<Request> req;

    if (ep->current_req == nullptr) {
        req = ep->queued_reqs.pop();
    }

    if (req.has_value()) {
        auto* usb_req = req->take();
        ep->current_req = usb_req;

        phys_iter_t iter;
        zx_paddr_t phys;
        usb_request_physmap(usb_req, bti_.get());
        usb_request_phys_iter_init(&iter, usb_req, PAGE_SIZE);
        usb_request_phys_iter_next(&iter, &phys);
        ep->phys = (uint32_t)phys;

        ep->send_zlp = usb_req->header.send_zlp && (usb_req->header.length % ep->max_packet_size) == 0;

        ep->req_offset = 0;
        ep->req_length = static_cast<uint32_t>(usb_req->header.length);
        StartTransfer(ep, ep->req_length);
    }
}

void Dwc2::StartTransfer(Endpoint* ep, uint32_t length) {
    auto ep_num = ep->ep_num;
    auto* mmio = get_mmio();
    bool is_in = DWC_EP_IS_IN(ep_num);

    if (length > 0) {
        if (is_in) {
            if (ep_num == DWC_EP0_IN) {
                ep0_buffer_.CacheFlush(ep->req_offset, length);
            } else {
                usb_request_cache_flush(ep->current_req, ep->req_offset, length);
            }
        } else {
            if (ep_num == DWC_EP0_OUT) {
                ep0_buffer_.CacheFlushInvalidate(ep->req_offset, length);
            } else {
                usb_request_cache_flush_invalidate(ep->current_req, ep->req_offset, length);
            }
        }
    }

    DEPDMA::Get(ep_num).FromValue(0).set_addr(ep->phys + ep->req_offset).WriteTo(mmio);

    uint32_t ep_mps = ep->max_packet_size;
    auto deptsiz = DEPTSIZ::Get(ep_num).FromValue(0);

    if (length == 0) {
        deptsiz.set_xfersize(is_in ? 0 : ep_mps);
        deptsiz.set_pktcnt(1);
    } else {
        deptsiz.set_pktcnt((length + (ep_mps - 1)) / ep_mps);
        deptsiz.set_xfersize(length);
    }
    deptsiz.set_mc(is_in ? 1 : 0);
    ep->req_xfersize = deptsiz.xfersize();
    deptsiz.WriteTo(mmio);
    hw_wmb();

    auto depctl = DEPCTL::Get(ep_num).ReadFrom(mmio);
    depctl.set_cnak(1);
    depctl.set_epena(1);
    if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT) {
        depctl.set_mps(DWC_DEP0CTL_MPS_64);
    } else {
        depctl.set_mps(ep->max_packet_size);
    }

    depctl.WriteTo(mmio);
    hw_wmb();
}

void Dwc2::FlushFifo(uint32_t fifo_num) {
    auto* mmio = get_mmio();
    auto grstctl = GRSTCTL::Get().FromValue(0);

    grstctl.set_txfflsh(1);
    grstctl.set_txfnum(fifo_num);
    grstctl.WriteTo(mmio);
    
    uint32_t count = 0;
    do {
        grstctl.ReadFrom(mmio);
        if (++count > 10000)
            break;
    } while (grstctl.txfflsh() == 1);

    zx_nanosleep(zx_deadline_after(ZX_USEC(1)));

    if (fifo_num == 0) {
        return;
    }

    grstctl.set_reg_value(0).set_rxfflsh(1).WriteTo(mmio);

    count = 0;
    do {
        grstctl.ReadFrom(mmio);
        if (++count > 10000)
            break;
    } while (grstctl.rxfflsh() == 1);

    zx_nanosleep(zx_deadline_after(ZX_USEC(1)));
}

void Dwc2::StartEndpoints() {
    for (uint8_t ep_num = 1; ep_num < fbl::count_of(endpoints_); ep_num++) {
        auto* ep = &endpoints_[ep_num];
        if (ep->enabled) {
            EnableEp(ep_num, true);

            fbl::AutoLock lock(&ep->lock);
            EpQueueNextLocked(ep);
        }
    }
}

void Dwc2::StopEndpoints() {
    auto* mmio = get_mmio();

    {
        fbl::AutoLock lock(&lock_);
        // disable all endpoints except EP0_OUT and EP0_IN
        DAINTMSK::Get().FromValue((1 << DWC_EP0_IN) | (1 << DWC_EP0_OUT)).WriteTo(mmio);
    }

    for (uint8_t ep_num = 1; ep_num < fbl::count_of(endpoints_); ep_num++) {
        EndTransfers(ep_num, ZX_ERR_IO_NOT_PRESENT);
        SetStall(ep_num, false);
    }
}

void Dwc2::EnableEp(uint8_t ep_num, bool enable) {
    auto* mmio = get_mmio();

    fbl::AutoLock lock(&lock_);

    uint32_t bit = 1 << ep_num;

    auto mask = DAINTMSK::Get().ReadFrom(mmio).reg_value();
    if (enable) {
        auto daint = DAINT::Get().ReadFrom(mmio).reg_value();
        daint |= bit;
        DAINT::Get().FromValue(daint).WriteTo(mmio);
        mask |= bit;
    } else {
        mask &= ~bit;
    }
    DAINTMSK::Get().FromValue(mask).WriteTo(mmio);
}

void Dwc2::HandleEp0Status(bool is_in) {
    ep0_state_ = (is_in ? Ep0State::STATUS_IN : Ep0State::STATUS_OUT);
    uint8_t ep_num = (is_in ? DWC_EP0_IN : DWC_EP0_OUT);
    auto* ep = &endpoints_[ep_num];
    fbl::AutoLock al(&ep->lock);
    StartTransfer(ep, 0);

    /* Prepare for more SETUP Packets */
    if (is_in) {
        StartEp0();
    }
}

void Dwc2::HandleEp0Setup() {
    auto* setup = &cur_setup_;

    auto length = letoh16(setup->wLength);
    bool is_in = ((setup->bmRequestType & USB_DIR_MASK) == USB_DIR_IN);
    size_t actual = 0;

    // no data to read, can handle setup now
    if (length == 0 || is_in) {
        // FIXME check result
        __UNUSED zx_status_t status = HandleSetup(&actual);
    }

    if (length > 0) {
        if (is_in) {
            ep0_state_ = Ep0State::DATA_IN;
            // send data in
            auto* ep = &endpoints_[DWC_EP0_IN];
            ep->req_offset = 0;
            ep->req_length = static_cast<uint32_t>(actual);
            fbl::AutoLock al(&ep->lock);
            StartTransfer(ep, (ep->req_length > 127 ? ep->max_packet_size : ep->req_length));
        } else {
            ep0_state_ = Ep0State::DATA_OUT;
            // queue a read for the data phase
            ep0_state_ = Ep0State::DATA_OUT;
            auto* ep = &endpoints_[DWC_EP0_OUT];
            ep->req_offset = 0;
            ep->req_length = length;
            fbl::AutoLock al(&ep->lock);
            StartTransfer(ep, (length > 127 ? ep->max_packet_size : length));
        }
    } else {
        // no data phase
        // status in IN direction
        HandleEp0Status(true);
    }
}

void Dwc2::HandleEp0TransferComplete() {
    switch (ep0_state_) {
    case Ep0State::IDLE: {
        StartEp0();
        break;
    }
    case Ep0State::DATA_IN: {
        auto* ep = &endpoints_[DWC_EP0_IN];
        auto transfered = ReadTransfered(ep);
        ep->req_offset += transfered;

        if (ep->req_offset == ep->req_length) {
            HandleEp0Status(false);
        } else {
            auto length = ep->req_length - ep->req_offset;
            if (length > 64) {
                length = 64;
            }
            fbl::AutoLock al(&ep->lock);
            StartTransfer(ep, length);
        }
        break;
    }
    case Ep0State::DATA_OUT: {
        auto* ep = &endpoints_[DWC_EP0_OUT];
        auto transfered = ReadTransfered(ep);
        ep->req_offset += transfered;

        if (ep->req_offset == ep->req_length) {
            if (dci_intf_.has_value()) {
                dci_intf_->Control(&cur_setup_, (uint8_t*)ep0_buffer_.virt(), ep->req_length, nullptr, 0, nullptr);
            }
            HandleEp0Status(true);
        } else {
            auto length = ep->req_length - ep->req_offset;
            if (length > 127) {
                length = 64;
            }
            fbl::AutoLock al(&ep->lock);
            StartTransfer(ep, length);
        }
        break;
    }
    case Ep0State::STATUS_OUT:
        ep0_state_ = Ep0State::IDLE;
        StartEp0();
        break;
    case Ep0State::STATUS_IN:
        ep0_state_ = Ep0State::IDLE;
        break;
    case Ep0State::STALL:
    default:
        zxlogf(LINFO, "EP0 state is %d, should not get here\n", static_cast<int>(ep0_state_));
        break;
    }
}

void Dwc2::HandleTransferComplete(uint8_t ep_num) {
    ZX_DEBUG_ASSERT(ep_num != DWC_EP0_IN && ep_num != DWC_EP0_OUT);
    auto* ep = &endpoints_[ep_num];

    ep->lock.Acquire();

    auto transfered = ReadTransfered(ep);

    ep->req_offset += transfered;

    usb_request_t* req = ep->current_req;
    if (req) {
        Request request(req, sizeof(usb_request_t));
        ep->lock.Release();
        request.Complete(ZX_OK, ep->req_offset);
        ep->lock.Acquire();

        ep->current_req = nullptr;
        EpQueueNextLocked(ep);
    }
    ep->lock.Release();
}

void Dwc2::EndTransfers(uint8_t ep_num, zx_status_t reason) {
    auto* ep = &endpoints_[ep_num];

    fbl::AutoLock lock(&ep->lock);

    if (ep->current_req) {
        // Is This Safe??
        Request request(ep->current_req, sizeof(usb_request_t));
        request.Complete(reason, 0);
        ep->current_req = NULL;
    }


    for (auto req = ep->queued_reqs.pop(); req; req = ep->queued_reqs.pop()) {
        req->Complete(reason, 0);
    }
}

zx_status_t Dwc2::SetStall(uint8_t ep_num, bool stall) {
    if (ep_num >= fbl::count_of(endpoints_)) {
        return ZX_ERR_INVALID_ARGS;
    }

    auto* ep = &endpoints_[ep_num];
    fbl::AutoLock lock(&ep->lock);

    if (!ep->enabled) {
        return ZX_ERR_BAD_STATE;
    }
/*
    if (stall && !ep->stalled) {
        dwc3_cmd_ep_set_stall(dwc, ep_num);
    } else if (!stall && ep->stalled) {
        dwc3_cmd_ep_clear_stall(dwc, ep_num);
    }
*/
    ep->stalled = stall;

    return ZX_OK;
}

zx_status_t Dwc2::InitController() {
    auto* mmio = get_mmio();

    printf("\nGSNPSID:\n");
    GSNPSID::Get().ReadFrom(mmio).Print();
    printf("\nGHWCFG1:\n");
    GHWCFG1::Get().ReadFrom(mmio).Print();
    printf("\nGHWCFG2:\n");
    GHWCFG2::Get().ReadFrom(mmio).Print();
    printf("\nGHWCFG3:\n");
    GHWCFG3::Get().ReadFrom(mmio).Print();
    printf("\nGHWCFG4:\n");
    GHWCFG4::Get().ReadFrom(mmio).Print();
    printf("\n");

    // Is this necessary?
    auto grstctl = GRSTCTL::Get();
    while (grstctl.ReadFrom(mmio).ahbidle() == 0) {
        usleep(1000);
    }

    GRSTCTL::Get().FromValue(0).set_csftrst(1).WriteTo(mmio);

    bool done = false;
    for (int i = 0; i < 1000; i++) {
        if (grstctl.ReadFrom(mmio).csftrst() == 0) {
            usleep(10 * 1000);
            done = true;
            break;
        }
        usleep(1000);
    }
    if (!done) {
        return ZX_ERR_TIMED_OUT;
    }

    usleep(10 * 1000);

    GUSBCFG::Get().ReadFrom(mmio).set_force_dev_mode(1).WriteTo(mmio);
    GAHBCFG::Get().FromValue(0).set_dmaenable(1).set_hburstlen(metadata_.dma_burst_len).WriteTo(mmio);

    // ???
    DCTL::Get().ReadFrom(mmio).set_sftdiscon(1).WriteTo(mmio);
    DCTL::Get().ReadFrom(mmio).set_sftdiscon(0).WriteTo(mmio);

    // reset phy clock
    PCGCCTL::Get().FromValue(0).WriteTo(mmio);

    // RX fifo size
    GRXFSIZ::Get().FromValue(0).set_size(metadata_.rx_fifo_size).WriteTo(mmio);

    // TX fifo size
    GNPTXFSIZ::Get().FromValue(0).set_depth(metadata_.nptx_fifo_size).set_startaddr(metadata_.rx_fifo_size).WriteTo(mmio);

    auto addr = metadata_.rx_fifo_size + metadata_.nptx_fifo_size;

    for (unsigned i = 1; i <= metadata_.in_ep_count; i++) {
        auto fifo_size = metadata_.in_ep_fifo_size[i - 1];
        if (fifo_size > 0) {
            printf("DTXFSIZ[%u]:\n", i);
            DTXFSIZ::Get(i).FromValue(0).set_startaddr(addr).set_depth(fifo_size).WriteTo(mmio).Print();
            addr += fifo_size;
        }
    }

    auto dfifo_depth = GHWCFG3::Get().ReadFrom(mmio).dfifo_depth();
    GDFIFOCFG::Get().FromValue(0).set_gdfifocfg(dfifo_depth).set_epinfobase(addr).WriteTo(mmio).Print();

    FlushFifo(0x10);

    GRSTCTL::Get().ReadFrom(mmio).set_intknqflsh(1).WriteTo(mmio);

    /* Clear all pending Device Interrupts */
    DIEPMSK::Get().FromValue(0).WriteTo(mmio);
    DOEPMSK::Get().FromValue(0).WriteTo(mmio);
    DAINT::Get().FromValue(0xffffffff).WriteTo(mmio);
    DAINTMSK::Get().FromValue(0).WriteTo(mmio);

    for (int i = 0; i < DWC_MAX_EPS; i++) {
        DEPCTL::Get(i).FromValue(0).WriteTo(mmio);
        DEPTSIZ::Get(i).FromValue(0).WriteTo(mmio);
    }

    auto gintmsk = GINTMSK::Get().FromValue(0);

    gintmsk.set_usbreset(1);
    gintmsk.set_enumdone(1);
    gintmsk.set_inepintr(1);
    gintmsk.set_outepintr(1);
    gintmsk.set_usbsuspend(1);
    gintmsk.set_modemismatch(1);

// ???
gintmsk.set_rxstsqlvl(1);
gintmsk.set_otgintr(1);

    gintmsk.set_resetdet(1);

    GOTGINT::Get().FromValue(0xFFFFFFF).WriteTo(mmio);
    GINTSTS::Get().FromValue(0xFFFFFFF).WriteTo(mmio);

    gintmsk.WriteTo(mmio);

    GAHBCFG::Get().ReadFrom(mmio).set_glblintrmsk(1).WriteTo(mmio);

    return ZX_OK;
}

zx_status_t Dwc2::Create(void* ctx, zx_device_t* parent) {
    ddk::CompositeProtocolClient composite(parent);
    if (!composite.is_valid()) {
        zxlogf(ERROR, "Dwc2::Create could not get composite protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_device_t* pdev_device;
    size_t actual;

    // Retrieve platform device protocol from our first component.
    composite.GetComponents(&pdev_device, 1, &actual);
    if (actual != 1) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    ddk::PDev pdev(pdev_device);
    if (!pdev.is_valid()) {
        zxlogf(ERROR, "Dwc2::Create: could not get platform device protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<Dwc2>(&ac, parent, std::move(pdev));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto status = dev->Init();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();
    return ZX_OK;
}

zx_status_t Dwc2::Init() {
    for (uint8_t i = 0; i < fbl::count_of(endpoints_); i++) {
        auto* ep = &endpoints_[i];
        ep->ep_num = i;
    }

    size_t actual;
    auto status = DdkGetMetadata(DEVICE_METADATA_PRIVATE, &metadata_, sizeof(metadata_), &actual);
    if (status != ZX_OK || actual != sizeof(metadata_)) {
        zxlogf(ERROR, "Dwc2::Init can't get driver metadata\n");
        return ZX_ERR_INTERNAL;
    }

    status = pdev_.MapMmio(0, &mmio_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init MapMmio failed: %d\n", status);
        return status;
    }

    status = pdev_.GetInterrupt(0, &irq_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init GetInterrupt failed: %d\n", status);
        return status;
    }

    status = pdev_.GetBti(0, &bti_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init GetBti failed: %d\n", status);
        return status;
    }

    status = ep0_buffer_.Init(bti_.get(), UINT16_MAX, IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init ep0_buffer_.Init failed: %d\n", status);
        return status;
    }

    status = ep0_buffer_.PhysMap();
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init ep0_buffer_.PhysMap failed: %d\n", status);
        return status;
    }

    if ((status = InitController()) != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init InitController failed: %d\n", status);
        return status;
    }

    status = DdkAdd("dwc2");
    if (status != ZX_OK) {
        zxlogf(ERROR, "Dwc2::Init DdkAdd failed: %d\n", status);
        return status;
    }

    int rc = thrd_create_with_name(&irq_thread_,
                                   [](void* arg) -> int {
                                       return reinterpret_cast<Dwc2*>(arg)->IrqThread();
                                   },
                                   reinterpret_cast<void*>(this),
                                   "dwc2-interrupt-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }

    return ZX_OK;
}

void Dwc2::DdkUnbind() {
    irq_.destroy();
    thrd_join(irq_thread_, nullptr);
}

void Dwc2::DdkRelease() {
    delete this;
}

int Dwc2::IrqThread() {
    auto* mmio = get_mmio();

    while (1) {
        auto wait_res = irq_.wait(nullptr);
        if (wait_res != ZX_OK) {
            zxlogf(ERROR, "dwc_usb: irq wait failed, retcode = %d\n", wait_res);
        }

        auto gintsts = GINTSTS::Get().ReadFrom(mmio);
        auto gintmsk = GINTMSK::Get().ReadFrom(mmio);
        gintsts.WriteTo(mmio);
        gintsts.set_reg_value(gintsts.reg_value() & gintmsk.reg_value());

        if (gintsts.reg_value() == 0) {
            continue;
        }

#if 0
        zxlogf(LINFO, "IRQ IRQ IRQ IRQ IRQ IRQ 0x%08X 0x%08X:", gintsts.reg_value(), gintmsk.reg_value());

        if (gintsts.modemismatch()) zxlogf(LINFO, " modemismatch");
        if (gintsts.otgintr()) zxlogf(LINFO, " otgintr gotgint: %08x\n  ", GOTGINT::Get().ReadFrom(mmio).reg_value());
        if (gintsts.sof_intr()) zxlogf(LINFO, " sof_intr");
        if (gintsts.rxstsqlvl()) zxlogf(LINFO, " rxstsqlvl");
        if (gintsts.nptxfempty()) zxlogf(LINFO, " nptxfempty");
        if (gintsts.ginnakeff()) zxlogf(LINFO, " ginnakeff");
        if (gintsts.goutnakeff()) zxlogf(LINFO, " goutnakeff");
        if (gintsts.ulpickint()) zxlogf(LINFO, " ulpickint");
        if (gintsts.i2cintr()) zxlogf(LINFO, " i2cintr");
        if (gintsts.erlysuspend()) zxlogf(LINFO, " erlysuspend");
        if (gintsts.usbsuspend()) zxlogf(LINFO, " usbsuspend");
        if (gintsts.usbreset()) zxlogf(LINFO, " usbreset");
        if (gintsts.enumdone()) zxlogf(LINFO, " enumdone");
        if (gintsts.isooutdrop()) zxlogf(LINFO, " isooutdrop");
        if (gintsts.eopframe()) zxlogf(LINFO, " eopframe");
        if (gintsts.restoredone()) zxlogf(LINFO, " restoredone");
        if (gintsts.epmismatch()) zxlogf(LINFO, " epmismatch");
        if (gintsts.inepintr()) zxlogf(LINFO, " inepintr");
        if (gintsts.outepintr()) zxlogf(LINFO, " outepintr");
        if (gintsts.incomplisoin()) zxlogf(LINFO, " incomplisoin");
        if (gintsts.incomplisoout()) zxlogf(LINFO, " incomplisoout");
        if (gintsts.fetsusp()) zxlogf(LINFO, " fetsusp");
        if (gintsts.resetdet()) zxlogf(LINFO, " resetdet");
        if (gintsts.port_intr()) zxlogf(LINFO, " port_intr");
        if (gintsts.host_channel_intr()) zxlogf(LINFO, " host_channel_intr");
        if (gintsts.ptxfempty()) zxlogf(LINFO, " ptxfempty");
        if (gintsts.lpmtranrcvd()) zxlogf(LINFO, " lpmtranrcvd");
        if (gintsts.conidstschng()) zxlogf(LINFO, " conidstschng");
        if (gintsts.disconnect()) zxlogf(LINFO, " disconnect");
        if (gintsts.sessreqintr()) zxlogf(LINFO, " sessreqintr");
        if (gintsts.wkupintr()) zxlogf(LINFO, " wkupintr");
        zxlogf(LINFO, "\n");
#endif

        if (gintsts.usbreset() || gintsts.resetdet()) {
            HandleReset();
        }
        if (gintsts.usbsuspend()) {
            HandleSuspend();
        }
        if (gintsts.enumdone()) {
            HandleEnumDone();
        }
        if (gintsts.inepintr()) {
            HandleInEpInterrupt();
        }
        if (gintsts.outepintr()) {
            HandleOutEpInterrupt();
        }
        if (gintsts.modemismatch()) {
printf("IRQ modemismatch\n");
        }
    }

    zxlogf(INFO, "dwc_usb: irq thread finished\n");
    return 0;
}

void Dwc2::UsbDciRequestQueue(usb_request_t* req, const usb_request_complete_t* cb) {
    uint8_t ep_num = DWC_ADDR_TO_INDEX(req->header.ep_address);
    if (ep_num == 0 || ep_num >= fbl::count_of(endpoints_)) {
        zxlogf(ERROR, "dwc_request_queue: bad ep address 0x%02X\n", req->header.ep_address);
        usb_request_complete(req, ZX_ERR_INVALID_ARGS, 0, cb);
        return;
    }

    auto* ep = &endpoints_[ep_num];

    // OUT transactions must have length > 0 and multiple of max packet size
    if (DWC_EP_IS_OUT(ep_num)) {
        if (req->header.length == 0 || req->header.length % ep->max_packet_size != 0) {
            zxlogf(ERROR, "dwc_ep_queue: OUT transfers must be multiple of max packet size\n");
            usb_request_complete(req, ZX_ERR_INVALID_ARGS, 0, cb);
            return;
        }
    }

    fbl::AutoLock lock(&ep->lock);

    if (!ep->enabled) {
        zxlogf(ERROR, "dwc_ep_queue ep not enabled!\n");    
        usb_request_complete(req, ZX_ERR_BAD_STATE, 0, cb);
        return;
    }

    if (!configured_) {
        zxlogf(ERROR, "dwc_ep_queue not configured!\n");
        usb_request_complete(req, ZX_ERR_BAD_STATE, 0, cb);
        return;
    }

    ep->queued_reqs.push(Request(req, *cb, sizeof(usb_request_t)));
    EpQueueNextLocked(ep);
}

zx_status_t Dwc2::UsbDciSetInterface(const usb_dci_interface_protocol_t* interface) {
    // TODO - handle interface == nullptr for tear down path?

    if (dci_intf_.has_value()) {
        zxlogf(ERROR, "%s: dci_intf_ already set\n", __func__);
        return ZX_ERR_BAD_STATE;
    }

    dci_intf_ = ddk::UsbDciInterfaceProtocolClient(interface);

    return ZX_OK;
}

 zx_status_t Dwc2::UsbDciConfigEp(const usb_endpoint_descriptor_t* ep_desc,
                                   const usb_ss_ep_comp_descriptor_t* ss_comp_desc) {
    auto* mmio = get_mmio();

    uint8_t ep_num = DWC_ADDR_TO_INDEX(ep_desc->bEndpointAddress);
    bool is_in = (ep_desc->bEndpointAddress & USB_DIR_MASK) == USB_DIR_IN;

    if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint8_t ep_type = usb_ep_type(ep_desc);
    if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
        zxlogf(ERROR, "UsbDciConfigEp: isochronous endpoints are not supported\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    auto* ep = &endpoints_[ep_num];

    fbl::AutoLock lock(&ep->lock);

    ep->max_packet_size = usb_ep_max_packet(ep_desc);
    ep->type = ep_type;
    ep->enabled = true;

    auto depctl = DEPCTL::Get(ep_num).FromValue(0);
    depctl.set_mps(ep->max_packet_size);
    depctl.set_eptype(ep_type);
    depctl.set_setd0pid(1); // correct for interrupt and bulk

    if (is_in) {
        depctl.set_txfnum(ep_num);
    } else {
        depctl.set_txfnum(0);
    }

    depctl.set_usbactep(1);
    depctl.WriteTo(mmio);

    EnableEp(ep_num, true);

    if (configured_) {
        EpQueueNextLocked(ep);
    }

    return ZX_OK;
}

zx_status_t Dwc2::UsbDciDisableEp(uint8_t ep_address) {
    auto* mmio = get_mmio();

    // convert address to index in range 0 - 31
    // low bit is IN/OUT
    // TODO validate address
    unsigned ep_num = DWC_ADDR_TO_INDEX(ep_address);
    if (ep_num < 2) {
        // index 0 and 1 are for endpoint zero
        return ZX_ERR_INVALID_ARGS;
    }

    // TODO validate ep_num?
    auto* ep = &endpoints_[ep_num];

    fbl::AutoLock lock(&ep->lock);

    DEPCTL::Get(ep_num).ReadFrom(mmio).set_usbactep(0).WriteTo(mmio);
    ep->enabled = false;

    return ZX_OK;
}

zx_status_t Dwc2::UsbDciEpSetStall(uint8_t ep_address) {
    // TODO
    return ZX_OK;
}

zx_status_t Dwc2::UsbDciEpClearStall(uint8_t ep_address) {
    // TODO
    return ZX_OK;
}

size_t Dwc2::UsbDciGetRequestSize() {
    return Request::RequestSize(sizeof(usb_request_t));
}

zx_status_t Dwc2::UsbDciCancelAll(uint8_t ep) {
    return ZX_ERR_NOT_SUPPORTED;
}

static zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Dwc2::Create;
    return ops;
}();

} // namespace dwc2

// The formatter does not play nice with these macros.
// clang-format off
ZIRCON_DRIVER_BEGIN(dwc2, dwc2::driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_DWC2),
ZIRCON_DRIVER_END(dwc2)
// clang-format on
