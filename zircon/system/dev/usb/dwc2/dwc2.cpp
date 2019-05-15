// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dwc2.h"

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform-device-lib.h>
#include <ddktl/pdev.h>
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
        set_xfercompl(1).
        set_ahberr(1).
        set_epdisabled(1).
        WriteTo(mmio);
    DIEPMSK::Get().FromValue(0).
        set_xfercompl(1).
        set_timeout(1).
        set_ahberr(1).
        set_epdisabled(1).
        WriteTo(mmio);

    /* Reset Device Address */
    DCFG::Get().ReadFrom(mmio).set_devaddr(0).WriteTo(mmio);

    StartEp0();

    // TODO how to detect disconnect?
    dci_intf_->SetConnected(true);
}

void Dwc2::HandleSuspend() {
    zxlogf(INFO, "Dwc2::HandleSuspend\n");
}

void Dwc2::HandleEnumDone() {
    auto* mmio = get_mmio();

    zxlogf(INFO, "dwc_handle_enumdone_irq\n");

/*
    if (dwc->astro_usb.ops) {
        astro_usb_do_usb_tuning(&dwc->astro_usb, false, false);
    }
*/
    ep0_state_ = Ep0State::IDLE;

    endpoints_[0].max_packet_size = 64;

    DEPCTL::Get(0).ReadFrom(mmio).set_mps(DWC_DEP0CTL_MPS_64).WriteTo(mmio);
    DEPCTL::Get(16).ReadFrom(mmio).set_epena(1).WriteTo(mmio);

#if 0 // astro future use
    depctl.d32 = dwc_read_reg32(DWC_REG_IN_EP_REG(1));
    if (!depctl.b.usbactep) {
        depctl.b.mps = BULK_EP_MPS;
        depctl.b.eptype = 2;//BULK_STYLE
        depctl.b.setd0pid = 1;
        depctl.b.txfnum = 0;   //Non-Periodic TxFIFO
        depctl.b.usbactep = 1;
        dwc_write_reg32(DWC_REG_IN_EP_REG(1), depctl.d32);
    }

    depctl.d32 = dwc_read_reg32(DWC_REG_OUT_EP_REG(2));
    if (!depctl.b.usbactep) {
        depctl.b.mps = BULK_EP_MPS;
        depctl.b.eptype = 2;//BULK_STYLE
        depctl.b.setd0pid = 1;
        depctl.b.txfnum = 0;   //Non-Periodic TxFIFO
        depctl.b.usbactep = 1;
        dwc_write_reg32(DWC_REG_OUT_EP_REG(2), depctl.d32);
    }
#endif

    DCTL::Get().ReadFrom(mmio).set_cgnpinnak(1).WriteTo(mmio);

    /* high speed */
#if 0 // astro
    GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(9).WriteTo(mmio);
    regs->gusbcfg.usbtrdtim = 9;
#else
    GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(5).WriteTo(mmio);
#endif

    dci_intf_->SetSpeed(USB_SPEED_HIGH);
}

