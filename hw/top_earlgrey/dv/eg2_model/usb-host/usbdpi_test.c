// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "usbdpi_test.h"

#include <assert.h>

#include "usbdpi.h"

// Test-specific initialization
void usbdpi_test_init(usbdpi_ctx_t *ctx) {
  // Test-specific initialization code
  bool ok = false;
  switch (ctx->test_number) {
    case kUsbTestNumberSmoke:
      // The simple smoke test (usbdev_test) does not require any parameters
      // or special initialization at present
      ok = true;
      break;

    // Initialize streaming, or ISO streaming test
    case kUsbTestNumberMixed:
    case kUsbTestNumberIso:
    case kUsbTestNumberStreams: {
      // Number of concurrent byte streams
      const unsigned nstreams = ctx->test_arg[0] & 0xfU;
      // Poll device for IN packets in streaming test?
      bool retrieve = (ctx->test_arg[0] & 0x10U) != 0U;
      // Checking of received data against expected LFSR output
      bool checking = (ctx->test_arg[0] & 0x20U) != 0U;
      // Request retrying of IN packets, feigning error
      bool retrying = (ctx->test_arg[0] & 0x40U) != 0U;
      // Attempt to send OUT packets to device
      bool send = (ctx->test_arg[0] & 0x80U) != 0U;

      // Isochronous streams?
      bool iso = (ctx->test_number == kUsbTestNumberIso);

      if (nstreams <= USBDPI_MAX_STREAMS) {
        uint8_t xfr_types[USBDPI_MAX_STREAMS];

        // Complete the transfer types
        if (ctx->test_number == kUsbTestNumberMixed) {
          // The transfer types are specified in additional test arguments;
          // 2 bits per stream, with the USB-defined standard coding, lowest-
          // numbered stream in the LSBs.
          uint32_t mixed_types = (ctx->test_arg[3] << 16) |
                                 (ctx->test_arg[2] << 8) | ctx->test_arg[1];
          for (unsigned id = 0U; id < nstreams; id++) {
            xfr_types[id] = (mixed_types & 3U);
            // Control Transfer endpoints are not yet supported.
            assert(xfr_types[id] != USB_TRANSFER_TYPE_CONTROL);
            mixed_types >>= 2;
          }
        } else {
          // For Iso and Stream tests, all streams have the same transfer type.
          for (unsigned id = 0U; id < nstreams; id++) {
            xfr_types[id] =
                iso ? USB_TRANSFER_TYPE_ISOCHRONOUS : USB_TRANSFER_TYPE_BULK;
          }
        }
        ok = streams_init(ctx, nstreams, xfr_types, retrieve, checking,
                          retrying, send);
      }
    } break;

    case kUsbTestNumberPinCfg:
      ok = true;
      break;

    // usbdev_suspend_*_test: the phase to run (usbdev_suspend.c phase_set: "Test phase").
    case kUsbTestNumberSuspend:
      ctx->susp_phase = ctx->test_arg[0];
      ok = true;
      break;

    default:
      assert(!"Unrecognised/unsupported test in USBDPI");
      break;
  }

  // TODO - commute this to a proper test failure at run time
  assert(ok);
}

// usbdev_suspend.h usbdev_suspend_phase_t, the values the device gives in its test descriptor.
enum {
  kPhaseSuspend = 0u,
  kPhaseSleepResume,
  kPhaseSleepReset,
  kPhaseSleepDisconnect,
  kPhaseDeepResume,
  kPhaseDeepReset,
  kPhaseDeepDisconnect,
  kPhaseShutdown,
};

// Bus frames (FRAME_INTERVAL bit intervals of 12 Mbit/s) in a duration, at least one.
static uint32_t frames_ms(uint32_t ms) {
  uint32_t f = ms * 12000u / FRAME_INTERVAL;
  return f ? f : 1u;
}

static usbdpi_test_step_t step_for(usbdpi_ctx_t *ctx, usbdpi_test_step_t step,
                                   uint32_t frames) {
  ctx->step_frames = frames;
  return step;
}

bool usbdpi_step_no_sof(usbdpi_test_step_t step) {
  return (step >= STEP_IDLE_START && step < STEP_IDLE_END) || step == STEP_SUSPEND ||
         step == STEP_RESUME || step == STEP_RESUME_EOP ||
         step == STEP_DISCONNECT_VBUS;
}

