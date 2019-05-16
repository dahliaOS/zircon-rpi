// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hwreg/bitfields.h>
#include <zircon/hw/usb.h>

#define MAX_EPS_CHANNELS 16

#define DWC_EP_IN_SHIFT  0
#define DWC_EP_OUT_SHIFT 16

#define DWC_EP_IN_MASK   0x0000ffff
#define DWC_EP_OUT_MASK  0xffff0000

#define DWC_EP_IS_IN(ep)    ((ep) < 16)
#define DWC_EP_IS_OUT(ep)   ((ep) >= 16)
#define DWC_EP0_IN          0
#define DWC_EP0_OUT         16

#define DWC_MAX_EPS    32

// converts a USB endpoint address to 0 - 31 index
// in endpoints -> 0 - 15
// out endpoints -> 17 - 31 (16 is unused)
#define DWC_ADDR_TO_INDEX(addr) (uint8_t)(((addr) & 0xF) + (16 * !((addr) & USB_DIR_IN)))

#define DWC_REG_DATA_FIFO_START 0x1000
#define DWC_REG_DATA_FIFO(regs, ep)	((volatile uint32_t*)((uint8_t*)regs + (ep + 1) * DWC_REG_DATA_FIFO_START))

class GOTGCTL : public hwreg::RegisterBase<GOTGCTL, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, sesreqscs);
    DEF_BIT(1, sesreq);
    DEF_BIT(2, vbvalidoven);
    DEF_BIT(3, vbvalidovval);
    DEF_BIT(4, avalidoven);
    DEF_BIT(5, avalidovval);
    DEF_BIT(6, bvalidoven);
    DEF_BIT(7, bvalidovval);
    DEF_BIT(8, hstnegscs);
    DEF_BIT(9, hnpreq);
    DEF_BIT(10, hstsethnpen);
    DEF_BIT(11, devhnpen);
    DEF_BIT(16, conidsts);
    DEF_BIT(17, dbnctime);
    DEF_BIT(18, asesvld);
    DEF_BIT(19, bsesvld);
    DEF_BIT(20, otgver);
    DEF_FIELD(26, 22, hburstlen);
    DEF_BIT(27, chirpen);
    static auto Get() { return hwreg::RegisterAddr<GOTGCTL>(0x0); }
};

class GOTGINT : public hwreg::RegisterBase<GOTGINT, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(2, sesenddet);
    DEF_BIT(8, sesreqsucstschng);
    DEF_BIT(9, hstnegsucstschng);
    DEF_BIT(17, hstnegdet);
    DEF_BIT(18, adevtoutchng);
    DEF_BIT(19, debdone);
    DEF_BIT(20, mvic);
    static auto Get() { return hwreg::RegisterAddr<GOTGINT>(0x4); }
};

class GAHBCFG : public hwreg::RegisterBase<GAHBCFG, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, glblintrmsk);
    DEF_FIELD(4, 1, hburstlen);
    DEF_BIT(5, dmaenable);
    DEF_BIT(7, nptxfemplvl_txfemplvl);
    DEF_BIT(8, ptxfemplvl);
    DEF_BIT(21, remmemsupp);
    DEF_BIT(22, notialldmawrit);
    DEF_BIT(23, AHBSingle);
    static auto Get() { return hwreg::RegisterAddr<GAHBCFG>(0x8); }
};

class GUSBCFG : public hwreg::RegisterBase<GUSBCFG, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(2, 0, toutcal);
    DEF_BIT(3, phyif);
    DEF_BIT(4, ulpi_utmi_sel);
    DEF_BIT(5, fsintf);
    DEF_BIT(6, physel);
    DEF_BIT(7, ddrsel);
    DEF_BIT(8, srpcap);
    DEF_BIT(9, hnpcap);
    DEF_FIELD(13, 10, usbtrdtim);
    DEF_BIT(15, phylpwrclksel);
    DEF_BIT(16, otgutmifssel);
    DEF_BIT(17, ulpi_fsls);
    DEF_BIT(18, ulpi_auto_res);
    DEF_BIT(19, ulpi_clk_sus_m);
    DEF_BIT(20, ulpi_ext_vbus_drv);
    DEF_BIT(21, ulpi_int_vbus_indicator);
    DEF_BIT(22, term_sel_dl_pulse);
    DEF_BIT(23, indicator_complement);
    DEF_BIT(24, indicator_pass_through);
    DEF_BIT(25, ulpi_int_prot_dis);
    DEF_BIT(26, ic_usb_cap);
    DEF_BIT(27, ic_traffic_pull_remove);
    DEF_BIT(28, tx_end_delay);
    DEF_BIT(29, force_host_mode);
    DEF_BIT(30, force_dev_mode);
    static auto Get() { return hwreg::RegisterAddr<GUSBCFG>(0xC); }
};

