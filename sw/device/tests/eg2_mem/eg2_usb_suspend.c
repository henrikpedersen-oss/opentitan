// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_usb_suspend (EG2 verification spec REQ_PBF_18; chip_testplan chip_sw_usb_suspend), in
 * both modes, against the USB DPI host model (chip_sw_usbdev_dpi_vseq's bench):
 *   1. enumeration and a data exchange, as usbdev_test (usb_testutils, simple serial: "Hi!Hi!"
 *      out and echoed back);
 *   2. the host stops sending SOF (the DPI's idle frames at the end of its default sequence): the
 *      device detects Suspend, `link_suspend` is raised and USBSTAT.LINK_STATE reads SUSPENDED
 *      (usbdev theory_of_operation "Link State"; >3 ms of idle bus);
 *   3. normal sleep: the AON wake module armed (WAKE_CONTROL.suspend_req, WAKE_EVENTS.module_active),
 *      pwrmgr's normal sleep with the USB wake-up (pinmux_usb_wkup_req, wake-up 3) and an
 *      aon_timer wake-up enabled. Normal sleep does not reset the chip: every power domain stays
 *      on ("All power domains are kept on for fast resume", pwrmgr theory_of_operation.md
 *      "Normal Sleep") and software "continues executing the instruction after the
 *      wait-for-interrupt invocation" (programmers_guide.md "Exiting from Normal Sleep";
 *      pwrmgr_fsm.sv FastPwrStateLowPowerPrep asserts the non-always-on domains' resets only as
 *      ~main_pd_ni; OpenTitan's pwrmgr_normal_sleep_all_wake_ups continues after the WFI; the
 *      SystemC model does the same). The core leaves the WFI on pwrmgr's wake-up interrupt,
 *      enabled at the PLIC and in mie (mstatus.MIE 0: no trap). pwrmgr's CONTROL is written
 *      read-modify-write, so USB_CLK_EN_ACTIVE (reset value 1) keeps the USB clock on in the
 *      active state: with it 0 the next usbdev access hangs;
 *   4. after the WFI, in the same boot: no reset (rstmgr's RESET_INFO has no low-power exit, the
 *      reset reason the test ROM recorded is unchanged), the mode (misa) is the build's; the bus
 *      stayed idle, so the USB wake-up must not have fired: WAKE_INFO holds reason 4 (aon_timer)
 *      alone, WAKE_EVENTS.bus_not_idle is clear, the AON module still active (it is always-on),
 *      and WAKE_CONTROL.wake_ack releases it (module_active clear). usbdev kept the device
 *      address the host assigned (USBCTRL.DEVICE_ADDRESS as before the sleep: the "enumerated
 *      information" is kept). A boot after the sleep (a low-power exit reset) fails.
 *
 * Not checked, and why (gaps against the testpoint):
 *   - "Resume the device through pinmux and check the link_resume interrupt", and data again
 *     after the resume: the DPI host model (hw/dv/dpi/usbdpi) has no Resume signalling; at the end
 *     of its default sequence it idles for good (STEP_IDLE_END). A host that resumes is needed
 *     (the chip bench's USB agent, framework step e).
 *   - The deep-sleep half (state wiped, re-enumeration): the DPI host enumerates once only.
 *
 * Knobs:
 *   wake_ticks   the aon_timer wake-up, in 200 kHz ticks (a tick is 5 us)
 *   model_gaps   skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | 1, 2 and 4: enumeration, data, Suspend       | eg2sim's usbdev has no USB host (no packets, |
 * |   (the device address stays 0)               |   no link states)                            |
 *
 * Built with EG2_USB_SUSPEND_INJECT_FAIL the first value check expects bit 0 flipped: the run must
 * fail with `MEM FAIL`.
 */

// The framework first: the USB library's headers define `noreturn`, which the framework's
// `__attribute__((noreturn))` must not meet.
#include "sw/device/tests/eg2_mem/eg2_chk.h"

#include "hw/top/aon_timer_regs.h"  // Generated.
#include "hw/top/pwrmgr_regs.h"     // Generated.
#include "hw/top/rstmgr_regs.h"     // Generated.
#include "hw/top/rv_plic_regs.h"    // Generated.
#include "hw/top/usbdev_regs.h"     // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/testing/usb_testutils.h"
#include "sw/device/lib/testing/usb_testutils_controlep.h"
#include "sw/device/lib/testing/usb_testutils_simpleserial.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define USB_SUSPEND_PARAMS(X) \
  X(wake_ticks, 40, 4, 1000)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(USB_SUSPEND_PARAMS)

enum {
  kPolls = 4000000,
  kUsbWake = kTopEarlgreyPowerManagerWakeUpsPinmuxUsbWkupReq,
  kAonWake = kTopEarlgreyPowerManagerWakeUpsAonTimerWkupReq,
};

// usbdev_test's configuration: two vendor interfaces with a bulk endpoint pair each.
static const uint8_t kConfigDescriptors[] = {
    USB_CFG_DSCR_HEAD(USB_CFG_DSCR_LEN + 2 * (USB_INTERFACE_DSCR_LEN + 2 * USB_EP_DSCR_LEN), 2),
    VEND_INTERFACE_DSCR(0, 2, 0x50, 1),
    USB_BULK_EP_DSCR(0, 1, 32, 0),
    USB_BULK_EP_DSCR(1, 1, 32, 4),
    VEND_INTERFACE_DSCR(1, 2, 0x50, 1),
    USB_BULK_EP_DSCR(0, 2, 32, 0),
    USB_BULK_EP_DSCR(1, 2, 32, 4),
};
static const uint8_t kTestDescriptor[] = {USB_TESTUTILS_TEST_DSCR(0, 0, 0, 0, 0)};

// The USB contexts live on eg2_test_main's stack: as globals they take the CHERIoT image's
// globals past 4 KiB (rtl_tickets.md F21; the image check of REQ_STE_12).
typedef struct usb_ctx {
  usb_testutils_ctx_t usbdev;
  usb_testutils_controlep_ctx_t control;
  usb_testutils_ss_ctx_t serial;
} usb_ctx_t;

static const char kHi[7] = "Hi!Hi!";
static char received[7];
static uint32_t n_received;

static void on_byte(uint8_t c) {
  if (n_received < 6) {
    received[n_received++] = (char)c;
  }
}

static uint32_t usb_rd(uint32_t off) { return eg2_dev_read32(kEg2DevUsbdev, off); }
static void usb_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevUsbdev, off, v); }
static uint32_t pwr_rd(uint32_t off) { return eg2_dev_read32(kEg2DevPwrmgr, off); }
static void pwr_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevPwrmgr, off, v); }

