# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""The legacy chip tests built in both modes (eg2_top_verification, EG2 spec REQ_PER, REQ_STE_02).

Each test listed here keeps its targets and also gets `<name>_cheriot` in its own package, built
from the same source for CHERIoT mode (rules/opentitan/cheriot_both.bzl); dvsim runs it as
`chip_..._cheriot` (regression `eg2_per_cheriot`). A test is listed once its CHERIoT build links
and passes `make check-cheriot`.
"""

CHERIOT_BOTH_MODES = [
    # The legacy peripheral tests of the spec's REQ_PER (Post-Boot Features, REQ_PBF_13, REQ_PBF_18,
    # REQ_PBF_21; I3C excluded): regression eg2_per_cheriot.
    "//hw/top_earlgrey/sw/autogen/tests:plic_all_irqs_test_0",
    "//hw/top_earlgrey/sw/autogen/tests:plic_all_irqs_test_10",
    "//hw/top_earlgrey/sw/autogen/tests:plic_all_irqs_test_20",
    "//sw/device/tests/sim_dv:adc_ctrl_sleep_debug_cable_wakeup_test",
    "//sw/device/tests/sim_dv:gpio_test",
    "//sw/device/tests/sim_dv:i2c_device_tx_rx_test",
    "//sw/device/tests/sim_dv:i2c_host_tx_rx_test",
    "//sw/device/tests/sim_dv:pwrmgr_sysrst_ctrl_test",
    "//sw/device/tests/sim_dv:sensor_ctrl_status_test",
    "//sw/device/tests/sim_dv:spi_passthrough_test",
    "//sw/device/tests/sim_dv:spi_host_tx_rx_test",
    "//sw/device/tests/sim_dv:sysrst_ctrl_ec_rst_l_test",
    "//sw/device/tests/sim_dv:ast_usb_clk_calib",
    "//sw/device/tests:aon_timer_irq_test",
    "//sw/device/tests:aon_timer_sleep_wdog_sleep_pause_test",
    "//sw/device/tests:aon_timer_smoketest",
    "//sw/device/tests:aon_timer_wdog_bite_reset_test",
    "//sw/device/tests:gpio_smoketest",
    "//sw/device/tests:plic_sw_irq_test",
    "//sw/device/tests:pwrmgr_deep_sleep_all_wake_ups",
    "//sw/device/tests:pwrmgr_normal_sleep_all_wake_ups",
    "//sw/device/tests:pwrmgr_sensor_ctrl_deep_sleep_wake_up",
    "//sw/device/tests:sensor_ctrl_wakeup_test",
    "//sw/device/tests:pwrmgr_usb_clk_disabled_when_active_test",
    "//sw/device/tests:pwrmgr_usbdev_smoketest",
    "//sw/device/tests:rstmgr_sw_req_test",
    "//sw/device/tests:rv_timer_smoketest",
    "//sw/device/tests:rv_timer_systick_test",
    "//sw/device/tests:sensor_ctrl_alert_test",
    "//sw/device/tests:sleep_pin_mio_dio_val_test",
    "//sw/device/tests:sleep_pin_retention_test",
    "//sw/device/tests:sleep_pin_wake_test",
    "//sw/device/tests:spi_device_sleep_test",
    "//sw/device/tests:spi_device_tpm_tx_rx_test",
    "//sw/device/tests:sysrst_ctrl_in_irq_test",
    "//sw/device/tests:sysrst_ctrl_inputs_test",
    "//sw/device/tests:sysrst_ctrl_outputs_test",
    "//sw/device/tests:sysrst_ctrl_reset_test",
    "//sw/device/tests:sysrst_ctrl_ulp_z3_wakeup_test",
    "//sw/device/tests:uart_smoketest",
    "//sw/device/tests:uart_tx_rx_test",
    "//sw/device/tests:usbdev_aon_pullup_test",
    "//sw/device/tests:usbdev_config_host_test",
    "//sw/device/tests:usbdev_test",
    "//sw/device/tests:usbdev_pincfg_test",
    "//sw/device/tests:usbdev_pullup_test",
    "//sw/device/tests:usbdev_setuprx_test",
    # Not //sw/device/tests:usbdev_stream_test: its globals are 9.4 KiB (the test's own stream
    # context, 7.9 KiB), and the CHERIoT toolchain reaches a global more than 2 KiB from CGP at the
    # wrong address (eg2_top_verification status/rtl_tickets.md F21): its CHERIoT build traps at
    # the first such access. Listed again once the toolchain is fixed.
    "//sw/device/tests:usbdev_toggle_restore_test",
    "//sw/device/tests:usbdev_vbus_test",

    # The other legacy tests whose `_cheriot` run the spec names (OTP, SRAM, alerts, core, debug):
    # regression eg2_legacy_cheriot.
    "//sw/device/tests:aes_smoketest",
    "//sw/device/tests:alert_handler_escalation_test",
    "//sw/device/tests:otp_ctrl_descrambling_test",
    "//sw/device/tests:otp_ctrl_mem_access_test",
    "//sw/device/tests:rv_core_ibex_icache_invalidate_test",
    "//sw/device/tests:rv_core_ibex_nmi_irq_test",
    "//sw/device/tests:rv_dm_access_after_wakeup_rma",
    "//sw/device/tests:rv_dm_ndm_reset_req_rma",
    "//sw/device/tests:rv_dm_ndm_reset_req_when_cpu_halted_rma",
    "//sw/device/tests:sram_ctrl_sleep_sram_ret_contents_no_scramble_test",
    "//sw/device/tests:sram_ctrl_sleep_sram_ret_contents_scramble_test",
    "//sw/device/tests/sim_dv:all_escalation_resets_test",
    # Not //sw/device/tests:sram_ctrl_scrambled_access_test (RV32 inline assembly that takes
    # integer addresses: the SRAM re-scramble, copy and reset sequence) nor
    # //sw/device/tests/sim_dv:sram_ctrl_execution_main_test (an RV32 `jalr` placed in .data, and
    # an exception handler that rewrites OTTF's RV32 trap frame through OT_FRAME_ADDR): neither
    # source is purecap. Their CHERIoT tests in the spec (REQ_OTP_02, REQ_SRAM_01) are new tests.
]
