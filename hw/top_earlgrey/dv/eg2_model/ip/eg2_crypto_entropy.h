// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// entropy_src  The noise source and its conditioning are replaced by a deterministic generator:
//              seed n is the first 384 bits of SHAKE256("EG2 model entropy_src" || n), n a 64-bit
//              little-endian counter that only a power-on reset clears. Each seed is distinct and a
//              run is reproducible. Health tests always pass. Routed to firmware
//              (CONF.ENTROPY_DATA_REG_ENABLE and ENTROPY_CONTROL.ES_ROUTE true) the seeds are read
//              through ENTROPY_DATA; otherwise they go to csrng. Firmware override supports insert
//              (FW_OV_WR_DATA through the SHA3 conditioner) and observe (FW_OV_RD_DATA).
// csrng        NIST SP 800-90A CTR_DRBG, AES-256, no derivation function, seedlen 384, as the
//              theory of operation specifies, with the block cipher from hw/ip/aes/model/aes.c.
//              Three instances: edn0 (0), edn1 (1), software (2). Byte order as in the programmer's
//              guide "Endianness and Known-Answer Tests": every 32-bit word carries the next less
//              significant part of the NIST byte string, word 0 the least significant.
// edn0, edn1   Boot-time, auto and software request modes. EDN_BUS_CMP_ALERT compares each
//              block with the previous one this EDN received since it was enabled
//              (edn_core.sv compares 64 bits; the model all 128). An endpoint (another model) asks for
//              32-bit words with Eg2Edn::get(); EDN serves them from the genbits its csrng instance
//              generates, and get() fails while EDN has nothing to give, as an endpoint waits in
//              hardware.
//
// Where the documentation is silent and the RTL or the DIF was read: the MAIN_SM_STATE encodings
// (dif_entropy_src.h; hw/ip/edn/rtl/edn_pkg.sv).

#ifndef EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_
#define EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_

#include "eg2_entropy_src.h"
#include "eg2_csrng.h"
#include "eg2_edn.h"


#endif  // EG2_MODEL_IP_EG2_CRYPTO_ENTROPY_H_