void Dwc2::HandleRxStatusQueueLevel() {
    auto* mmio = get_mmio();
    auto* regs = mmio->get();

    GINTMSK::Get().ReadFrom(mmio).set_rxstsqlvl(0).WriteTo(mmio);

    /* Get the Status from the top of the FIFO */
    auto grxstsp = GRXSTSP::Get().ReadFrom(mmio);
    auto ep_num = grxstsp.epnum();
    auto* ep = &endpoints_[ep_num + DWC_EP_OUT_SHIFT];

    switch (grxstsp.pktsts()) {
    case DWC_STS_DATA_UPDT: {
        uint32_t fifo_count = grxstsp.bcnt();
        if (fifo_count > ep->req_length - ep->req_offset) {
            fifo_count = ep->req_length - ep->req_offset;
        }
        if (fifo_count > 0) {
            ReadPacket(ep->req_buffer + ep->req_offset, fifo_count);
            ep->req_offset += fifo_count;
            if (ep->req_offset == ep->req_length) {
                if (ep->ep_num == DWC_EP0_OUT) {
                    // FIXME check status
                    dci_intf_->Control(&cur_setup_, ep0_buffer_, ep->req_length, nullptr, 0, nullptr);
                    CompleteEp0();
                }
            }
        }
        break;
    }

    case DWC_DSTS_SETUP_UPDT: {
    volatile uint32_t* fifo = (uint32_t *)((uint8_t *)regs + 0x1000);
    uint32_t* dest = (uint32_t*)&cur_setup_;
    dest[0] = *fifo;
    dest[1] = *fifo;
zxlogf(LINFO, "SETUP bmRequestType: 0x%02x bRequest: %u wValue: %u wIndex: %u wLength: %u\n",
        cur_setup_.bmRequestType, cur_setup_.bRequest, cur_setup_.wValue, cur_setup_.wIndex,
        cur_setup_.wLength);
        got_setup_ = true;
        break;
    }

    case DWC_DSTS_GOUT_NAK:
    case DWC_STS_XFER_COMP:
    case DWC_DSTS_SETUP_COMP:
    default:
        break;
    }

    GINTMSK::Get().ReadFrom(mmio).set_rxstsqlvl(1).WriteTo(mmio);
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
                if (0 == ep_num) {
                    HandleEp0();
                } else {
                    EpComplete(ep_num);
                    if (diepint.nak()) {
    printf("diepint.nak ep_num %u\n", ep_num);
                        DIEPINT::Get(ep_num).ReadFrom(mmio).set_nak(1).WriteTo(mmio);
                    }
                }
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
    //                handle_in_ep_timeout_intr(ep_num);
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

    uint8_t ep_num = 0;

    /* Read in the device interrupt bits */
    auto ep_bits = DAINT::Get().ReadFrom(mmio).reg_value();
    auto ep_mask = DAINTMSK::Get().ReadFrom(mmio).reg_value();
    ep_bits &= ep_mask;
    ep_bits &= DWC_EP_OUT_MASK;
    ep_bits >>= DWC_EP_OUT_SHIFT;

zxlogf(LINFO, "Dwc2::HandleOutEpInterrupt ep_bits %x\n", ep_bits);

    /* Clear the interrupt */
    DAINT::Get().FromValue(DWC_EP_OUT_MASK).WriteTo(mmio);

    while (ep_bits) {
        if (ep_bits & 1) {
            auto doepint = DOEPINT::Get(ep_num).ReadFrom(mmio);
            doepint.set_reg_value(doepint.reg_value() & DOEPMSK::Get().ReadFrom(mmio).reg_value());
if (ep_num > 0) zxlogf(LINFO, "dwc_handle_outepintr_irq doepint.val %08x\n", doepint.reg_value());

            /* Transfer complete */
            if (doepint.xfercompl()) {
if (ep_num > 0) zxlogf(LINFO, "dwc_handle_outepintr_irq xfercompl\n");
                /* Clear the bit in DOEPINTn for this interrupt */
                DOEPINT::Get(ep_num).FromValue(0).set_xfercompl(1).WriteTo(mmio);

                if (ep_num == 0) {
                    if (doepint.setup()) { // astro
                        DOEPINT::Get(ep_num).ReadFrom(mmio).set_setup(1).WriteTo(mmio);
                    }
                    HandleEp0();
                } else {
                    EpComplete(ep_num);
                }
            }
            /* Endpoint disable  */
            if (doepint.epdisabled()) {
zxlogf(LINFO, "dwc_handle_outepintr_irq epdisabled\n");
                /* Clear the bit in DOEPINTn for this interrupt */
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_epdisabled(1).WriteTo(mmio);
            }
            /* AHB Error */
            if (doepint.ahberr()) {
zxlogf(LINFO, "dwc_handle_outepintr_irq ahberr\n");
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_ahberr(1).WriteTo(mmio);
            }
            /* Setup Phase Done (contr0l EPs) */
            if (doepint.setup()) {
/*                if (1) { // astro
printf("HandleEp0 %d\n", __LINE__);
                    HandleEp0();
                }
*/
                DOEPINT::Get(ep_num).ReadFrom(mmio).set_setup(1).WriteTo(mmio);
            }
        }
        ep_num++;
        ep_bits >>= 1;
    }
}