static void pwrmgr_sync(void) {
  pwr_wr(PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  for (uint32_t i = 0; i < kPolls && (pwr_rd(PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1); ++i) {
  }
}

/** Steps 1, 2: enumeration, data, then Suspend. */
static bool enumerate_and_suspend(usb_ctx_t *u) {
  bool ok = true;
  dif_pinmux_t pinmux;
  ok = eg2_chk_eq("pinmux handle", kDifOk,
                  dif_pinmux_init(mmio_region_from_addr(TOP_EARLGREY_PINMUX_BASE_ADDR),
                                  &pinmux)) &&
       ok;
  ok = eg2_chk_eq("usbdev sense from IOC7", kDifOk,
                  dif_pinmux_input_select(&pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
                                          kTopEarlgreyPinmuxInselIoc7)) &&
       ok;
  ok = eg2_chk_true("usb_testutils_init",
                    status_ok(usb_testutils_init(&u->usbdev, false, true, false))) &&
       ok;
  ok = eg2_chk_true("control endpoint",
                    status_ok(usb_testutils_controlep_init(
                        &u->control, &u->usbdev, 0, kConfigDescriptors,
                        sizeof(kConfigDescriptors), kTestDescriptor, sizeof(kTestDescriptor)))) &&
       ok;
  ok = eg2_chk_true("configured by the host",
                    status_ok(usb_testutils_controlep_config_wait(&u->control, &u->usbdev))) &&
       ok;
  ok = eg2_chk_true("simple serial",
                    status_ok(usb_testutils_simpleserial_init(&u->serial, &u->usbdev, 1,
                                                              on_byte))) &&
       ok;
  for (uint32_t i = 0; i < 6; ++i) {
    ok = eg2_chk_true("byte sent", status_ok(usb_testutils_simpleserial_send_byte(
                                       &u->serial, (uint8_t)kHi[i]))) &&
         ok;
  }
  for (uint32_t i = 0; i < kPolls && n_received < 6; ++i) {
    (void)usb_testutils_poll(&u->usbdev);
  }
  ok = eg2_chk_eq("bytes echoed by the host", 6, n_received) && ok;
  for (uint32_t i = 0; i < n_received; ++i) {
    if (received[i] != kHi[i]) {
      ok = eg2_chk_eq("echoed byte", (uint8_t)kHi[i], (uint8_t)received[i]) && ok;
      break;
    }
  }
  // The host goes idle: Suspend after 3 ms without SOF. The device keeps answering until then.
  const uint32_t susp = 1u << USBDEV_INTR_STATE_LINK_SUSPEND_BIT;
  uint32_t polls = 0;
  while (polls < kPolls && !(usb_rd(USBDEV_INTR_STATE_REG_OFFSET) & susp)) {
    (void)usb_testutils_poll(&u->usbdev);
    ++polls;
  }
  ok = eg2_chk_true("link_suspend raised", (usb_rd(USBDEV_INTR_STATE_REG_OFFSET) & susp) != 0) &&
       ok;
  ok = eg2_chk_eq("USBSTAT.LINK_STATE (SUSPENDED)", USBDEV_USBSTAT_LINK_STATE_VALUE_SUSPENDED,
                  (usb_rd(USBDEV_USBSTAT_REG_OFFSET) >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
                      USBDEV_USBSTAT_LINK_STATE_MASK) &&
       ok;
  usb_wr(USBDEV_INTR_STATE_REG_OFFSET, susp);
  return ok;
}

/** pwrmgr's wake-up interrupt at the PLIC and in mie, mstatus.MIE 0: it ends the WFI, no trap. */
static void wake_irq(bool on) {
  const uint32_t id = kTopEarlgreyPlicIrqIdPwrmgrWakeup;
  const uint32_t ie = RV_PLIC_IE0_0_REG_OFFSET + 4 * (id / 32);
  const uint32_t meie = 1u << 11;
  eg2_chk_irq_off();
  if (on) {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * id, 1);
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_THRESHOLD0_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevRvPlic, ie, eg2_dev_read32(kEg2DevRvPlic, ie) | (1u << (id % 32)));
    pwr_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 1u << PWRMGR_INTR_ENABLE_WAKEUP_BIT);
    __asm__ volatile("csrs mie, %0" : : "r"(meie));
  } else {
    __asm__ volatile("csrc mie, %0" : : "r"(meie));
    pwr_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevRvPlic, ie, eg2_dev_read32(kEg2DevRvPlic, ie) & ~(1u << (id % 32)));
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * id, 0);
  }
}