// The host side of usbdev_suspend_*_test, after the test configuration. Per phase, as the FPGA/silicon
// host does it (sw/host/tests/chip/usb/usbdev_suspend.rs): suspend the device (the bus left idle,
// no SOF) for 3 ms in the Suspend phase and 50 ms in the sleep phases, then the phase's stimulus:
// Resume Signaling (Suspend, SleepResume, DeepResume), a Bus Reset (SleepReset, DeepReset) or VBUS
// removed (SleepDisconnect, DeepDisconnect). After a resume the host goes on to the next phase
// itself; after a reset or reconnection it configures the device again and reads the phase from
// its test descriptor. Resume Signaling per USB 2.0 7.1.7.7 (K for at least 20 ms, ended by a
// low-speed EOP, then 10 ms resume recovery). In simulation the device waits only 8 frames to be
// configured after a reconnection (usbdev_suspend.c phase_start_resume), so VBUS is removed for 2 ms, not
// usbdev_suspend.rs's 1000 ms ("fairly arbitrary, may be modified quite freely"). The Bus Reset lasts
// the USB 2.0 10 ms (the test allows 8 + 10 frames after one).
static usbdpi_test_step_t suspend_seq_next(usbdpi_ctx_t *ctx,
                                           usbdpi_test_step_t step) {
  if (step != STEP_GET_TEST_CONFIG && ctx->step_frames > 1u) {
    ctx->step_frames--;
    return step;
  }
  switch (step) {
    case STEP_GET_TEST_CONFIG:
      // Shutdown: the device disconnects; nothing for the host to do.
      if (ctx->susp_phase == kPhaseShutdown) {
        return step_for(ctx, STEP_SUSPEND_HOLD, 1u);
      }
      return step_for(ctx, STEP_SUSPEND_WAIT, 2u);
    case STEP_SUSPEND_WAIT:
      // A frame more than the duration, so that the idle bus spans it whole.
      return step_for(ctx, STEP_SUSPEND,
                      frames_ms(ctx->susp_phase == kPhaseSuspend ? 3u : 50u) + 1u);
    case STEP_SUSPEND:
      switch (ctx->susp_phase) {
        case kPhaseSuspend:
        case kPhaseSleepResume:
        case kPhaseDeepResume:
          return step_for(ctx, STEP_RESUME, frames_ms(20u));
        case kPhaseSleepReset:
        case kPhaseDeepReset:
          // USB 2.0 7.1.7.5: the host drives SE0 for at least 10 ms (TDRST). usbdpi.c holds SE0
          // through every frame of the step but the last (no SOF in between), so 10 ms + 532 bits.
          return step_for(ctx, STEP_BUS_RESET, frames_ms(10u) + 1u);
        default:
          return step_for(ctx, STEP_DISCONNECT_VBUS, frames_ms(2u));
      }
    case STEP_RESUME:
      return step_for(ctx, STEP_RESUME_EOP, 1u);
    case STEP_RESUME_EOP:
      return step_for(ctx, STEP_RESUME_RECOVERY, frames_ms(10u));
    case STEP_RESUME_RECOVERY:
      // usbdev_suspend.rs: the next phase after a resume.
      switch (ctx->susp_phase) {
        case kPhaseSuspend:
          ctx->susp_phase = kPhaseSleepResume;
          break;
        case kPhaseSleepResume:
          ctx->susp_phase = kPhaseSleepReset;
          break;
        default:
          ctx->susp_phase = kPhaseDeepReset;
          break;
      }
      return step_for(ctx, STEP_SUSPEND_WAIT, 2u);
    case STEP_DISCONNECT_VBUS:
      ctx->driving |= P2D_SENSE;
      return STEP_BUS_RESET;
    default:
      return step_for(ctx, STEP_SUSPEND_HOLD, 1u);
  }
}