void Dwc2::HandleTxFifoEmpty() {
    bool need_more = false;
    auto* mmio = get_mmio();

    for (uint8_t ep_num = DWC_EP0_IN; ep_num < MAX_EPS_CHANNELS; ep_num++) {
        if (DEPCTL::Get(ep_num).ReadFrom(mmio).epena() == 0) {
			continue;
        }

        auto* ep = &endpoints_[ep_num];

//		flush_cpu_cache();

		/* While there is space in the queue and space in the FIFO and
		 * More data to tranfer, Write packets to the Tx FIFO */
//		txstatus.d32 = dwc_read_reg32(DWC_REG_GNPTXSTS);
		while  (/*txstatus.b.nptxqspcavail > 0 &&
			txstatus.b.nptxfspcavail > dwords &&*/
			ep->req_offset < ep->req_length) {
				uint32_t retry = 1000000;

				uint32_t len = ep->req_length - ep->req_offset;
				if (len > ep->max_packet_size)
					len = ep->max_packet_size;

				uint32_t dwords = (len + 3) >> 2;

				while (retry--) {
				    auto txstatus = GNPTXSTS::Get().ReadFrom(mmio);
					if (txstatus.nptxqspcavail() > 0 && txstatus.nptxfspcavail() > dwords)
						break;
//					else
//						flush_cpu_cache();
				}
				if (0 == retry) {
					printf("TxFIFO FULL: Can't trans data to HOST !\n");
					break;
				}
				/* Write the FIFO */
                if (WritePacket(ep_num)) {
                    need_more = true;
                }

//				flush_cpu_cache();
			}

/*
        if (DAINTMSK::Get().ReadFrom(mmio).mask() & (1 << ep_num)) {
            if (WritePacket(ep_num)) {
                need_more = true;
            }
        }
*/
    }
    if (!need_more) {
        GINTMSK::Get().ReadFrom(mmio).set_nptxfempty(0).WriteTo(mmio);
    }
}