/** The reset reasons the test ROM recorded for this boot (it moves RESET_INFO there and clears
 * the register). */
static uint32_t recorded_reset_reason(void) {
  return eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR +
                      offsetof(retention_sram_t, creator.reset_reasons));
}

/**
 * Steps 3 and 4: arm the AON wake module and the aon_timer, enter normal sleep, and after the
 * WFI (the same boot) check what the sleep kept.
 */
static bool sleep_and_wake(uint32_t dev_addr) {
  bool ok = true;
  usb_wr(USBDEV_WAKE_CONTROL_REG_OFFSET, 1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  uint32_t ev = 0;
  for (uint32_t i = 0; i < kPolls; ++i) {
    ev = usb_rd(USBDEV_WAKE_EVENTS_REG_OFFSET);
    if (ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) {
      break;
    }
  }
  ok = eg2_chk_true("WAKE_EVENTS.module_active",
                    ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) &&
       ok;
  if (!ok) {
    return false;
  }
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, EG2_PARAM(wake_ticks));
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET,
                  1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  uint32_t reason = recorded_reset_reason();
  pwr_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  pwr_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, (1u << kUsbWake) | (1u << kAonWake));
  // Normal sleep: the main power domain stays on, the clocks stop. Read-modify-write: only the
  // hint and MAIN_PD_N change; USB_CLK_EN_ACTIVE stays as it is (1 after reset).
  const uint32_t kHint = 1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT;
  const uint32_t kPdN = 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT;
  uint32_t control = pwr_rd(PWRMGR_CONTROL_REG_OFFSET) & ~kHint;
  pwr_wr(PWRMGR_CONTROL_REG_OFFSET, control | kHint | kPdN);
  pwrmgr_sync();
  eg2_chk_step("normal sleep, wake-up after aon_timer ticks", EG2_PARAM(wake_ticks));
  wake_irq(true);
  __asm__ volatile("wfi");
  // Here after the wake-up, in the same boot (step 4).
  pwr_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
  uint32_t claimed = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
  if (claimed != 0) {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, claimed);
  }
  wake_irq(false);
  uint32_t wake = pwr_rd(PWRMGR_WAKE_INFO_REG_OFFSET);
  ok = eg2_chk_eq("WAKE_INFO (aon_timer only: no USB wake-up on an idle bus)", 1u << kAonWake,
                  wake) &&
       ok;
  ok = eg2_chk_eq("the WFI ended by pwrmgr's wake-up interrupt (PLIC claim)",
                  kTopEarlgreyPlicIrqIdPwrmgrWakeup, claimed) &&
       ok;
  ok = eg2_chk_eq("rstmgr RESET_INFO: no low-power exit reset", 0,
                  eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET) &
                      (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) &&
       ok;
  ok = eg2_chk_eq("the boot's recorded reset reason unchanged", reason, recorded_reset_reason()) &&
       ok;
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
#ifdef __CHERIOT__
  const uint32_t kMisaMode = 1u << 4;  // E: the CHERIoT build