// Return the next step in the test sequence
usbdpi_test_step_t usbdpi_test_seq_next(usbdpi_ctx_t *ctx,
                                        usbdpi_test_step_t step) {
  // Default to disconnecting and stopping
  usbdpi_test_step_t next_step = STEP_BUS_DISCONNECT;

  switch (step) {
    // Bus reset (VBUS newly present...)
    case STEP_BUS_RESET:
      next_step = STEP_GET_DEVICE_DESCRIPTOR;
      break;
    // Standard device set up sequence
    case STEP_GET_DEVICE_DESCRIPTOR:
      next_step = STEP_SET_DEVICE_ADDRESS;
      break;
    // TODO: a real host issues a second Bus Reset at this point, before
    // proceeding to read the device descriptor again and then set the address.
    case STEP_SET_DEVICE_ADDRESS:
      next_step = STEP_GET_CONFIG_DESCRIPTOR;
      break;
    case STEP_GET_CONFIG_DESCRIPTOR:
      next_step = STEP_GET_FULL_CONFIG_DESCRIPTOR;
      break;
    case STEP_GET_FULL_CONFIG_DESCRIPTOR:
      next_step = STEP_SET_DEVICE_CONFIG;
      break;
      // TODO: a real host issues additional GET_DESCRIPTOR commands at
      // this point, reading device qualifier and endpoint information

    case STEP_SET_DEVICE_CONFIG:
      next_step = STEP_GET_TEST_CONFIG;
      break;

    case STEP_SET_TEST_STATUS:
      next_step = STEP_BUS_DISCONNECT;
      break;

    case STEP_BUS_DISCONNECT:
      // deassert VBUS
      ctx->driving &= ~P2D_SENSE;
      break;

    // At this point we have discovered which test we are to perform
    default:
      switch (ctx->test_number) {
        // Streaming test (usbdev_stream_test), Iso (usbdev_iso_test) and
        // Mixed (usbdev_mixed_test)
        case kUsbTestNumberMixed:
        case kUsbTestNumberIso:
        case kUsbTestNumberStreams:
          switch (step) {
            case STEP_GET_TEST_CONFIG:
              next_step = STEP_STREAM_SERVICE;
              break;

            default:
              // The single state that keeps aggressively polling for IN
              // packets, processing them and pushing them OUT to the
              // device. The device test software is responsible for
              // terminating the test and reporting its status
              next_step = STEP_STREAM_SERVICE;
              break;
          }
          break;

        // Suspend/resume/sleep tests (usbdev_suspend.c)
        case kUsbTestNumberSuspend:
          next_step = suspend_seq_next(ctx, step);
          break;

        // Pin configuration test need only proceed as far as reading the
        // test configuration, at which point it knows to reset the device;
        // the software detects this and proceeds to the next configuration.
        case kUsbTestNumberPinCfg:
          next_step = STEP_BUS_RESET;
          break;

        // Default behavior; usbdev_test and usbdev_pincfg_test
        default:
          // TODO - for now we're just maintaining the existing timing
          // sequence
          switch (step) {
            case STEP_GET_TEST_CONFIG:
              next_step = STEP_FIRST_READ;
              break;

            case STEP_FIRST_READ:
              next_step = STEP_READ_BAUD;
              break;
            // case STEP_READ_BAUD:
            //   next_step = STEP_SECOND_READ;
            //   break;
            case STEP_SECOND_READ:
              next_step = STEP_SET_BAUD;
              break;
            case STEP_SET_BAUD:
              next_step = STEP_THIRD_READ;
              break;
            case STEP_THIRD_READ:
              next_step = STEP_TEST_ISO1;
              break;
            case STEP_TEST_ISO1:
              next_step = STEP_TEST_ISO2;
              break;
            // case STEP_TEST_ISO2:
            //   next_step = STEP_ENDPT_UNIMPL_SETUP;
            //   break;
            case STEP_ENDPT_UNIMPL_SETUP:
              next_step = STEP_ENDPT_UNIMPL_SETUP;
              break;
            case STEP_ENDPT_UNIMPL_OUT:
              next_step = STEP_ENDPT_UNIMPL_IN;
              break;
            case STEP_ENDPT_UNIMPL_IN:
              next_step = STEP_DEVICE_UK_SETUP;
              break;
            case STEP_DEVICE_UK_SETUP:
              next_step = STEP_IDLE_START;
              break;

            case STEP_IDLE_END:
              // Final resting state
              next_step = STEP_IDLE_END;
              break;

            // TODO - for now this is still required to advance through
            // the test steps
            case STEP_IDLE_START:
            case STEP_TEST_ISO2:
            case STEP_READ_BAUD:
            default:
              next_step = (usbdpi_test_step_t)((unsigned)step + 1U);
              break;
          }
          break;
      }
      break;
  }
  return next_step;
}