zx_status_t Dwc2::HandleSetup(size_t* out_actual) {
    zx_status_t status;

    auto* setup = &cur_setup_;
    auto* buffer = ep0_buffer_;
    auto length = sizeof(ep0_buffer_);

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
            status = dci_intf_->Control(setup, nullptr, 0, buffer, length, out_actual);
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
        status = dci_intf_->Control(setup, nullptr, 0, buffer, length, out_actual);
        if (status == ZX_OK) {
            StartEndpoints();
        } else {
            configured_ = false;
        }
        return status;
    }

    bool is_in = ((setup->bmRequestType & USB_DIR_MASK) == USB_DIR_IN);
    if (is_in) {
        status = dci_intf_->Control(setup, nullptr, 0, buffer, length, out_actual);
    } else if (length == 0) {
        status = dci_intf_->Control(setup, buffer, length, nullptr, 0, out_actual);
    } else {
        status = -1;
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

void Dwc2::StartEp0() {
    auto* mmio = get_mmio();

    auto doeptsize0 = DEPTSIZ0::Get(DWC_EP0_OUT).FromValue(0);

    doeptsize0.set_supcnt(3);
    doeptsize0.set_pktcnt(1);
    doeptsize0.set_xfersize(8 * 3);
    doeptsize0.WriteTo(mmio);

    DEPCTL::Get(DWC_EP0_OUT).FromValue(0).set_epena(1).WriteTo(mmio);
}

void Dwc2::ReadPacket(void* buffer, uint32_t length) {
    auto* regs = get_mmio()->get();
    uint32_t count = (length + 3) >> 2;
    uint32_t* dest = (uint32_t*)buffer;
    // FIXME use register thingy
    volatile uint32_t* fifo = (uint32_t *)((uint8_t *)regs + 0x1000);

    for (uint32_t i = 0; i < count; i++) {
        *dest++ = *fifo;
    }
}

bool Dwc2::WritePacket(uint8_t ep_num) {
    auto* ep = &endpoints_[ep_num];
    auto* mmio = get_mmio();

    uint32_t len = ep->req_length - ep->req_offset;
    if (len > ep->max_packet_size)
        len = ep->max_packet_size;

    uint32_t dwords = (len + 3) >> 2;
    uint8_t *req_buffer = &ep->req_buffer[ep->req_offset];

    auto txstatus = GNPTXSTS::Get().ReadFrom(mmio);

    while  (ep->req_offset < ep->req_length && txstatus.nptxqspcavail() > 0 && txstatus.nptxfspcavail() > dwords) {
        volatile uint32_t* fifo = DWC_REG_DATA_FIFO(mmio_->get(), ep_num);
    
        for (uint32_t i = 0; i < dwords; i++) {
            uint32_t temp = *((uint32_t*)req_buffer);
//zxlogf(LINFO, "write %08x\n", temp);
            *fifo = temp;
            req_buffer += 4;
        }
    
        ep->req_offset += len;

        len = ep->req_length - ep->req_offset;
        if (len > ep->max_packet_size)
            len = ep->max_packet_size;

        dwords = (len + 3) >> 2;
        txstatus.ReadFrom(mmio);
    }

    if (ep->req_offset < ep->req_length) {
        // enable txempty
        GINTMSK::Get().ReadFrom(mmio).set_nptxfempty(1).WriteTo(mmio);
        return true;
    } else {
        return false;
    }
}

void Dwc2::EpQueueNextLocked(Endpoint* ep) {
    std::optional<Request> req;

#if SINGLE_EP_IN_QUEUE
    bool is_in = DWC_EP_IS_IN(ep->ep_num);
    if (is_in) {
        if (current_in_req_ == nullptr) {
            req = queued_in_reqs_.pop();
        }
    } else
#endif
    {
        if (ep->current_req == nullptr) {
            req = ep->queued_reqs.pop();
        }
    }

    if (req.has_value()) {
        auto* usb_req = req->take();
        ep->current_req = usb_req;
        
        usb_request_mmap(usb_req, (void **)&ep->req_buffer);
        ep->send_zlp = usb_req->header.send_zlp && (usb_req->header.length % ep->max_packet_size) == 0;

        StartTransfer(ep->ep_num, static_cast<uint32_t>(usb_req->header.length));
    }
}

void Dwc2::StartTransfer(uint8_t ep_num, uint32_t length) {
    auto* ep = &endpoints_[ep_num];
    auto* mmio = get_mmio();
    bool is_in = DWC_EP_IS_IN(ep_num);

    uint32_t ep_mps = ep->max_packet_size;

    ep->req_offset = 0;
    ep->req_length = length;

    auto deptsiz = DEPTSIZ::Get(ep_num).ReadFrom(mmio);

    /* Zero Length Packet? */
    if (length == 0) {
        deptsiz.set_xfersize(is_in ? 0 : ep_mps);
        deptsiz.set_pktcnt(1);
    } else {
        deptsiz.set_pktcnt((length + (ep_mps - 1)) / ep_mps);
        if (is_in && length < ep_mps) {
            deptsiz.set_xfersize(length);
        }
        else {
            deptsiz.set_xfersize(length - ep->req_offset);
        }
    }
    deptsiz.WriteTo(mmio);

    if (is_in) {
        GINTSTS::Get().FromValue(0).set_nptxfempty(1).WriteTo(mmio);
        GINTMSK::Get().ReadFrom(mmio).set_nptxfempty(1).WriteTo(mmio);
    }

    /* EP enable */
    auto depctl = DEPCTL::Get(ep_num).ReadFrom(mmio);
    depctl.set_cnak(1);
    depctl.set_epena(1);
    depctl.WriteTo(mmio);

/*???
    if (is_in) {
        WritePacket(ep_num);
    }
*/
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

#if SINGLE_EP_IN_QUEUE
    // Do something here
#endif

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
    ep0_state_ = Ep0State::STATUS;

    StartTransfer((is_in ? DWC_EP0_IN : DWC_EP0_OUT), 0);

    /* Prepare for more SETUP Packets */
    StartEp0();
}

void Dwc2::CompleteEp0() { // ep0_complete_request
    auto* ep = &endpoints_[0];
    auto* mmio = get_mmio();

    if (ep0_state_ == Ep0State::STATUS) {
        ep->req_offset = 0;
        ep->req_length = 0;
// this interferes with zero length OUT
//    } else if ( ep->req_length == 0) {
//zxlogf(LINFO, "CompleteEp0 ep->req_length == 0\n");
//      dwc_otg_ep_start_transfer(ep);
    } else if ((cur_setup_.bmRequestType & USB_DIR_MASK) == USB_DIR_IN) {
        if (DEPTSIZ0::Get(DWC_EP0_IN).ReadFrom(mmio).xfersize() == 0) {
			HandleEp0Status(false);
		}
    } else {
        HandleEp0Status(true);
    }

#if 0
    deptsiz0_data_t deptsiz;
    dwc_ep_t* ep = &pcd->dwc_eps[0].dwc_ep;
    int ret = 0;

    if (EP0_STATUS == pcd->ep0state) {
        ep->start_xfer_buff = 0;
        ep->xfer_buff = 0;
        ep->xfer_len = 0;
        ep->num = 0;
        ret = 1;
    } else if (0 == ep->xfer_len) {
        ep->xfer_len = 0;
        ep->xfer_count = 0;
        ep->sent_zlp = 1;
        ep->num = 0;
        dwc_otg_ep_start_transfer(ep);
        ret = 1;
    } else if (ep->is_in) {
        deptsiz.d32 = dwc_read_reg32(DWC_REG_IN_EP_TSIZE(0));
        if (0 == deptsiz.b.xfersize) {
            /* Is a Zero Len Packet needed? */
            HandleEp0Status(false);
        }
    } else {
        /* ep0-OUT */
        HandleEp0Status(true);
    }

#endif
}

void Dwc2::HandleEp0Setup() {
    auto* setup = &cur_setup_;

    if (!got_setup_) {
        return;
    }
    got_setup_ = false;


    if (setup->bmRequestType & USB_DIR_IN) {
        ep0_state_ = Ep0State::DATA_IN;
    } else {
        ep0_state_ = Ep0State::DATA_OUT;
    }

    if (setup->wLength > 0 && ep0_state_ == Ep0State::DATA_OUT) {
        // queue a read for the data phase
        ep0_state_ = Ep0State::DATA_OUT;
        StartTransfer(DWC_EP0_OUT, setup->wLength);
    } else {
        size_t actual = 0;
        // FIXME check result
        __UNUSED zx_status_t status = HandleSetup(&actual);
//            if (status != ZX_OK) {
//                dwc3_cmd_ep_set_stall(dwc, EP0_OUT);
//                dwc3_queue_setup_locked(dwc);
//                break;
//            }

        if (ep0_state_ == Ep0State::DATA_IN && setup->wLength > 0) {
            StartTransfer(DWC_EP0_IN, static_cast<uint32_t>(actual));
        } else {
            CompleteEp0();
        }
    }
}

void Dwc2::HandleEp0() {
    switch (ep0_state_) {
    case Ep0State::IDLE: {
//        req_flag->request_config = 0;
        HandleEp0Setup();
        break;
    }
    case Ep0State::DATA_IN:
//        if (ep0->xfer_count < ep0->total_len)
//            zxlogf(LINFO, "FIX ME!! dwc_otg_ep0_continue_transfer!\n");
//        else
            CompleteEp0();
        break;
    case Ep0State::DATA_OUT:
        CompleteEp0();
        break;
    case Ep0State::STATUS:
        CompleteEp0();
        /* OUT for next SETUP */
        ep0_state_ = Ep0State::IDLE;
//        ep0->stopped = 1;
//        ep0->is_in = 0;
        break;

    case Ep0State::STALL:
    default:
        zxlogf(LINFO, "EP0 state is %d, should not get here\n", static_cast<int>(ep0_state_));
        break;
    }
}

void Dwc2::EpComplete(uint8_t ep_num) {
    if (ep_num != 0) {
        auto* ep = &endpoints_[ep_num];
        usb_request_t* req = ep->current_req;

        if (req) {
#if SINGLE_EP_IN_QUEUE
        if (DWC_EP_IS_IN(ep->ep_num)) {
            ZX_DEBUG_ASSERT(current_in_req_ == ep->current_req);
            current_in_req_ = nullptr;
        }
#endif

            ep->current_req = NULL;
            // Is This Safe??
            Request request(req, sizeof(usb_request_t));
            request.Complete(ZX_OK, ep->req_offset);
        }

        ep->req_buffer = NULL;
        ep->req_offset = 0;
        ep->req_length = 0;
    }

/*
    u32 epnum = ep_num;
    if (ep_num) {
        if (!is_in)
            epnum = ep_num + 1;
    }
*/


/*
    if (is_in) {
        pcd->dwc_eps[epnum].req->actual = ep->xfer_len;
        deptsiz.d32 = dwc_read_reg32(DWC_REG_IN_EP_TSIZE(ep_num));
        if (deptsiz.b.xfersize == 0 && deptsiz.b.pktcnt == 0 &&
                    ep->xfer_count == ep->xfer_len) {
            ep->start_xfer_buff = 0;
            ep->xfer_buff = 0;
            ep->xfer_len = 0;
        }
        pcd->dwc_eps[epnum].req->status = 0;
    } else {
        deptsiz.d32 = dwc_read_reg32(DWC_REG_OUT_EP_TSIZE(ep_num));
        pcd->dwc_eps[epnum].req->actual = ep->xfer_count;
        ep->start_xfer_buff = 0;
        ep->xfer_buff = 0;
        ep->xfer_len = 0;
        pcd->dwc_eps[epnum].req->status = 0;
    }
*/
}

void Dwc2::EndTransfers(uint8_t ep_num, zx_status_t reason) {
    auto* ep = &endpoints_[ep_num];

    fbl::AutoLock lock(&ep->lock);

    if (ep->current_req) {
//        dwc_cmd_ep_end_transfer(dwc, ep_num);

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
    GAHBCFG::Get().FromValue(0).set_dmaenable(0).WriteTo(mmio);

#if 0 // astro
    GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(9).WriteTo(mmio);
#else
//    GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(5).WriteTo(mmio);
#endif

    // ???
    DCTL::Get().ReadFrom(mmio).set_sftdiscon(1).WriteTo(mmio);
    DCTL::Get().ReadFrom(mmio).set_sftdiscon(0).WriteTo(mmio);

    // reset phy clock
    PCGCCTL::Get().FromValue(0).WriteTo(mmio);

    // RX fifo size
    GRXFSIZ::Get().FromValue(0).set_size(256).WriteTo(mmio);

    // TX fifo size
    GNPTXFSIZ::Get().FromValue(0).set_depth(512).set_startaddr(256).WriteTo(mmio);

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

    gintmsk.set_rxstsqlvl(1);
    gintmsk.set_usbreset(1);
    gintmsk.set_enumdone(1);
    gintmsk.set_inepintr(1);
    gintmsk.set_outepintr(1);
//    gintmsk.set_sof_intr(1);
    gintmsk.set_usbsuspend(1);

    gintmsk.set_resetdet(1);

//    gintmsk.set_erlysuspend(1);


//    gintmsk.set_ginnakeff(1);
//    gintmsk.set_goutnakeff(1);


/*
    gintmsk.set_modemismatch(1);
    gintmsk.set_conidstschng(1);
    gintmsk.set_wkupintr(1);
    gintmsk.set_disconnect(0);
*/
//    gintmsk.set_sessreqintr(1);
//    gintmsk.set_otgintr(1);

//printf("ghwcfg1 %08x ghwcfg2 %08x ghwcfg3 %08x\n", regs->ghwcfg1, regs->ghwcfg2, regs->ghwcfg3);

    GOTGINT::Get().FromValue(0xFFFFFFF).WriteTo(mmio);
    GINTSTS::Get().FromValue(0xFFFFFFF).WriteTo(mmio);

    gintmsk.WriteTo(mmio);

    GAHBCFG::Get().ReadFrom(mmio).set_glblintrmsk(1).WriteTo(mmio);

    return ZX_OK;
}

zx_status_t Dwc2::Create(void* ctx, zx_device_t* parent) {
    pdev_protocol_t pdev;

    auto status = device_get_protocol(parent, ZX_PROTOCOL_PDEV, &pdev);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    auto mt_usb = fbl::make_unique_checked<Dwc2>(&ac, parent, &pdev);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    status = mt_usb->Init();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = mt_usb.release();
    return ZX_OK;
}

zx_status_t Dwc2::Init() {
    for (uint8_t i = 0; i < fbl::count_of(endpoints_); i++) {
        auto* ep = &endpoints_[i];
        ep->ep_num = i;
    }
    endpoints_[DWC_EP0_IN].req_buffer = ep0_buffer_;
    endpoints_[DWC_EP0_OUT].req_buffer = ep0_buffer_;

    auto status = pdev_.MapMmio(0, &mmio_);
    if (status != ZX_OK) {
        return status;
    }

    status = pdev_.GetInterrupt(0, &irq_);
    if (status != ZX_OK) {
        return status;
    }

    if ((status = InitController()) != ZX_OK) {
        zxlogf(ERROR, "usb_dwc: failed to init controller.\n");
        return status;
    }

    status = DdkAdd("dwc2");
    if (status != ZX_OK) {
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

/*
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
*/

        if (gintsts.rxstsqlvl()) {
            HandleRxStatusQueueLevel();
        }
        if (gintsts.nptxfempty()) {
            HandleTxFifoEmpty();
        }
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
    }

    zxlogf(INFO, "dwc_usb: irq thread finished\n");
    return 0;
}

void Dwc2::UsbDciRequestQueue(usb_request_t* req, const usb_request_complete_t* cb) {
    zxlogf(INFO, "XXXXXXX Dwc2::UsbDciRequestQueue ep: 0x%02x length %zu\n", req->header.ep_address, req->header.length);
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

    // convert address to index in range 0 - 31
    // low bit is IN/OUT
    uint8_t ep_num = DWC_ADDR_TO_INDEX(ep_desc->bEndpointAddress);
zxlogf(LINFO, "dwc_ep_config address %02x ep_num %d\n", ep_desc->bEndpointAddress, ep_num);
    if (ep_num == 0) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint8_t ep_type = usb_ep_type(ep_desc);
    if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
        zxlogf(ERROR, "dwc_ep_config: isochronous endpoints are not supported\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    auto* ep = &endpoints_[ep_num];

    fbl::AutoLock lock(&ep->lock);

    ep->max_packet_size = usb_ep_max_packet(ep_desc);
    ep->type = ep_type;
    ep->interval = ep_desc->bInterval;
    // TODO(voydanoff) USB3 support

    ep->enabled = true;

    auto depctl = DEPCTL::Get(ep_num).ReadFrom(mmio);

    depctl.set_mps(usb_ep_max_packet(ep_desc));
    depctl.set_eptype(usb_ep_type(ep_desc));
    depctl.set_setd0pid(1);
    depctl.set_txfnum(0);   //Non-Periodic TxFIFO
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
ZIRCON_DRIVER_BEGIN(dwc2, dwc2::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_DWC2),
ZIRCON_DRIVER_END(dwc2)
// clang-format on
