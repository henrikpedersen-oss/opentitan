// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The CHERIoT layer of the EG2 constrained-random tests (eg2_top_verification, spec "Constraints
// in SystemVerilog": chip_sw_eg2_cheriot_vseq; "CHERIoT Test Suite", "Shared classes").
//
// Capability shape, cursor against bounds and permission families under the weight rule (the
// spec's w_cursor_c), the arena the software derives its capabilities in (a representable
// capability: base and length aligned to 2^E, length at most 2^(E+9)), the capability instruction
// family weights (their defaults ±50 % per seed), and `addr_hi` (capabilities based at or above
// 2^31; stimulus only). A test's vseq writes the ones its software reads.
class chip_sw_eg2_cheriot_vseq extends chip_sw_eg2_rand_vseq;
  `uvm_object_utils(chip_sw_eg2_cheriot_vseq)

  `uvm_object_new

  localparam bit [31:0] MainSramBase = 32'h1000_0000;
  localparam bit [31:0] SecSramTop = 32'h1003_0000;

  typedef enum int unsigned {
    CurBelowBase, CurAtBase, CurInside, CurLastByte, CurAtTop, CurPastTop, CurAboveTop,
    CurZero, CurAllOnes, CurNum
  } cursor_e;
  localparam bit [CurNum-1:0] CursorPlain = 1 << CurInside;

  typedef enum int unsigned {
    ShapeRoot, ShapeExp0, ShapeSmallExp, ShapeExp24, ShapeZeroLen, ShapeUntagged, ShapeSealed,
    ShapeSentry, ShapeReducedPerms, ShapeNum
  } shape_e;
  localparam bit [ShapeNum-1:0] ShapePlain = 1 << ShapeSmallExp;

  typedef enum int unsigned {
    PermAll, PermNoLd, PermNoSd, PermNoMc, PermNoSl, PermNoGl, PermNoEx, PermNoSr, PermNoSeal,
    PermNone, PermNum
  } perm_e;
  localparam bit [PermNum-1:0] PermPlain = 1 << PermAll;

  // The capability instruction families of chip_sw_eg2_rand_cap_ops and their default weights.
  typedef enum int unsigned {
    FamBounds, FamAddress, FamGet, FamSeal, FamHighSubset, FamAuipcc, FamCrrl, FamSpecial,
    FamMove, FamNum
  } family_e;
  localparam int unsigned FamDefault[FamNum] = '{20, 20, 15, 10, 10, 10, 5, 5, 5};

  rand int unsigned w_cursor[CurNum];
  rand int unsigned w_shape[ShapeNum];
  rand int unsigned w_perm[PermNum];
  rand int unsigned w_family[FamNum];
  rand bit [3:0]    arena_e;
  rand bit [31:0]   arena_base, arena_len;
  rand bit          addr_hi;

  constraint w_cursor_c {
    solve boost, boosted before w_cursor;
    foreach (w_cursor[i]) w_cursor[i] inside {[1:100]};
    if (boost && boosted < CurNum) {
      w_cursor[boosted] * 2 == w_cursor.sum();
    } else {
      (w_cursor.sum() with (CursorPlain[item.index] ? 0 : item)) * 10 >= w_cursor.sum() * 6;
      (w_cursor.sum() with (CursorPlain[item.index] ? item : 0)) * 10 <= w_cursor.sum() * 3;
      (w_cursor.sum() with (CursorPlain[item.index] ? item : 0)) * 20 >= w_cursor.sum();
    }
  }
  constraint w_shape_c {
    solve boost, boosted before w_shape;
    foreach (w_shape[i]) w_shape[i] inside {[1:100]};
    if (boost && boosted < ShapeNum) {
      w_shape[boosted] * 2 == w_shape.sum();
    } else {
      (w_shape.sum() with (ShapePlain[item.index] ? 0 : item)) * 10 >= w_shape.sum() * 6;
      (w_shape.sum() with (ShapePlain[item.index] ? item : 0)) * 10 <= w_shape.sum() * 3;
      (w_shape.sum() with (ShapePlain[item.index] ? item : 0)) * 20 >= w_shape.sum();
    }
  }
  constraint w_perm_c {
    solve boost, boosted before w_perm;
    foreach (w_perm[i]) w_perm[i] inside {[1:100]};
    if (boost && boosted < PermNum) {
      w_perm[boosted] * 2 == w_perm.sum();
    } else {
      (w_perm.sum() with (PermPlain[item.index] ? 0 : item)) * 10 >= w_perm.sum() * 6;
      (w_perm.sum() with (PermPlain[item.index] ? item : 0)) * 10 <= w_perm.sum() * 3;
      (w_perm.sum() with (PermPlain[item.index] ? item : 0)) * 20 >= w_perm.sum();
    }
  }
  constraint w_family_c {
    foreach (w_family[i]) {
      w_family[i] inside {[FamDefault[i] / 2 : FamDefault[i] * 3 / 2]};
    }
  }
  constraint arena_c {
    solve arena_e before arena_base, arena_len;
    arena_e dist {0 := 30, [1:4] :/ 40, [5:14] :/ 30};
    arena_e <= 14;
    arena_base inside {[MainSramBase:SecSramTop - 1]};
    {1'b0, arena_base} + arena_len <= {1'b0, SecSramTop};
    (arena_base & ((32'd1 << arena_e) - 1)) == 0;
    (arena_len & ((32'd1 << arena_e) - 1)) == 0;
    arena_len > 0;
    arena_len <= (32'd1 << (arena_e + 9));
  }
  constraint addr_hi_c {
    soft addr_hi == 1'b1;
  }

endclass : chip_sw_eg2_cheriot_vseq
