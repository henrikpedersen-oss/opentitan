// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_usb_suspend (sw/device/tests/eg2_mem/eg2_usb_suspend.c; EG2 spec REQ_PBF_18): the USB
// DPI host model connected as chip_sw_usbdev_dpi_vseq connects it (the tb's weak pull-ups on the
// DIOs released, the DPI drivers on, the packet buffer zeroed), and the aon_timer wake-up time.
class chip_sw_eg2_usb_suspend_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_usb_suspend_vseq)

  `uvm_object_new

  rand int unsigned wake_ticks;
  constraint wake_ticks_c {
    wake_ticks dist {[4:20] :/ 40, [21:200] :/ 50, [201:1000] :/ 10};
  }

  virtual function void write_params();
    set_param("wake_ticks", wake_ticks);
  endfunction

  virtual task body();
    super.body();
    cfg.chip_vif.cfg_default_weak_pulls_on_dios(0);
    cfg.usb20_vif.enable_driver(1);
    cfg.mem_bkdr_util_h[UsbdevBuf].clear_mem();
  endtask

endclass : chip_sw_eg2_usb_suspend_vseq
