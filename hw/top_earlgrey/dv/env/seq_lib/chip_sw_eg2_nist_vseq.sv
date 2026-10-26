// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The NIST CAVP chip tests (eg2_top_verification spec "NIST CAVP Test Suite", REQ_PBF_23; software
// in sw/device/tests/eg2_nist). One vseq for every engine: `+eg2_nist=<test>` (the hjson entry)
// names the test, and the vseq writes that test's knobs only (a knob the image does not have is
// an error of the base vseq). The vectors themselves are tables in the image; the knobs choose how
// many random vectors run and how the engine is driven.
//
// `monte` (SHA-2, SHA-3) stays 0 unless the run pins +eg2_param_monte=1 (the weekly entries):
// the spec runs the Monte Carlo vectors in the model and weekly, never nightly. `model_gaps` is
// always 0 on the chip. `sign` of otbn_rsa (MODEXP_DE, 16.5 million instructions per 2048-bit
// signature) stays 0 unless pinned, as `make nist-otbn-ref` runs it only on request.
class chip_sw_eg2_nist_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_nist_vseq)
  `uvm_object_new

  rand int unsigned random_vectors;
  rand int unsigned manual_pct;
  rand int unsigned max_burst;
  rand int unsigned per_group;
  rand bit          sign;

  string test;

  constraint knobs_c {
    random_vectors inside {[0 : 32]};
    manual_pct dist {0 :/ 10, [1 : 99] :/ 70, 100 :/ 20};
    max_burst dist {1 :/ 10, [2 : 15] :/ 40, 16 :/ 20, [17 : 64] :/ 30};
    per_group inside {[1 : 2]};
    sign dist {1 :/ 80, 0 :/ 20};
  }

  virtual task pre_start();
    if (!$value$plusargs("eg2_nist=%s", test)) begin
      `uvm_fatal(`gfn, "EG2_NIST: +eg2_nist=<test> missing (chip_eg2_nist_tests.hjson)")
    end
    super.pre_start();
  endtask

  // Fewer random vectors where one costs more (the OTBN programs, DRBG, GCM).
  protected function int unsigned scaled(int unsigned max);
    return random_vectors % (max + 1);
  endfunction

  virtual function void write_params();
    case (test)
      "aes": begin
        set_param("random_vectors", 8 + scaled(24));
        set_param("manual_pct", manual_pct);
      end
      "aes_gcm": set_param("random_vectors", 4 + scaled(12));
      "sha2": begin
        set_param("random_vectors", 4 + scaled(12));
        set_param("max_burst", max_burst);
        set_param("monte", 0);
      end
      "hmac": begin
        set_param("random_vectors", 6 + scaled(18));
        set_param("max_burst", max_burst);
      end
      "sha3": begin
        set_param("random_vectors", 4 + scaled(12));
        set_param("monte", 0);
      end
      "drbg": set_param("random_vectors", 1 + scaled(5));
      "aes_kw": begin
        set_param("random_vectors", 4 + scaled(12));
        set_param("model_gaps", 0);
      end
      "otbn_ecdsa": begin
        set_param("per_group", per_group);
        set_param("random_vectors", scaled(1));
        set_param("sign", sign);
        set_param("model_gaps", 0);
      end
      "otbn_ecdh": begin
        set_param("per_group", per_group);
        set_param("random_vectors", scaled(1));
        set_param("model_gaps", 0);
      end
      "otbn_rsa": begin
        set_param("per_group", 1);
        set_param("random_vectors", 0);
        set_param("sign", 0);
        set_param("model_gaps", 0);
      end
      default: `uvm_fatal(`gfn, $sformatf("EG2_NIST: unknown test %s", test))
    endcase
  endfunction
endclass
