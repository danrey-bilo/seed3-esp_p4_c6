"""Build-time patches to exactly espressif/tinyusb 0.19.0~3 DWC2.

Keep upstream MIT copyright in generated C. Scoped fixes:
ISO IN DMA transaction count, rewind DMA on incomplete, bounded ISO abort,
and ISO OUT token-phase synchronisation / incomplete recovery.
Unknown upstream contents fail closed instead of silently changing another driver.
"""
import hashlib
import pathlib
import sys

src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
expected = "b02378ee5b90f4eb00fee6289ab7531432e7e41b91e618c0319890b03b9fb25f"
# read_text normalizes CRLF to LF. The pinned upstream digest is for LF.
# Accept the same source after a Windows checkout, but never content changes.
if hashlib.sha256(src.encode("utf-8")).hexdigest() != expected:
    raise SystemExit("Unexpected dcd_dwc2.c SHA256: review the port patch before upgrading")

def replace_once(old, new):
    global src
    if src.count(old) != 1:
        raise SystemExit(f"DWC2 patch anchor count != 1: {old[:80]}")
    src = src.replace(old, new)

begin = src.index("static void edpt_disable(")
end = src.index("// Since this function returns void", begin)
src = src[:begin] + r'''
#include "esp_rom_sys.h"
static volatile bool p4_port_fault;
bool p4_dwc2_faulted(void) { return p4_port_fault; }
uint32_t p4_dwc2_frame_number(void) {
  return (DWC2_REG(1)->dsts & DSTS_FNSOF) >> DSTS_FNSOF_Pos;
}
// All waits are bounded, including calls made from ISO incomplete ISR.
static bool p4_wait(volatile uint32_t *reg, uint32_t mask, bool set) {
  for (unsigned i = 0; i < 250; ++i) {
    if (((*reg & mask) != 0) == set) return true;
    esp_rom_delay_us(1);
  }
  p4_port_fault = true;
  return false;
}
static void edpt_disable(uint8_t rhport, uint8_t ep_addr, bool stall) {
  dwc2_regs_t *dwc2 = DWC2_REG(rhport);
  uint8_t epnum = tu_edpt_number(ep_addr);
  bool in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;
  dwc2_dep_t *dep = &dwc2->ep[in ? 0 : 1][epnum];
  if (in) {
    if (dep->ctl & DIEPCTL_EPENA) {
      dep->ctl |= DIEPCTL_SNAK;
      if (!p4_wait(&dep->intr, DIEPINT_INEPNE, true)) return;
      dep->ctl |= DIEPCTL_EPDIS | (stall ? DIEPCTL_STALL : 0);
      if (!p4_wait(&dep->intr, DIEPINT_EPDISD, true)) return;
    } else {
      dep->ctl |= DIEPCTL_SNAK | (stall ? DIEPCTL_STALL : 0);
    }
    dep->intr = 0xffffffffu;
    dwc2->grstctl = GRSTCTL_TXFFLSH | ((uint32_t)epnum << GRSTCTL_TXFNUM_Pos);
    if (!p4_wait(&dwc2->grstctl, GRSTCTL_TXFFLSH, false)) return;
  } else if (epnum && (dep->ctl & DOEPCTL_EPENA)) {
    dwc2->dctl |= DCTL_SGONAK;
    if (!p4_wait(&dwc2->gintsts, GINTSTS_BOUTNAKEFF, true)) {
      dwc2->dctl |= DCTL_CGONAK;
      return;
    }
    dep->ctl |= DOEPCTL_EPDIS | (stall ? DOEPCTL_STALL : 0);
    bool stopped = p4_wait(&dep->intr, DOEPINT_EPDISD, true);
    dwc2->dctl |= DCTL_CGONAK;
    if (!stopped) return;
    dep->intr = 0xffffffffu;
  } else {
    dep->ctl |= stall ? DOEPCTL_STALL : 0;
  }
}

''' + src[end:]
replace_once("  deptsiz.packet_count = num_packets;\n  dep->tsiz = deptsiz.value;",
"""  deptsiz.packet_count = num_packets;
  // Buffer-DMA ISO IN requires MCNT=1 for a single transaction/microframe.
  if (dir == TUSB_DIR_IN && (dep->ctl & DIEPCTL_EPTYP_Msk) ==
      (DEPCTL_EPTYPE_ISOCHRONOUS << DIEPCTL_EPTYP_Pos))
    deptsiz.value |= 1u << DIEPTSIZ_MULCNT_Pos;
  dep->tsiz = deptsiz.value;""")
replace_once("        epin->tsiz              = deptsiz.value;",
"""        deptsiz.value |= 1u << DIEPTSIZ_MULCNT_Pos;
        epin->tsiz = deptsiz.value;
        // A previous FIFO prefetch may have advanced the DMA address.
        if (dma_device_enabled(dwc2)) epin->diepdma = (uintptr_t)xfer->buffer;""")
replace_once("  // Core Initialization\n", "  p4_port_fault = false;\n\n  // Core Initialization\n")
replace_once("  edpt_disable(rhport, p_endpoint_desc->bEndpointAddress, false);\n  edpt_activate",
             "  edpt_disable(rhport, p_endpoint_desc->bEndpointAddress, false);\n  if (p4_port_fault) return false;\n  edpt_activate")
replace_once("        dcd_event_xfer_complete(rhport, epnum | TUSB_DIR_IN_MASK, 0, XFER_RESULT_FAILED, true);",
             "        if (!p4_port_fault) dcd_event_xfer_complete(rhport, epnum | TUSB_DIR_IN_MASK, 0, XFER_RESULT_FAILED, true);")

