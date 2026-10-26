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
 *      aon_timer wake-up enabled. Every low-power exit goes through a reset (pwrmgr
 *      theory_of_operation; the SystemC model does the same), so the test runs twice: before the
 *      sleep it writes a marker and the device address it was given into the last words of
 *      retention SRAM, which keeps its contents across the low-power exit;
 *   4. after the wake-up (the reset reason the test ROM keeps in retention SRAM: LOW_POWER_EXIT;
 *      the marker found): the bus stayed idle, so
 *      the USB wake-up must not have fired: WAKE_INFO holds reason 4 (aon_timer) alone,
 *      WAKE_EVENTS.bus_not_idle is clear, the AON module still active (it is always-on), and
 *      WAKE_CONTROL.wake_ack releases it (module_active clear). The device address kept in
 *      retention SRAM is the one the host assigned (the "enumerated information" software keeps).
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
 * |                                              |   no link states)                            |
 * | 4: the AON wake module still active after    | eg2sim resets usbdev's AON wake state with   |
 * |   the low-power exit                         |   the chip (model finding: it is always-on)  |
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

// The marker and the device address in the last words of retention SRAM, across the low-power
// exit.
enum {
  kMarker = 0x5a5b1eef,
  kRetMarker = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES -
               8,
  kRetAddr = kRetMarker + 4,
};

/** Step 3: arm the AON wake module and the aon_timer, enter normal sleep. Does not return. */
static bool sleep_enter(uint32_t dev_addr) {
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
  eg2_chk_wr32(kRetAddr, dev_addr);
  eg2_chk_wr32(kRetMarker, kMarker);
  pwr_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  pwr_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, (1u << kUsbWake) | (1u << kAonWake));
  // Normal sleep: the main power domain stays on, the clocks stop.
  pwr_wr(PWRMGR_CONTROL_REG_OFFSET,
         (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT) | (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
  pwrmgr_sync();
  eg2_chk_step("normal sleep, wake-up after aon_timer ticks", EG2_PARAM(wake_ticks));
  eg2_chk_irq_off();
  for (uint32_t i = 0; i < 16; ++i) {
    __asm__ volatile("wfi");
  }
  // Still here: no low-power entry, or no wake-up.
  eg2_chk_wr32(kRetMarker, 0);
  return eg2_chk_true("low-power entry and its exit through a reset", false);
}

/** Step 4: the boot after the low-power exit. */
static bool after_wake(bool gaps) {
  bool ok = true;
  uint32_t dev_addr = eg2_chk_rd32(kRetAddr);
  eg2_chk_wr32(kRetMarker, 0);
  // The test ROM moves RESET_INFO into retention SRAM (creator.reset_reasons) and clears it.
  uint32_t info = eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR +
                               offsetof(retention_sram_t, creator.reset_reasons));
  ok = eg2_chk_eq("reset reason (low-power exit)", 1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT,
                  info) &&
       ok;
  uint32_t wake = pwr_rd(PWRMGR_WAKE_INFO_REG_OFFSET) & PWRMGR_WAKE_INFO_REASONS_MASK;
  ok = eg2_chk_eq("WAKE_INFO (aon_timer only: no USB wake-up on an idle bus)", 1u << kAonWake,
                  wake) &&
       ok;
  uint32_t ev = usb_rd(USBDEV_WAKE_EVENTS_REG_OFFSET);
  ok = eg2_chk_eq("WAKE_EVENTS.bus_not_idle", 0, (ev >> USBDEV_WAKE_EVENTS_BUS_NOT_IDLE_BIT) & 1) &&
       ok;
  if (gaps) {
    eg2_chk_skip("AON wake module active after the exit (eg2sim resets usbdev's AON wake state)");
  } else {
    ok = eg2_chk_true("AON wake module still active after the exit",
                      ev & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) &&
         ok;
  }
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
  if (gaps) {
    eg2_chk_skip("the device address the host assigned (no host in eg2sim)");
  } else {
    ok = eg2_chk_true("device address kept in retention SRAM", dev_addr != 0 && dev_addr < 128) &&
         ok;
  }
  // Tidy up: the wake-up sources off, their causes and the expected reset reason cleared.
  pwr_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, 0);
  pwr_wr(PWRMGR_CONTROL_REG_OFFSET, 0);
  pwrmgr_sync();
  pwr_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
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
  // After a power-on reset the marker words are uninitialised retention SRAM (the test ROM skips
  // its initialisation) and reading them is an integrity error on the chip (regr-vlt 2026-10-25).
  // The reset reasons the test ROM stored (bit 0: power-on) decide first, without reading them.
  uint32_t reasons = eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR +
                                  offsetof(retention_sram_t, creator.reset_reasons));
  if ((reasons & 0x1u) == 0 && eg2_chk_rd32(kRetMarker) == kMarker) {
    return after_wake(gaps) && eg2_chk_summary(gaps ? 4 : 6);
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
  return sleep_enter(dev_addr);
}
