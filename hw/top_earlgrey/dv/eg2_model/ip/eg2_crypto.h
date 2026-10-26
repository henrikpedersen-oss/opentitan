// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Cryptography and entropy of the EG2 SystemC model (Phase B group: crypto). Registered through
// EG2_MODELS_CRYPTO, at the end of this file.
//
//   eg2_crypto_entropy.h  entropy_src, csrng (CTR_DRBG), edn0 / edn1 (endpoint API Eg2Edn::get)
//   eg2_crypto_aes.h      aes
//   eg2_crypto_hash.h     hmac, kmac (application interface Eg2Kmac::app_kmac / app_cshake)
//   eg2_crypto_keymgr.h   keymgr_dpe, otbn (register interface only)
//
// Reference code reused (compiled in by the model's Makefile): hw/ip/aes/model/aes.c (lowRISC,
// Apache-2.0), hw/ip/hmac/dv/cryptoc_dpi/sha{256,384,512}.c (Google CryptoC, Apache-2.0),
// hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp (header-only, public domain).

#ifndef EG2_MODEL_IP_EG2_CRYPTO_H_
#define EG2_MODEL_IP_EG2_CRYPTO_H_

#include "../eg2_ip_model.h"
#include "eg2_crypto_aes.h"
#include "eg2_crypto_entropy.h"
#include "eg2_crypto_hash.h"
#include "eg2_crypto_keymgr.h"

// This group's entries of kModels (eg2_ip_models.cc): {window name or IP type, factory}.
#define EG2_MODELS_CRYPTO                                  \
  {"entropy_src", make<eg2_model::Eg2EntropySrc>},          \
  {"csrng", make<eg2_model::Eg2Csrng>},                     \
  {"edn", make<eg2_model::Eg2Edn>},                         \
  {"aes", make<eg2_model::Eg2Aes>},                         \
  {"hmac", make<eg2_model::Eg2Hmac>},                       \
  {"kmac", make<eg2_model::Eg2Kmac>},                       \
  {"keymgr_dpe", make<eg2_model::Eg2KeymgrDpe>},            \
  {"otbn", make<eg2_model::Eg2Otbn>},

#endif  // EG2_MODEL_IP_EG2_CRYPTO_H_