# At bInterval > 1 an arbitrary parity at SET_INTERFACE can miss every OUT
# token. Learn the real host phase from OUT-token-while-disabled, as required
# by DWC2 buffer-DMA operation. No guessed millisecond watchdog phase.
replace_once("  uint8_t iso_retry; // ISO retry counter",
"""  uint8_t iso_retry; // ISO retry counter
  bool iso_out_synced, iso_out_queued;
  uint16_t iso_out_frame;""")
replace_once("  xfer->max_size = tu_edpt_packet_size(p_endpoint_desc);",
"""  xfer->max_size = tu_edpt_packet_size(p_endpoint_desc);
  xfer->iso_out_synced = false;
  xfer->iso_out_queued = false;""")
replace_once("  depctl.type = p_endpoint_desc->bmAttributes.xfer;",
"""  depctl.type = p_endpoint_desc->bmAttributes.xfer;
  if (dir == TUSB_DIR_OUT && depctl.type == DEPCTL_EPTYPE_ISOCHRONOUS) {
    depctl.set_nak = 1;
    dwc2->doepmsk |= DOEPMSK_OTEPDM;
  }""")
replace_once("    const uint32_t odd_now = dsts.frame_number & 1u;\n    if (odd_now) {",
"""    const uint32_t odd_now = dsts.frame_number & 1u;
    // HS interval > 1 keeps the host's parity; interval 1 targets next frame.
    const bool odd_target = (dir == TUSB_DIR_OUT && xfer->iso_out_synced && xfer->interval > 1)
                          ? (xfer->iso_out_frame & 1u) : !odd_now;
    depctl.set_data0_iso_even = 0;
    depctl.set_data1_iso_odd = 0;
    if (!odd_target) {""")
replace_once("    // Schedule packets to be sent within interrupt\n    edpt_schedule_packets(rhport, epnum, dir);",
"""    // First ISO OUT is queued in software until the host reveals its phase.
    dwc2_regs_t* dwc2 = DWC2_REG(rhport);
    const bool iso_out = dir == TUSB_DIR_OUT && epnum &&
        (dwc2->epout[epnum].ctl & DOEPCTL_EPTYP_Msk) ==
        (DEPCTL_EPTYPE_ISOCHRONOUS << DOEPCTL_EPTYP_Pos);
    if (iso_out) xfer->iso_out_queued = true;
    if (!iso_out || xfer->iso_out_synced)
      edpt_schedule_packets(rhport, epnum, dir);""")
replace_once("static void handle_epout_dma(uint8_t rhport, uint8_t epnum, dwc2_doepint_t doepint_bm) {\n  dwc2_regs_t* dwc2 = DWC2_REG(rhport);",
"""static void handle_epout_dma(uint8_t rhport, uint8_t epnum, dwc2_doepint_t doepint_bm) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  xfer_ctl_t* pending = XFER_CTL_BASE(epnum, TUSB_DIR_OUT);
  if (epnum && (doepint_bm.value & DOEPINT_OTEPDIS) &&
      pending->iso_out_queued && !pending->iso_out_synced) {
    pending->iso_out_frame = (p4_dwc2_frame_number() + pending->interval) & 0x3fff;
    pending->iso_out_synced = true;
    edpt_schedule_packets(rhport, epnum, TUSB_DIR_OUT);
  }
  if (epnum && doepint_bm.xfer_complete) {
    pending->iso_out_queued = false;
    pending->iso_out_frame = (p4_dwc2_frame_number() + pending->interval) & 0x3fff;
  }""")
replace_once("  dwc2->gintmsk |= GINTMSK_OEPINT | GINTMSK_IEPINT | GINTMSK_IISOIXFRM;",
             "  dwc2->gintmsk |= GINTMSK_OEPINT | GINTMSK_IEPINT | GINTMSK_IISOIXFRM | GINTMSK_PXFRM_IISOOXFRM;")
replace_once("static void handle_incomplete_iso_in(uint8_t rhport) {",
"""static void p4_incomplete_iso_out(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const uint32_t now = p4_dwc2_frame_number();
  const uint32_t parity = now & 1u;
  for (uint8_t n = 1; n < dwc2_ep_count(dwc2); ++n) {
    dwc2_depctl_t ctl = {.value = dwc2->epout[n].ctl};
    xfer_ctl_t* xfer = XFER_CTL_BASE(n, TUSB_DIR_OUT);
    if (ctl.enable && ctl.type == DEPCTL_EPTYPE_ISOCHRONOUS &&
        ctl.dpid_iso_odd == parity && xfer->iso_out_queued &&
        ((now - xfer->iso_out_frame) & 0x3fffu) < 8192) {
      edpt_disable(rhport, n, false);
      xfer->iso_out_synced = false;
      xfer->iso_out_queued = false;
      if (!p4_port_fault)
        dcd_event_xfer_complete(rhport, n, 0, XFER_RESULT_FAILED, true);
    }
  }
}

static void handle_incomplete_iso_in(uint8_t rhport) {""")
replace_once("  // Incomplete isochronous IN transfer interrupt handling.",
"""  if (gintsts & GINTSTS_PXFR_INCOMPISOOUT) {
    dwc2->gintsts = GINTSTS_PXFR_INCOMPISOOUT;
    p4_incomplete_iso_out(rhport);
  }

  // Incomplete isochronous IN transfer interrupt handling.""")
pathlib.Path(sys.argv[2]).write_text(src, encoding="utf-8", newline="\n")
