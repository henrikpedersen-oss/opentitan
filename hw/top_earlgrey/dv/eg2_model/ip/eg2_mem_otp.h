// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// OTP and life-cycle models. The integration methods are in eg2_otp_ctrl.cc.

// The OTP contents are otp_ctrl.core's SW_CFG_WINDOW memory: eg2sim --otp loads the image there
// (one byte address per OTP byte, the whole 2064-byte array of otp_ctrl_mmap.md), DAI writes and
// life-cycle programming change it, and lc_ctrl reads its partition from it. Read from it raw: the
// image holds the SECRET* partitions scrambled (PRESENT, netlist constants); the DAI scrambles a
// secret partition's words on write and descrambles them on read, as the chip does. The other
// readers of the secret partitions (the key derivation interfaces) get the raw words. Not modelled: ECC, the background integrity and consistency checks
// (CHECK_TRIGGER, the periods), zeroization (no partition of this top is zeroizable), the key
// derivation interfaces' scrambling.

#ifndef EG2_MODEL_IP_EG2_MEM_OTP_H_
#define EG2_MODEL_IP_EG2_MEM_OTP_H_

#include "eg2_otp_ctrl.h"
#include "eg2_lc_ctrl.h"

#endif  // EG2_MODEL_IP_EG2_MEM_OTP_H_
