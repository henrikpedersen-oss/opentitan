// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// sysrst_ctrl, adc_ctrl and pinmux's wake-up detectors of the EG2 SystemC model, the always-on
// peripherals that wake the chip (hw/ip/sysrst_ctrl, hw/ip/adc_ctrl, hw/top_earlgrey/ip_autogen/
// pinmux: data/*.hjson, doc/theory_of_operation.md), one header each below. Their timers count the
// always-on clock, 200 kHz in sim_dv. Wake-ups go to pwrmgr by the top's index (sysrst_ctrl 0,
// adc_ctrl 1, pinmux 2, see ip/eg2_aon.h). Their inputs start at the values --stim gives
// (ip/eg2_periph_stim.h); sysrst_ctrl's and the pads pinmux's detectors watch also change while the
// model runs, as the chip bench drives them at a point of the test (eg2sim --stim
// <key>=<value>@<point>): their events follow the input changes, and so do adc_ctrl's channels
// (adc_ctrl.chan<N>=<v>@<point>).

#ifndef EG2_MODEL_IP_EG2_PERIPH_SYS_H_
#define EG2_MODEL_IP_EG2_PERIPH_SYS_H_

#include "eg2_sysrst_ctrl.h"
#include "eg2_adc_ctrl.h"
#include "eg2_pinmux.h"


#endif  // EG2_MODEL_IP_EG2_PERIPH_SYS_H_