class GRSTCTL : public hwreg::RegisterBase<GRSTCTL, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, csftrst);
    DEF_BIT(1, hsftrst);
    DEF_BIT(2, hstfrm);
    DEF_BIT(3, intknqflsh);
    DEF_BIT(4, rxfflsh);
    DEF_BIT(5, txfflsh);
    DEF_FIELD(10, 6, txfnum);
    DEF_BIT(30, dmareq);
    DEF_BIT(31, ahbidle);
    static auto Get() { return hwreg::RegisterAddr<GRSTCTL>(0x10); }
};

class GINTSTS : public hwreg::RegisterBase<GINTSTS, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, curmode);
    DEF_BIT(1, modemismatch);
    DEF_BIT(2, otgintr);
    DEF_BIT(3, sof_intr);
    DEF_BIT(4, rxstsqlvl);
    DEF_BIT(5, nptxfempty);
    DEF_BIT(6, ginnakeff);
    DEF_BIT(7, goutnakeff);
    DEF_BIT(8, ulpickint);
    DEF_BIT(9, i2cintr);
    DEF_BIT(10, erlysuspend);
    DEF_BIT(11, usbsuspend);
    DEF_BIT(12, usbreset);
    DEF_BIT(13, enumdone);
    DEF_BIT(14, isooutdrop);
    DEF_BIT(15, eopframe);
    DEF_BIT(16, restoredone);
    DEF_BIT(17, epmismatch);
    DEF_BIT(18, inepintr);
    DEF_BIT(19, outepintr);
    DEF_BIT(20, incomplisoin);
    DEF_BIT(21, incomplisoout);
    DEF_BIT(22, fetsusp);
    DEF_BIT(23, resetdet);
    DEF_BIT(24, port_intr);
    DEF_BIT(25, host_channel_intr);
    DEF_BIT(26, ptxfempty);
    DEF_BIT(27, lpmtranrcvd);
    DEF_BIT(28, conidstschng);
    DEF_BIT(29, disconnect);
    DEF_BIT(30, sessreqintr);
    DEF_BIT(31, wkupintr);
    static auto Get() { return hwreg::RegisterAddr<GINTSTS>(0x14); }
};

class GINTMSK : public hwreg::RegisterBase<GINTMSK, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, curmode);
    DEF_BIT(1, modemismatch);
    DEF_BIT(2, otgintr);
    DEF_BIT(3, sof_intr);
    DEF_BIT(4, rxstsqlvl);
    DEF_BIT(5, nptxfempty);
    DEF_BIT(6, ginnakeff);
    DEF_BIT(7, goutnakeff);
    DEF_BIT(8, ulpickint);
    DEF_BIT(9, i2cintr);
    DEF_BIT(10, erlysuspend);
    DEF_BIT(11, usbsuspend);
    DEF_BIT(12, usbreset);
    DEF_BIT(13, enumdone);
    DEF_BIT(14, isooutdrop);
    DEF_BIT(15, eopframe);
    DEF_BIT(16, restoredone);
    DEF_BIT(17, epmismatch);
    DEF_BIT(18, inepintr);
    DEF_BIT(19, outepintr);
    DEF_BIT(20, incomplisoin);
    DEF_BIT(21, incomplisoout);
    DEF_BIT(22, fetsusp);
    DEF_BIT(23, resetdet);
    DEF_BIT(24, port_intr);
    DEF_BIT(25, host_channel_intr);
    DEF_BIT(26, ptxfempty);
    DEF_BIT(27, lpmtranrcvd);
    DEF_BIT(28, conidstschng);
    DEF_BIT(29, disconnect);
    DEF_BIT(30, sessreqintr);
    DEF_BIT(31, wkupintr);
    static auto Get() { return hwreg::RegisterAddr<GINTMSK>(0x18); }
};

class GRXSTSP : public hwreg::RegisterBase<GRXSTSP, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(3, 0, epnum);
    DEF_FIELD(14, 4, bcnt);
    DEF_FIELD(16, 15, dpid);