#else
  const uint32_t kMisaMode = 1u << 8;  // I: the RISC-V build
#endif
  ok = eg2_chk_eq("mode after the wake-up (misa): unchanged", kMisaMode,
                  misa & ((1u << 4) | (1u << 8))) &&
       ok;
  ev = usb_rd(USBDEV_WAKE_EVENTS_REG_OFFSET);
  ok = eg2_chk_eq("WAKE_EVENTS.bus_not_idle", 0, (ev >> USBDEV_WAKE_EVENTS_BUS_NOT_IDLE_BIT) & 1) &&
       ok;
  ok = eg2_chk_true("AON wake module still active after the wake-up",
                    ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) &&
       ok;
  usb_wr(USBDEV_WAKE_CONTROL_REG_OFFSET, 1u << USBDEV_WAKE_CONTROL_WAKE_ACK_BIT);
  for (uint32_t i = 0; i < kPolls; ++i) {
    ev = usb_rd(USBDEV_WAKE_EVENTS_REG_OFFSET);
    if (!(ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT))) {
      break;
    }
  }
  ok = eg2_chk_eq("module_active after wake_ack", 0,
                  ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) &&
       ok;
  ok = eg2_chk_eq("USBCTRL.DEVICE_ADDRESS kept through the sleep", dev_addr,
                  (usb_rd(USBDEV_USBCTRL_REG_OFFSET) >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
                      USBDEV_USBCTRL_DEVICE_ADDRESS_MASK) &&
       ok;
  // Tidy up: the wake-up sources off, CONTROL back as it was (hint clear), their causes cleared.
  pwr_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, 0);
  pwr_wr(PWRMGR_CONTROL_REG_OFFSET, control);
  pwrmgr_sync();
  pwr_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET,
                  1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  // A low-power exit reset means the normal sleep reset the chip: the test does not start over.
  if (recorded_reset_reason() & (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) {
    eg2_chk_step("reset reasons", recorded_reset_reason());
    return eg2_chk_true("no reset after the normal sleep (the wake-up reset the chip)", false) &&
           eg2_chk_summary(1);
  }
  bool ok = true;
  uint32_t dev_addr = 0;
  // Zeroed: usb_testutils_init does not clear the per-endpoint transfer state (ctx->in[ep].transfer:
  // buffer, next_valid), which upstream's static contexts get zeroed. On the stack it held garbage,
  // and the first packet-sent interrupt "continued" a transfer that was never started (regr-vlt
  // 2026-10-26: "dif_usbdev_send(ctx->dev, ep, &transfer->next_part) returns 3", kDifBadArg).
  usb_ctx_t u = {0};
  if (gaps) {
    eg2_chk_skip("enumeration, data and Suspend (no USB host in eg2sim)");
  } else {
    ok = enumerate_and_suspend(&u) && ok;
    dev_addr = (usb_rd(USBDEV_USBCTRL_REG_OFFSET) >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
               USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
    ok = eg2_chk_true("device address assigned", dev_addr != 0) && ok;
  }
  if (!ok) {
    return eg2_chk_summary(1) && ok;
  }
  ok = sleep_and_wake(dev_addr) && ok;
  return eg2_chk_summary(gaps ? 10 : 16) && ok;
}
