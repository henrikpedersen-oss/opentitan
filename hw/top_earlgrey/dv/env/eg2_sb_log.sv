// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One <instance>.chip.log of the EG2 scoreboards, with the header REQ_STE_10 requires (EG2 spec
// "Agents"): who reports and what was run, so the log read on its own reproduces the run. Then one
// line per item; then the `# end:` summary line. A log without its end line was cut short (a crash
// or a timeout) and is read as a failure (REQ_STE_04).
//
// Where the values come from:
//   test, vseq, image, seed  +eg2_test (dvsim's {test}, set by the EG2 build modes), +UVM_TEST_SEQ,
//                            +sw_images, +SVSEED (Xcelium) or +ntb_random_seed (VCS); the software
//                            seed from the EG2 vseq (set_sw)
//   commit                   +eg2_git_eg2_top_verification, +eg2_git_eg2, +eg2_git_ibex (`git describe
//                            --always --dirty`, passed by eg2_top_verification's Makefile)
//   run                      +eg2_run_date (UTC), +eg2_run_host (the Makefile)
//   configuration            the image name (mode), cfg (boot, OTP image), +eg2_icache or the vseq,
//                            +eg2_mutate, +eg2_build (the build mode's run_opts)
//   tools                    +eg2_tool_sim, +eg2_tool_spike, +eg2_tool_cheriot_sail (the Makefile)
//   closes                   +eg2_closes=<testpoint>:<REQ>,<REQ>... (the run's hjson entry)
// A value nobody passed is written as "not given", never left out.
//
// The file is opened at the first item or at the end, whichever comes first, so the EG2 vseq has
// already told the software seed, mode and iCache setting (set_sw) when the header is written.

class eg2_sb_log;
  localparam int unsigned FormatVersion = 1;

  // What the EG2 vseq knows (chip_sw_eg2_base_vseq::cpu_init).
  protected static string sw_seed = "";
  protected static string sw_icache = "";

  string   inst;
  string   header_lines[$];
  protected int fd;
  protected bit opened;
  protected bit enabled = 1'b1;

  function new(string inst);
    bit en;
    this.inst = inst;
    if ($value$plusargs("eg2_sb_log=%b", en)) enabled = en;
  endfunction

  static function void set_sw(bit [31:0] seed, string icache);
    sw_seed = $sformatf("0x%08h", seed);
    sw_icache = icache;
  endfunction

  static function string plusarg(string name);
    string v;
    if ($value$plusargs({name, "=%s"}, v)) return v;
    return "not given";
  endfunction

  // The header: `port` (e.g. "port 1"), what the peripheral is used for and its specification,
  // the agent's settings, the predictor and the configuration from cfg.
  function void set_header(string port, string block, string used_for, string spec,
                           string agent, string predictor, string cfg_line);
    string seed, images, mode, closes, testpoint, reqs;
    int    colon;
    images = plusarg("sw_images");
    mode = (images != "not given" && has(images, "_cheriot")) ? "CHERIoT" : "RISC-V";
    seed = plusarg("SVSEED");
    if (seed == "not given") seed = plusarg("ntb_random_seed");
    closes = plusarg("eg2_closes");
    testpoint = closes;
    reqs = "not given";
    colon = index_of(closes, ":");
    if (colon >= 0) begin
      testpoint = closes.substr(0, colon - 1);
      reqs = closes.substr(colon + 1, closes.len() - 1);
    end
    header_lines.delete();
    header_lines.push_back($sformatf("%s (%s) reporting, log format %0d", inst, port, FormatVersion));
    header_lines.push_back($sformatf("block:         %s, %s ;", block, used_for));
    header_lines.push_back($sformatf("               specification %s", spec));
    header_lines.push_back($sformatf("run:           %s UTC on %s", plusarg("eg2_run_date"),
                                     plusarg("eg2_run_host")));
    header_lines.push_back($sformatf("test:          %s (vseq %s,", plusarg("eg2_test"),
                                     plusarg("UVM_TEST_SEQ")));
    header_lines.push_back($sformatf("               image %s)", images));
    header_lines.push_back($sformatf("seed:          %s (svseed) ; kEg2Seed %s", seed,
                                     sw_seed == "" ? "not given (not an EG2 framework test)" : sw_seed));
    header_lines.push_back($sformatf("commit:        eg2_top_verification %s, eg2 %s, ibex %s",
                                     plusarg("eg2_git_eg2_top_verification"),
                                     plusarg("eg2_git_eg2"), plusarg("eg2_git_ibex")));
    header_lines.push_back($sformatf("configuration: Ibex earlgrey, mode %s, %s, iCache %s,", mode,
                                     cfg_line, icache()));
    header_lines.push_back($sformatf("               mutation %s, build %s",
                                     plusarg("eg2_mutate") == "not given" ? "none" :
                                     plusarg("eg2_mutate"), plusarg("eg2_build")));
    header_lines.push_back($sformatf("agent:         %s", agent));
    header_lines.push_back($sformatf("expected from: %s", predictor));
    header_lines.push_back($sformatf("tools:         %s, Spike %s, CHERIoT-Sail %s",
                                     plusarg("eg2_tool_sim"), plusarg("eg2_tool_spike"),
                                     plusarg("eg2_tool_cheriot_sail")));
    header_lines.push_back($sformatf("closes:        testpoint %s ; %s", testpoint, reqs));
  endfunction

  protected static function string icache();
    string v = plusarg("eg2_icache");
    if (v != "not given") return v == "0" ? "off" : "on";
    if (sw_icache != "") return sw_icache;
    return "image default (on)";
  endfunction

  protected function void open();
    if (opened || !enabled) return;
    opened = 1'b1;
    fd = $fopen({inst, ".chip.log"}, "w");
    if (!fd) begin
      `uvm_error("EG2_SB", $sformatf("EG2_SB cannot open %s.chip.log", inst))
      return;
    end
    foreach (header_lines[i]) $fdisplay(fd, "# %s", header_lines[i]);
  endfunction

  function void line(string s);
    if (!enabled) return;
    open();
    if (fd) $fdisplay(fd, "%s", s);
  endfunction

  function void comment(string s);
    line({"# ", s});
  endfunction

  // The summary line, then the file is closed.
  function void finish(string summary);
    if (!enabled) return;
    open();
    if (!fd) return;
    $fdisplay(fd, "# end:           %s", summary);
    $fclose(fd);
    fd = 0;
  endfunction

  static function bit has(string s, string sub);
    return index_of(s, sub) >= 0;
  endfunction

  static function int index_of(string s, string sub);
    for (int i = 0; i + sub.len() <= s.len(); i++) begin
      if (s.substr(i, i + sub.len() - 1) == sub) return i;
    end
    return -1;
  endfunction
endclass