#define DWC_DSTS_GOUT_NAK		0x1	// Global OUT NAK
#define DWC_STS_DATA_UPDT		0x2	// OUT Data Packet
#define DWC_STS_XFER_COMP		0x3	// OUT Data Transfer Complete
#define DWC_DSTS_SETUP_COMP		0x4	// Setup Phase Complete
#define DWC_DSTS_SETUP_UPDT     0x6	// SETUP Packet
    DEF_FIELD(20, 17, pktsts);
    DEF_FIELD(24, 21, fn);
    static auto Get() { return hwreg::RegisterAddr<GRXSTSP>(0x20); }
};

class GRXFSIZ : public hwreg::RegisterBase<GRXFSIZ, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(31, 0, size);
    static auto Get() { return hwreg::RegisterAddr<GRXFSIZ>(0x24); }
};

class GNPTXFSIZ : public hwreg::RegisterBase<GNPTXFSIZ, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(15, 0, startaddr);
    DEF_FIELD(31, 16, depth);
    static auto Get() { return hwreg::RegisterAddr<GNPTXFSIZ>(0x28); }
};

class GNPTXSTS : public hwreg::RegisterBase<GNPTXSTS, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(15, 0, nptxfspcavail);
    DEF_FIELD(23, 16, nptxqspcavail);
    DEF_BIT(24, nptxqtop_terminate);
    DEF_FIELD(26, 25, nptxqtop_token);
    DEF_FIELD(30, 27, nptxqtop_chnep);
    static auto Get() { return hwreg::RegisterAddr<GNPTXSTS>(0x2C); }
};

class DEPCTL : public hwreg::RegisterBase<DEPCTL, uint32_t, hwreg::EnablePrinter> {
public:
#define DWC_DEP0CTL_MPS_64	 0
#define DWC_DEP0CTL_MPS_32	 1
#define DWC_DEP0CTL_MPS_16	 2
#define DWC_DEP0CTL_MPS_8	 3
    DEF_FIELD(10, 0, mps);
    DEF_FIELD(14, 11, nextep);
    DEF_BIT(15, usbactep);
    DEF_BIT(16, dpid);
    DEF_BIT(17, naksts);
    DEF_FIELD(19, 18, eptype);
    DEF_BIT(20, snp);
    DEF_BIT(21, stall);
    DEF_FIELD(25, 22, txfnum);
    DEF_BIT(26, cnak);
    DEF_BIT(27, snak);
    DEF_BIT(28, setd0pid);
    DEF_BIT(29, setd1pid);
    DEF_BIT(30, epdis);
    DEF_BIT(31, epena);
    static auto Get(unsigned i) { return hwreg::RegisterAddr<DEPCTL>(0x900 + 0x20 * i); }
};

class DIEPINT : public hwreg::RegisterBase<DIEPINT, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, xfercompl);
    DEF_BIT(1, epdisabled);
    DEF_BIT(2, ahberr);
    DEF_BIT(3, timeout);
    DEF_BIT(4, intktxfemp);
    DEF_BIT(5, intknepmis);
    DEF_BIT(6, inepnakeff);
    DEF_BIT(8, txfifoundrn);
    DEF_BIT(9, bna);
    DEF_BIT(13, nak);
    static auto Get(unsigned i) { return hwreg::RegisterAddr<DIEPINT>(0x908 + 0x20 * i); }
};

class DOEPINT : public hwreg::RegisterBase<DOEPINT, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, xfercompl);
    DEF_BIT(1, epdisabled);
    DEF_BIT(2, ahberr);
    DEF_BIT(3, setup);
    DEF_BIT(4, outtknepdis);
    DEF_BIT(5, stsphsercvd);
    DEF_BIT(6, back2backsetup);
    DEF_BIT(8, outpkterr);
    DEF_BIT(9, bna);
    DEF_BIT(11, pktdrpsts);
    DEF_BIT(12, babble);
    DEF_BIT(13, nak);
    DEF_BIT(14, nyet);
    DEF_BIT(15, sr);
    static auto Get(unsigned i) { return hwreg::RegisterAddr<DOEPINT>(0xB08 + 0x20 * i); }
};

class DEPTSIZ : public hwreg::RegisterBase<DEPTSIZ, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(18, 0, xfersize);
    DEF_FIELD(28, 19, pktcnt);
    DEF_FIELD(30, 29, mc);
    static auto Get(unsigned i) { return hwreg::RegisterAddr<DEPTSIZ>(0x910 + 0x20 * i); }
};

class DEPTSIZ0 : public hwreg::RegisterBase<DEPTSIZ0, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(6, 0, xfersize);
    DEF_FIELD(20, 19, pktcnt);
    DEF_FIELD(30, 29, supcnt);
    static auto Get(unsigned i) { return hwreg::RegisterAddr<DEPTSIZ0>(0x910 + 0x20 * i); }
};

class DCFG : public hwreg::RegisterBase<DCFG, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(1, 0, devspd);
    DEF_BIT(2, nzstsouthshk);
    DEF_BIT(3, ena32khzs);
    DEF_FIELD(10, 4, devaddr);
    DEF_FIELD(12, 11, perfrint);
    DEF_BIT(13, endevoutnak);
    DEF_FIELD(22, 18, epmscnt);
    DEF_BIT(23, descdma);
    DEF_FIELD(25, 24, perschintvl);
    DEF_FIELD(31, 26, resvalid);
    static auto Get() { return hwreg::RegisterAddr<DCFG>(0x800); }
};

class DCTL : public hwreg::RegisterBase<DCTL, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, rmtwkupsig);
    DEF_BIT(1, sftdiscon);
    DEF_BIT(2, gnpinnaksts);
    DEF_BIT(3, goutnaksts);
    DEF_FIELD(6, 4, tstctl);
    DEF_BIT(7, sgnpinnak);
    DEF_BIT(8, cgnpinnak);
    DEF_BIT(9, sgoutnak);
    DEF_BIT(10, cgoutnak);
    DEF_BIT(11, pwronprgdone);
    DEF_FIELD(14, 13, gmc);
    DEF_BIT(15, ifrmnum);
    DEF_BIT(16, nakonbble);
    DEF_BIT(17, encontonbna);
    DEF_BIT(18, besl_reject);
    static auto Get() { return hwreg::RegisterAddr<DCTL>(0x804); }
};

class DSTS : public hwreg::RegisterBase<DSTS, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, suspsts);
    DEF_FIELD(2, 1, enumspd);
    DEF_BIT(3, errticerr);
    DEF_FIELD(21, 8, soffn);
    static auto Get() { return hwreg::RegisterAddr<DSTS>(0x808); }
};

class DIEPMSK : public hwreg::RegisterBase<DIEPMSK, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, xfercompl);
    DEF_BIT(1, epdisabled);
    DEF_BIT(2, ahberr);
    DEF_BIT(3, timeout);
    DEF_BIT(4, intktxfemp);
    DEF_BIT(5, intknepmis);
    DEF_BIT(6, inepnakeff);
    DEF_BIT(8, txfifoundrn);
    DEF_BIT(9, bna);
    DEF_BIT(13, nak);
    static auto Get() { return hwreg::RegisterAddr<DIEPMSK>(0x810); }
};

class DOEPMSK : public hwreg::RegisterBase<DOEPMSK, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_BIT(0, xfercompl);
    DEF_BIT(1, epdisabled);
    DEF_BIT(2, ahberr);
    DEF_BIT(3, setup);
    DEF_BIT(4, outtknepdis);
    DEF_BIT(5, stsphsercvd);
    DEF_BIT(6, back2backsetup);
    DEF_BIT(8, outpkterr);
    DEF_BIT(9, bna);
    DEF_BIT(11, pktdrpsts);
    DEF_BIT(12, babble);
    DEF_BIT(13, nak);
    DEF_BIT(14, nyet);
    DEF_BIT(15, sr);
    static auto Get() { return hwreg::RegisterAddr<DOEPMSK>(0x814); }
};

class DAINT : public hwreg::RegisterBase<DAINT, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(31, 0, enable);
    static auto Get() { return hwreg::RegisterAddr<DAINT>(0x818); }
};

class DAINTMSK : public hwreg::RegisterBase<DAINTMSK, uint32_t, hwreg::EnablePrinter> {
public:
    DEF_FIELD(31, 0, mask);
    static auto Get() { return hwreg::RegisterAddr<DAINTMSK>(0x81C); }
};

class PCGCCTL : public hwreg::RegisterBase<PCGCCTL, uint32_t, hwreg::EnablePrinter> {
public:
    static auto Get() { return hwreg::RegisterAddr<PCGCCTL>(0xE00); }
};
