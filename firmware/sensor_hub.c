/* ============================================================================
 *  sensor_hub.c — Reads sensors + manages per-channel state
 *  Board: BRD2709A (EFR32xG26 Explorer Kit). Drop sensor D0 -> PD02.
 *
 *  Full hardware wiring (per the wiring diagram provided by the user):
 *    - HX711 (load cell, FLOW channel): DT -> PC01, SCK -> PC03 (dedicated
 *      wires, NOT routed through the standard mikroBUS SPI header, so the
 *      MOSI/MISO names in the SDK's SPI docs do not apply here). Pure
 *      GPIO bit-banging. FINALIZED — matches the user's actual physical
 *      wiring, do not change again.
 *    - MAX30102 (HR + SpO2 channels, single chip, DFRobot Gravity SEN0344
 *      module): SDA/SCL -> shared mikroBUS/Qwiic I2C bus (PC07=SDA,
 *      PC05=SCL). ALSO pure GPIO bit-banging, NOT using the SDK's built-in
 *      I2CSPM driver — that driver has a FIXED 300-SECOND timeout per
 *      transaction, far too dangerous for a medical monitoring device
 *      (one stuck bus transaction would freeze the whole system for 5
 *      minutes). See the comment at the top of the MAX30102 block below
 *      for details.
 * ========================================================================== */
#include "sensor_hub.h"
#include "drop_filter.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_core.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include "nvm3_default.h"
#include <stdio.h>
#include <string.h>

/* ---- Drop sensor pin (BRD2709A, mikroBUS "AN" = PD02) ---- */
#define SENSOR_PORT   gpioPortD
#define SENSOR_PIN    2

/* Noise rejection for the drop sensor lives in drop_filter.c - kept separate
 * from this file so it can be replayed against recorded noise on a PC. See
 * tools/drop_filter_test.c. */

/* Baseline value (= scaler.mean) for a channel that is NOT CONNECTED -> normalizes
 * to ~0 -> lets the autoencoder reconstruct it cleanly. When a channel is
 * enabled (ENABLED=1) but temporarily loses signal, this same value is used
 * as a "placeholder" while waiting for a fresh sample. */
#define HR_BASE_FILL    81.68f
#define SPO2_BASE_FILL  97.80f

#if DROPS_ENABLED
static drop_filter_t drop_filter;
#endif

static uint32_t now_ms(void)
{
  return sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
}

/* Microseconds. Two callers need finer than a millisecond: the drop detector's
 * confirm window, and the buzzer's tone generator.
 *
 * The drop pulses measured on this sensor are 1-6 ms wide, and a millisecond
 * clock quantises the short ones to zero - which is exactly how the first
 * version of this filter came to count nothing at all. The sleeptimer runs at
 * 32768 Hz here, so one tick is ~30 us: coarse, but twenty times finer than
 * what it replaces, and far finer than the 600 us being measured.
 *
 * Wraps every ~71 minutes. That is fine because this value is only ever used
 * for differences within a single pulse; everything that can span minutes
 * keeps using now_ms(). */
static uint32_t now_us(void)
{
  static uint32_t hz = 0;
  if (hz == 0) hz = sl_sleeptimer_get_timer_frequency();

  uint64_t ticks = (uint64_t)sl_sleeptimer_get_tick_count();
  return (uint32_t)((ticks * 1000000ULL) / (uint64_t)hz);
}

/* ============================================================================
 *  HX711 (load cell) — FLOW channel
 *  The HX711's dedicated 2-wire protocol (not standard SPI): DOUT signals
 *  "data ready" by pulling itself low; read 24 bits (MSB first) by
 *  generating 24 SCK pulses, sampling one bit per rising edge; the 25th
 *  pulse re-selects channel A / gain 128 for the next reading (the default
 *  behavior of the common Arduino HX711 library, matching the
 *  hx711_test.ino the user already tested against).
 * ========================================================================== */
#define HX711_DOUT_PORT   gpioPortC
#define HX711_DOUT_PIN    1   /* PC01 = mikroBUS MISO theo sơ đồ chân board.
                                * Khớp đúng dây thật, đừng đổi sang PC02 —
                                * PC02 là MOSI và đang có CÒI ở đó. */
#define HX711_SCK_PORT    gpioPortC
#define HX711_SCK_PIN     3   /* mikroBUS SCK/CLK - confirmed correct via the BRD2709A source */

#define HX711_TARE_SAMPLES      30U     /* number of samples averaged during taring - matches hx711_test.ino (scale.tare(30)) */
#define FLOW_CALC_INTERVAL_MS   10000U  /* recompute flow rate every 10s to avoid noise from differentiating a jittery weight signal */
#define HX711_PRINT_INTERVAL_MS 500U    /* print weight to serial every 500ms - matches hx711_test.ino's print cadence */
#define HX711_ZERO_FILTER_G     15.0f   /* +-15g around 0 -> displayed as 0 (matches the +-0.015kg threshold in the original .ino) */
#define HX711_CONFIRM_TOLERANCE_COUNTS  3000L  /* ~214g (14 counts/g) - threshold for confirming 2 consecutive samples agree */

static int32_t  hx711_pending_raw   = 0;       /* "candidate" sample awaiting confirmation on the next read */
static bool     hx711_have_pending  = false;

static bool     hx711_tare_started  = false;   /* taring has started (prints the message once) */
static bool     hx711_inited        = false;   /* taring is done, ready for real readings */
static int32_t  hx711_tare_offset   = 0;
static int64_t  hx711_tare_accum    = 0;       /* SIGNED - raw can be negative; an unsigned accumulator would overflow */
static uint32_t hx711_tare_count    = 0;
static float    hx711_weight_g      = 0.0f;    /* EMA-filtered weight, grams */
static bool     hx711_have_sample   = false;
static uint32_t hx711_last_valid_ms = 0;
static uint32_t hx711_last_print_ms = 0;

/* ---- Detect a REAL HX711 module before trusting any value -----
 * A real HX711 pulls DOUT LOW on a REGULAR CYCLE (~100ms at 10SPS, ~12.5ms
 * at 80SPS) to signal "new sample ready". A floating pin (no real module
 * connected, or the module hasn't started yet) makes the DOUT level jump
 * around with NO regular cycle - we need several consecutive "ready" events
 * with a plausible spacing before concluding a real chip is present, to
 * avoid mistaking noise for real data. */
#define HX711_DETECT_INTERVAL_MIN_MS   8U
#define HX711_DETECT_INTERVAL_MAX_MS   500U
#define HX711_DETECT_STREAK_NEEDED     4U
#define HX711_DETECT_REPORT_MS         2000U   /* report "not found yet" every 2s while still unconfirmed */

static bool     hx711_found            = false;
static uint32_t hx711_last_ready_ms    = 0;
static uint32_t hx711_good_streak      = 0;
static uint32_t hx711_last_report_ms   = 0;

static float    flow_anchor_weight_g = 0.0f;
static uint32_t flow_anchor_ms       = 0;
static float    flow_ml_per_h        = 0.0f;   /* most recently computed flow rate, ml/hour */

/* Doctor-set target infusion rate (ml/hour). Starts at the SET_FLOW_ML_H
 * default from sensor_hub.h but can be changed at runtime - either locally
 * or by the doctor writing the TargetFlowMlH Zigbee attribute from the HIS
 * Server (see sh_set_target_flow_ml_h() and app.c's
 * sl_zigbee_af_post_attribute_change_cb()). sh_flow_ratio() compares the
 * measured flow against THIS value, not the compile-time default, so a
 * change takes effect immediately without reflashing. */
static float target_flow_ml_h = SET_FLOW_ML_H;

/* Doctor-set target drop rate (drops/min) - same idea as target_flow_ml_h
 * above, changeable at runtime via sh_set_target_drops_per_min(). */
static float target_drops_per_min = SET_DROPS_DPM;

/* ---- Persisting the doctor's targets across power cycles (NVM3) ----------
 * Without this, a power blip or a reflash silently reverts BOTH targets to
 * the SET_FLOW_ML_H / SET_DROPS_DPM compile-time defaults while the infusion
 * carries on - the AI would then judge the flow against a prescription
 * nobody ordered, and the bedside dashboard would show a target the doctor
 * never set. For a medical device that is a real hazard, so both values are
 * written to flash whenever they actually change and restored at boot.
 *
 * Key 0x0A001 is in the NVM3 "user" region (0x00000-0x0FFFF per
 * sl_token_manager_defines.h) - the Zigbee stack lives in its own region
 * from 0x10000 up, so there is no collision. Both targets share ONE record
 * so they can never be left half-updated by a power loss mid-write.
 *
 * Flash wear is a non-issue: a write only happens when a doctor actually
 * changes a target (a handful of times per patient), never on the periodic
 * report path - sh_set_target_*() below returns early when the value is
 * unchanged, and app.c re-writes the SAME value into the Zigbee attribute
 * every second. */
#define SMART_IV_NVM3_KEY_TARGETS  0x0A001U
#define SMART_IV_TARGETS_MAGIC     0x53495631U   /* "SIV1" - guards against reading a stale/foreign record */

typedef struct {
  uint32_t magic;
  float    flow_ml_h;
  float    drops_per_min;
} sh_persisted_targets_t;

static void targets_save(void)
{
  sh_persisted_targets_t rec = {
    .magic         = SMART_IV_TARGETS_MAGIC,
    .flow_ml_h     = target_flow_ml_h,
    .drops_per_min = target_drops_per_min,
  };

  Ecode_t st = nvm3_writeData(nvm3_defaultHandle, SMART_IV_NVM3_KEY_TARGETS,
                              &rec, sizeof(rec));
  if (st != ECODE_NVM3_OK) {
    /* Report but keep running on the in-RAM values: losing persistence is
     * far less bad than refusing to monitor the patient. */
    printf("[NVM3] WARNING: could not save the doctor's targets (0x%08lX) - "
           "they will revert to defaults after a power cycle\r\n", (unsigned long)st);
    return;
  }
  printf("[NVM3] Targets saved: %d ml/h, %d dpm\r\n",
         (int)(target_flow_ml_h + 0.5f), (int)(target_drops_per_min + 0.5f));
}

static void targets_load(void)
{
  sh_persisted_targets_t rec;
  uint32_t type = 0;
  size_t   len  = 0;

  if (nvm3_getObjectInfo(nvm3_defaultHandle, SMART_IV_NVM3_KEY_TARGETS, &type, &len) != ECODE_NVM3_OK
      || type != NVM3_OBJECTTYPE_DATA || len != sizeof(rec)) {
    printf("[NVM3] No saved targets yet - using defaults: %d ml/h, %d dpm\r\n",
           (int)(target_flow_ml_h + 0.5f), (int)(target_drops_per_min + 0.5f));
    return;
  }

  if (nvm3_readData(nvm3_defaultHandle, SMART_IV_NVM3_KEY_TARGETS, &rec, sizeof(rec)) != ECODE_NVM3_OK
      || rec.magic != SMART_IV_TARGETS_MAGIC) {
    printf("[NVM3] Saved targets unreadable/foreign - keeping defaults\r\n");
    return;
  }

  /* Sanity-check before trusting flash contents: a corrupted record must
   * never be able to push a nonsense prescription into the AI's ratio math
   * (or divide it by zero). */
  if (rec.flow_ml_h > 0.0f && rec.flow_ml_h < 10000.0f) {
    target_flow_ml_h = rec.flow_ml_h;
  }
  if (rec.drops_per_min > 0.0f && rec.drops_per_min < 1000.0f) {
    target_drops_per_min = rec.drops_per_min;
  }

  printf("[NVM3] Restored the doctor's targets: %d ml/h, %d dpm\r\n",
         (int)(target_flow_ml_h + 0.5f), (int)(target_drops_per_min + 0.5f));
}

/* ---- Tare button (onboard BTN0, BRD2709A: port B pin 0) -----
 * Lets the doctor re-zero the scale without power-cycling the board: press
 * BTN0 with the scale empty, wait for the "tare done" indication, then hang
 * the new IV bag. Internally this just resets the same tare state machine
 * hx711_poll() already runs once automatically at boot. */
#define TARE_BTN_PORT          gpioPortB
#define TARE_BTN_PIN           0
#define TARE_BTN_DEBOUNCE_MS   250U

static bool     tare_btn_prev_pressed    = false;
static uint32_t tare_btn_last_change_ms  = 0;

/* One-shot pulse, consumed by app.c's periodic report: true for exactly one
 * report cycle right after a tare (button-triggered or the initial
 * power-on tare) finishes, so the doctor/nurse sees a clear "tare done"
 * event instead of having to infer it from the weight settling at 0. */
static bool tare_just_completed = false;

/* Persistent counter, incremented once per completed tare and NEVER reset -
 * unlike the one-shot pulse above, this can't be missed by a race where the
 * tare finishes (often within a few seconds of boot) before Zigbee
 * reporting has even been configured (which only happens after the network
 * join completes, and can occasionally take longer than the tare itself).
 * The HIS Server detects "count increased since last seen" instead of
 * having to catch a transient bit at exactly the right instant, and stamps
 * its own wall-clock "last tared at" timestamp when it does. */
static uint8_t tare_event_count = 0;

static void hx711_gpio_init(void)
{
  GPIO_PinModeSet(HX711_SCK_PORT, HX711_SCK_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(HX711_DOUT_PORT, HX711_DOUT_PIN, gpioModeInput, 0);
  /* Pull-up: BTN0 ties the pin to GND when pressed (idle = HIGH), matching
   * the board's standard button wiring. */
  GPIO_PinModeSet(TARE_BTN_PORT, TARE_BTN_PIN, gpioModeInputPullFilter, 1);
}

/* Re-arms the exact same tare state machine hx711_poll() already runs once
 * at boot - it will print "Please keep the scale EMPTY..." and average
 * HX711_TARE_SAMPLES readings again, exactly like the initial power-on
 * tare. Shared by the physical BTN0 press and a remote "reset scale"
 * command from the HIS Server (see sh_flow_trigger_tare() / app.c's
 * post_attribute_change_cb()). */
static void hx711_trigger_tare(const char *source)
{
  hx711_inited       = false;
  hx711_tare_started = false;
  hx711_tare_accum   = 0;
  hx711_tare_count   = 0;
  hx711_have_pending = false;
  hx711_have_sample  = false;
  printf("[HX711] Tare triggered (%s) - remove any load, re-zeroing the scale...\r\n", source);
}

static void tare_button_poll(void)
{
  uint32_t now = now_ms();
  bool pressed = (GPIO_PinInGet(TARE_BTN_PORT, TARE_BTN_PIN) == 0);

  if (pressed && !tare_btn_prev_pressed
      && (now - tare_btn_last_change_ms) >= TARE_BTN_DEBOUNCE_MS) {
    tare_btn_last_change_ms = now;
    hx711_trigger_tare("BTN0 pressed");
  }
  tare_btn_prev_pressed = pressed;
}

static bool hx711_is_ready(void)
{
  /* The HX711 pulls DOUT LOW once a new sample is ready to be read. */
  return GPIO_PinInGet(HX711_DOUT_PORT, HX711_DOUT_PIN) == 0;
}

/* Reads one raw 24-bit (signed) sample, then sends 1 extra pulse to
 * re-select channel A/gain 128. IMPORTANT: this whole sequence MUST run
 * with interrupts disabled (atomic section) - mirroring exactly what the
 * original Arduino HX711 library (bogde/HX711) does by wrapping
 * shiftIn() in noInterrupts()/interrupts(). Reason: if SCK is held at
 * either level for more than ~60us (e.g. because another interrupt - the
 * Zigbee radio stack - preempts partway through the 24 bits), the HX711
 * automatically enters power-down mode MID-READ, corrupting the sample
 * (part of it is bits from the old reading, the rest is garbage) - NOT a
 * clean 24-bit read failure, but a subtly-wrong value, which is exactly
 * what explains the previously observed symptom: the weight jittering by
 * tens of grams even while the load was stable (not an obvious "read
 * failure" that would be easy to spot, but a "close-but-wrong read" that's
 * very hard to detect without knowing to look for it in advance). This
 * step was previously missing. */
static int32_t hx711_read_raw(void)
{
  int32_t value = 0;
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_ATOMIC();

  /* IMPORTANT: this used to be an empty loop counting iterations (not timed
   * against a real clock) to hold each SCK pulse - the same class of bug
   * hit with the MAX30102 I2C code: under -Os optimization, a loop of ~4
   * iterations takes only a few tens of NANOseconds, far faster than the
   * HX711 can shift a bit out, leaving DOUT "frozen" reading the same fixed
   * value forever (weight always = 0 no matter what's on the scale). Using
   * sl_udelay_wait() (calibrated against the real CPU clock) gives a true
   * ~1us per half-cycle - comfortably above the ~0.2us minimum the HX711
   * requires, while staying far below the ~50-60us threshold that would
   * make the chip enter power-down on its own. */
  for (int i = 0; i < 24; i++) {
    GPIO_PinOutSet(HX711_SCK_PORT, HX711_SCK_PIN);
    sl_udelay_wait(1);
    value = (value << 1) | (GPIO_PinInGet(HX711_DOUT_PORT, HX711_DOUT_PIN) ? 1 : 0);
    GPIO_PinOutClear(HX711_SCK_PORT, HX711_SCK_PIN);
    sl_udelay_wait(1);
  }

  /* 25th pulse: selects channel A, gain 128 for the next reading (matches hx711_test.ino) */
  GPIO_PinOutSet(HX711_SCK_PORT, HX711_SCK_PIN);
  sl_udelay_wait(1);
  GPIO_PinOutClear(HX711_SCK_PORT, HX711_SCK_PIN);

  CORE_EXIT_ATOMIC();

  if (value & 0x00800000) {
    value = (int32_t)((uint32_t)value | 0xFF000000U);   /* sign-extend 24-bit -> 32-bit */
  }
  return value;
}

static void hx711_poll(void)
{
#if FLOW_ENABLED
  uint32_t now0 = now_ms();
  bool ready_now = hx711_is_ready();

  if (!hx711_found) {
    /* Step 1: confirm a REAL HX711 chip is present before trusting any
     * value at all.
     *
     * BUG FIXED (the real reason HX711 was never detected, EVEN WITH
     * WIRING 100% CORRECT): the previous version only READ the DOUT level
     * (GPIO_PinInGet) but NEVER pulsed SCK while not yet "found". Per the
     * HX711 protocol: once a sample finishes converting, DOUT pulls LOW
     * and STAYS there FOREVER until the host sends >=25 SCK pulses to both
     * read the sample AND "unlock" the next conversion. If SCK is never
     * pulsed (as the old "detect" phase did), even a real chip will only
     * pull DOUT low ONCE (~100ms after power-up) and then latch there
     * permanently - which exactly explains the symptom observed: the log
     * always printed "DOUT reading level 0" on every report, never once
     * showing a new edge, regardless of whether the GND was reconnected or
     * the pin was swapped between PC01<->PC02 (because this was never a
     * wiring problem - it was an algorithm that could never "unlock" the
     * chip so it would convert a second sample).
     *
     * Fix: EVERY time DOUT is observed ready (ready_now), ACTIVELY pulse
     * SCK right away (call hx711_read_raw() - this both reads the data and
     * "unlocks" the chip to run its next conversion), then use the
     * interval BETWEEN CONSECUTIVE REAL READS to confirm a regular cycle,
     * exactly how a real Arduino HX711 library operates (is_ready() and
     * read() always go together). */
    if (!ready_now) {
      if (now0 - hx711_last_report_ms >= HX711_DETECT_REPORT_MS) {
        hx711_last_report_ms = now0;
        printf("[HX711] Real HX711 module not yet confirmed - DOUT reading level %d "
               "but with no stable cycle (possibly a floating pin). "
               "Check the DT/SCK wiring and the module's VCC/GND supply.\r\n",
               GPIO_PinInGet(HX711_DOUT_PORT, HX711_DOUT_PIN));
      }
      return;   /* NOT ready yet -> nothing to clock out, wait for the next poll */
    }

    /* ready_now == true: pulse SCK RIGHT NOW (both samples the data and
     * "unlocks" the chip to run its next conversion) - this is the step
     * that was missing before. */
    (void)hx711_read_raw();

    if (hx711_last_ready_ms != 0) {
      uint32_t interval = now0 - hx711_last_ready_ms;
      if (interval >= HX711_DETECT_INTERVAL_MIN_MS && interval <= HX711_DETECT_INTERVAL_MAX_MS) {
        hx711_good_streak++;
      } else {
        hx711_good_streak = 0;   /* interval out of range - looks like noise, restart the streak */
      }
      if (hx711_good_streak >= HX711_DETECT_STREAK_NEEDED) {
        hx711_found = true;
        printf("[HX711] REAL HX711 module DETECTED (DOUT signals ready on a regular ~%lums cycle)\r\n",
               (unsigned long)interval);
      }
    }
    hx711_last_ready_ms = now0;
    return;   /* the sample used just to "unlock" the chip was discarded during
               * the detect phase - real reads start fresh once found */
  }

  if (!ready_now) {
    return;   /* no new sample yet -> don't read, don't block the main loop */
  }

  int32_t raw_candidate = hx711_read_raw();
  uint32_t now = now0;

  /* "Two-reads-must-agree" filter: some reads get BIT-SHIFTED during the
   * 24-bit shift (because the read start time isn't perfectly synced with
   * the HX711 yet), producing a value roughly ~2^n times the real value
   * (e.g. a real 379g read as 758g, 1517g, 47935g...) - easy to spot
   * because this is a ONE-OFF error that does NOT repeat identically on
   * the very next read. Only accept a sample once it closely matches the
   * immediately preceding one (within tolerance), i.e. it has been
   * "confirmed" - this filters out virtually all bit-shifted reads without
   * needing to know the exact electrical/mechanical root cause in advance. */
  if (!hx711_have_pending) {
    hx711_pending_raw   = raw_candidate;
    hx711_have_pending   = true;
    return;   /* wait for the next read to confirm */
  }

  int32_t diff = raw_candidate - hx711_pending_raw;
  if (diff < 0) diff = -diff;
  if (diff > HX711_CONFIRM_TOLERANCE_COUNTS) {
    /* Doesn't match the previous sample -> suspect the previous sample OR
     * this one is bad. Store the new sample as the next "candidate" and
     * wait for confirmation on the following read. */
    hx711_pending_raw = raw_candidate;
    return;
  }

  /* Match -> accept, average the 2 samples for extra stability. */
  int32_t raw = (raw_candidate + hx711_pending_raw) / 2;
  hx711_have_pending = false;

  if (!hx711_inited) {
    /* Just like a real electronic scale: the scale MUST be empty at
     * power-on; wait for taring to finish (averaging 30 samples, matching
     * scale.tare(30) in hx711_test.ino) before announcing "taring done" -
     * only then does the user hang the IV bag on the scale. */
    if (!hx711_tare_started) {
      hx711_tare_started = true;
      printf("[HX711] Please keep the scale EMPTY, taring now...\r\n");
    }

    hx711_tare_accum += raw;
    hx711_tare_count++;
    if (hx711_tare_count >= HX711_TARE_SAMPLES) {
      hx711_tare_offset = (int32_t)(hx711_tare_accum / (int64_t)hx711_tare_count);
      hx711_inited = true;
      tare_just_completed = true;
      tare_event_count++;   // wraps at 255->0, harmless: the server only checks "did it change"
      /* Reset the flow-rate anchor to the fresh zero, not whatever weight
       * was last recorded before a mid-operation retare - otherwise the
       * next flow computation would see a huge bogus "drop" from the old
       * (now stale) anchor weight down to the freshly-zeroed scale. */
      flow_anchor_ms = now;
      flow_anchor_weight_g = 0.0f;
      printf("[HX711] Taring done! Scale is now at 0g. You may hang the IV bag on the scale.\r\n");
    }
    return;
  }

  float weight_g = (float)(raw - hx711_tare_offset) / HX711_CALIBRATION_FACTOR * 1000.0f;

  /* EMA filter to reduce the load cell's inherent mechanical jitter */
  const float alpha = 0.2f;
  hx711_weight_g = hx711_have_sample
      ? (hx711_weight_g * (1.0f - alpha) + weight_g * alpha)
      : weight_g;
  hx711_have_sample   = true;
  hx711_last_valid_ms = now;

  /* Periodically print the weight to serial (matching the cadence + the
   * +-15g zero-band filter of hx711_test.ino) for visual sanity checking -
   * unrelated to the flow-rate computation. */
  if (now - hx711_last_print_ms >= HX711_PRINT_INTERVAL_MS) {
    hx711_last_print_ms = now;
    float disp_g = hx711_weight_g;
    if (disp_g < HX711_ZERO_FILTER_G && disp_g > -HX711_ZERO_FILTER_G) {
      disp_g = 0.0f;
    }
    int wg = (int)(disp_g + (disp_g >= 0.0f ? 0.5f : -0.5f));
    int kg_whole = wg / 1000;
    int kg_frac  = wg % 1000;
    if (kg_frac < 0) kg_frac = -kg_frac;
    printf("[HX711] IV bag weight: %d.%03d kg (%d g)\r\n", kg_whole, kg_frac, wg);
  }

  if (flow_anchor_ms == 0) {
    flow_anchor_ms = now;
    flow_anchor_weight_g = hx711_weight_g;
    return;
  }

  if (now - flow_anchor_ms >= FLOW_CALC_INTERVAL_MS) {
    float delta_g = flow_anchor_weight_g - hx711_weight_g;   /* decrease = fluid that has flowed out */
    float delta_h = (float)(now - flow_anchor_ms) / 3600000.0f;
    if (delta_h > 0.0f) {
      /* Assume infusion fluid density ~1 g/ml (dilute saline/glucose solution) */
      float computed = delta_g / delta_h;

      /* Clamp to the physically possible range. Without it, one noisy load-cell
       * reading (loose wire, someone bumping the stand, or no load attached at
       * all) produces a delta_g of thousands of grams, sending flow_ml_per_h
       * into the millions. That in turn made sh_flow_ratio() report "3367972%"
       * and OVERFLOWED the autoencoder's reconstruction error - observed for
       * real on the chip as err = 2147483647 (INT32_MAX). No infusion regimen
       * exceeds 2000 ml/h, so anything past that is certainly noise rather than
       * a genuine measurement. */
      if (computed < 0.0f)    computed = 0.0f;
      if (computed > 2000.0f) computed = 2000.0f;
      flow_ml_per_h = computed;
    }
    flow_anchor_ms = now;
    flow_anchor_weight_g = hx711_weight_g;
  }
#endif
}

/* ============================================================================
 *  DFRobot Gravity MAX30102 module (SEN0344) — I2C via pure GPIO bit-banging,
 *  NOT using the SDK's I2CSPM driver. Reason: that driver (sl_i2cspm.c) has
 *  a FIXED 300-SECOND timeout per transaction — if the bus ever gets "stuck"
 *  (e.g. the module clock-stretches too long while busy), the ENTIRE SYSTEM
 *  (including the drop channel, which is otherwise running fine, and even
 *  Zigbee) would freeze for up to 5 minutes PER OCCURRENCE — unacceptable
 *  for a medical monitoring device. A dedicated, minimal I2C layer is
 *  written here instead to keep the timeout genuinely short (tens of ms at
 *  most), the same approach already used above for the HX711 (FLOW channel).
 *
 *  Command protocol (register addresses) taken from the actual source of
 *  the DFRobot_BloodOxygen_S library (repo DFRobot/DFRobot_BloodOxygen_S):
 *    - I2C address: 0x57
 *    - Start measuring: write 2 bytes {0x00, 0x01} to "register" 0x20
 *    - Read result: read 8 bytes from "register" 0x0C ->
 *        byte[0]      = SpO2 (%)
 *        byte[2..5]   = Heartbeat (bpm), packed as big-endian 32-bit
 *      The module refreshes this result roughly every 4 seconds on its own.
 * ========================================================================== */
#define BLOODOX_I2C_ADDR      0x57
#define BLOODOX_REG_START     0x20
#define BLOODOX_REG_RESULT    0x0C
#define BLOODOX_RESULT_LEN    8

/* I2C pins (shared mikroBUS/Qwiic bus on BRD2709A): SCL=PC05, SDA=PC07 */
#define I2CBB_SCL_PORT  gpioPortC
#define I2CBB_SCL_PIN   5
#define I2CBB_SDA_PORT  gpioPortC
#define I2CBB_SDA_PIN   7

/* Loop-iteration cap for EACH step waiting on the line to go high
 * (clock-stretching, or SDA held low) — NOT using sl_sleeptimer (this has
 * to be a busy-wait loop so it can exit the instant the bus recovers,
 * without oversleeping). This number is an "iteration count", not a direct
 * unit of time, but it's chosen generously enough to allow ~20-30ms before
 * giving up — VASTLY safer than the SDK driver's 300 seconds. */
#define I2CBB_STRETCH_LOOP_MAX  20000

/* IMPORTANT: this function used to be an empty loop counting iterations
 * (not timed against a real clock), so under -Os optimization it actually
 * took only ~a few tens of NANOseconds - hundreds of times faster than
 * intended. The I2C pulses were driven far too fast, causing the ACK-bit
 * read right after releasing SCL high to happen EARLIER than the DFRobot
 * module's own internal MCU could pull SDA low to signal ACK -> repeatedly
 * misread as "no ACK" even though the module was present and functioning
 * normally (a real I2CSPM hardware driver running at the correct 100kHz
 * would have caught it fine). Using sl_udelay_wait() (calibrated against
 * the real CPU clock, via the "udelay" component already included in the
 * project) gives a true ~5us per step, matching standard 100kHz I2C speed. */
static void i2cbb_delay(void)
{
  sl_udelay_wait(5);
}

static void i2cbb_scl_release(void)  { GPIO_PinModeSet(I2CBB_SCL_PORT, I2CBB_SCL_PIN, gpioModeWiredAndPullUp, 1); }
static void i2cbb_scl_low(void)      { GPIO_PinModeSet(I2CBB_SCL_PORT, I2CBB_SCL_PIN, gpioModeWiredAndPullUp, 0); }
static void i2cbb_sda_release(void)  { GPIO_PinModeSet(I2CBB_SDA_PORT, I2CBB_SDA_PIN, gpioModeWiredAndPullUp, 1); }
static void i2cbb_sda_low(void)      { GPIO_PinModeSet(I2CBB_SDA_PORT, I2CBB_SDA_PIN, gpioModeWiredAndPullUp, 0); }
static int  i2cbb_sda_read(void)     { return GPIO_PinInGet(I2CBB_SDA_PORT, I2CBB_SDA_PIN); }
static int  i2cbb_scl_read(void)     { return GPIO_PinInGet(I2CBB_SCL_PORT, I2CBB_SCL_PIN); }

/* Release SCL to high and WAIT (with a loop cap) in case the slave is
 * "clock-stretching" (holding SCL low to ask for more processing time).
 * Returns false if the cap is exceeded — the caller reports a "bus error",
 * NEVER sleeping forever like the original driver does. */
static bool i2cbb_scl_release_wait(void)
{
  i2cbb_scl_release();
  for (int i = 0; i < I2CBB_STRETCH_LOOP_MAX; i++) {
    if (i2cbb_scl_read()) { return true; }
  }
  return false;
}

static void i2cbb_init_pins(void)
{
  i2cbb_scl_release();
  i2cbb_sda_release();
}

/* The bus used to be brought up only by max30102_init(), i.e. only when the
 * HR/SpO2 channels are compiled in. The OLED shares these pins, so it would
 * have driven an uninitialised bus the moment someone set HR_ENABLED to 0 to
 * work on another channel - the screen going blank for a reason nowhere near
 * the code being changed. Every entry point into the bus brings it up. */
static bool i2cbb_pins_ready = false;

static void i2cbb_ensure_pins(void)
{
  if (!i2cbb_pins_ready) {
    i2cbb_init_pins();
    i2cbb_pins_ready = true;
  }
}

static void i2cbb_start(void)
{
  i2cbb_sda_release();
  i2cbb_scl_release_wait();
  i2cbb_delay();
  i2cbb_sda_low();
  i2cbb_delay();
  i2cbb_scl_low();
}

static void i2cbb_stop(void)
{
  i2cbb_sda_low();
  i2cbb_delay();
  i2cbb_scl_release_wait();
  i2cbb_delay();
  i2cbb_sda_release();
  i2cbb_delay();
}

/* Writes 1 byte, returns true if the slave ACKs (pulls SDA low on the 9th pulse) */
static bool i2cbb_write_byte(uint8_t b)
{
  for (int i = 0; i < 8; i++) {
    if (b & 0x80) { i2cbb_sda_release(); } else { i2cbb_sda_low(); }
    b = (uint8_t)(b << 1);
    i2cbb_delay();
    if (!i2cbb_scl_release_wait()) { return false; }   /* bus error/stuck - give up immediately, do NOT wait 300s */
    i2cbb_delay();
    i2cbb_scl_low();
  }
  /* 9th pulse: release SDA to read the ACK from the slave */
  i2cbb_sda_release();
  i2cbb_delay();
  if (!i2cbb_scl_release_wait()) { return false; }
  bool ack = (i2cbb_sda_read() == 0);
  i2cbb_delay();
  i2cbb_scl_low();
  return ack;
}

static bool i2cbb_read_byte(uint8_t *out, bool send_ack)
{
  uint8_t val = 0;
  i2cbb_sda_release();
  for (int i = 0; i < 8; i++) {
    i2cbb_delay();
    if (!i2cbb_scl_release_wait()) { return false; }
    val = (uint8_t)((val << 1) | (i2cbb_sda_read() ? 1 : 0));
    i2cbb_scl_low();
  }
  if (send_ack) { i2cbb_sda_low(); } else { i2cbb_sda_release(); }
  i2cbb_delay();
  if (!i2cbb_scl_release_wait()) { return false; }
  i2cbb_delay();
  i2cbb_scl_low();
  i2cbb_sda_release();
  *out = val;
  return true;
}

/* Write to ANY device on this bus: START, ADDR+W, first_byte, data..., STOP.
 *
 * Exported (sh_i2c_write) so the bedside OLED can share the MAX30102's two
 * wires without a second bit-bang implementation. The bus has exactly one
 * owner - this file - because two independent implementations toggling PC05/
 * PC07 would eventually interleave and leave a half-finished transaction on
 * the wires, which shows up as an occasional garbled reading rather than an
 * obvious failure.
 *
 * `first_byte` is the register for a sensor, or the control byte (0x00 =
 * command, 0x40 = display RAM) for the OLED - the same position in the frame
 * either way. */
static bool i2cbb_write_to(uint8_t address, uint8_t first_byte,
                           const uint8_t *data, uint8_t len)
{
  i2cbb_ensure_pins();
  i2cbb_start();
  bool ok = i2cbb_write_byte((uint8_t)(address << 1));
  ok = ok && i2cbb_write_byte(first_byte);
  for (uint8_t i = 0; ok && i < len; i++) {
    ok = i2cbb_write_byte(data[i]);
  }
  i2cbb_stop();
  return ok;
}

bool sh_i2c_write(uint8_t address, uint8_t first_byte,
                  const uint8_t *data, uint8_t len)
{
  return i2cbb_write_to(address, first_byte, data, len);
}

/* Write: START, ADDR+W, reg, data..., STOP (matches DFRobot's writeReg()) */
static bool bloodox_write(uint8_t reg, const uint8_t *data, uint8_t len)
{
  return i2cbb_write_to(BLOODOX_I2C_ADDR, reg, data, len);
}

/* Read: 2 separate transactions (STOP in between) — matches how DFRobot's
 * readReg() works (separate Wire.endTransmission() + Wire.requestFrom(),
 * not a combined repeated-start). */
static bool bloodox_read(uint8_t reg, uint8_t *data, uint8_t len)
{
  i2cbb_start();
  bool ok = i2cbb_write_byte((uint8_t)(BLOODOX_I2C_ADDR << 1));
  ok = ok && i2cbb_write_byte(reg);
  i2cbb_stop();
  if (!ok) { return false; }

  i2cbb_start();
  ok = i2cbb_write_byte((uint8_t)((BLOODOX_I2C_ADDR << 1) | 0x01));
  for (uint8_t i = 0; ok && i < len; i++) {
    ok = i2cbb_read_byte(&data[i], i < (uint8_t)(len - 1));
  }
  i2cbb_stop();
  return ok;
}

static bool     max30102_inited         = false;
static uint32_t max30102_last_sample_ms = 0;
static uint32_t max30102_last_probe_ms  = 0;
static float    hr_bpm   = 0.0f;
static float    spo2_pct = 0.0f;
static bool     hr_valid = false;

/* Interval between retries to re-probe the module if it wasn't found last
 * time - the module might boot slower than expected after power-up, or the
 * bus may have had a momentary glitch; retry periodically (non-blocking)
 * instead of giving up permanently. */
#define MAX30102_PROBE_INTERVAL_MS  2000U

/* ---- Sampling cadence: matches the original .ino reference exactly -----
 * Read the module once every 0.5s; after 4 attempts (2 seconds), publish
 * the average of whichever reads came back in a plausible range (the value
 * "closest to the others") instead of overwriting HR/SpO2 on every single
 * raw read. This filters out the occasional spurious reading without
 * throwing away a whole window just because one read was noisy. */
#define MAX30102_SAMPLE_INTERVAL_MS  500U
#define MAX30102_SAMPLE_COUNT        4U

/* Plausibility range for a raw reading, same bounds as the .ino's
 * isValidHeartRate()/isValidSpO2() - readings outside this range are
 * discarded rather than averaged in. */
#define MAX30102_HR_MIN    30
#define MAX30102_HR_MAX   240
#define MAX30102_SPO2_MIN  70
#define MAX30102_SPO2_MAX 100

static uint32_t max30102_last_sample_attempt_ms = 0;
static uint8_t  max30102_attempt_count          = 0;
static int32_t  max30102_hr_samples[MAX30102_SAMPLE_COUNT];
static int32_t  max30102_spo2_samples[MAX30102_SAMPLE_COUNT];
static uint8_t  max30102_hr_valid_count   = 0;
static uint8_t  max30102_spo2_valid_count = 0;

/* Picks the ACTUAL sample (not a synthetic average) that sits closest to
 * all the others in the set - the one minimizing the total distance to
 * every other sample. With a single sample it's the obvious choice; with
 * ties, the first one found wins. This guarantees the value sent to the
 * server was a real reading the sensor produced, not a rounded blend. */
static int32_t pick_closest_to_others(const int32_t *values, uint8_t count)
{
  int32_t best_value = values[0];
  int32_t best_score  = -1;

  for (uint8_t i = 0; i < count; i++) {
    int32_t score = 0;
    for (uint8_t j = 0; j < count; j++) {
      int32_t diff = values[i] - values[j];
      if (diff < 0) diff = -diff;
      score += diff;
    }
    if (best_score < 0 || score < best_score) {
      best_score = score;
      best_value = values[i];
    }
  }
  return best_value;
}

/* begin(): the original DFRobot library pings with a 0-byte WRITE, but here
 * we instead read 1 byte to check for an ACK on the address as an
 * equivalent substitute (simpler, and protocol-equivalent — the slave
 * ACKing its address is all that's needed to know it's present). */
static bool bloodox_ping(void)
{
  uint8_t dummy = 0;
  return bloodox_read(BLOODOX_REG_RESULT, &dummy, 1);
}

/* Try once (won't block for long): if it ACKs, send the start-measuring
 * command and mark it initialized. Safe to call any number of times - used
 * for both the initial boot-time attempt and the periodic retries inside
 * poll(). */
static bool max30102_try_find(void)
{
  if (!bloodox_ping()) {
    return false;
  }

  uint8_t start_cmd[2] = { 0x00, 0x01 };
  bloodox_write(BLOODOX_REG_START, start_cmd, 2);
  /* Do not block here for ~4s like the original .ino did (that would delay
   * the first Zigbee join) — max30102_poll() will naturally pick up a
   * valid value once the module is ready (returns spo2/heartbeat > 0);
   * until then sh_hr_state()/sh_spo2_state() simply report CH_LOST as
   * usual, which is fine. */

  max30102_inited = true;
  printf("[BloodOx] DFRobot MAX30102 module initialized successfully (I2C bit-bang)\r\n");
  return true;
}

static void max30102_init(void)
{
#if HR_ENABLED || SPO2_ENABLED
  i2cbb_ensure_pins();

  /* The module has its own onboard MCU and may need a few hundred ms to
   * boot after power-up (like any sensor module with its own processor).
   * Retry a few times right at boot instead of giving up on the first
   * ping. Each attempt is capped at a few tens of ms at most
   * (I2CBB_STRETCH_LOOP_MAX), NOT 300 seconds like the old SDK driver — so
   * even this whole 6-attempt loop stays fast even if the bus is faulty. */
  bool found = false;
  for (int attempt = 0; attempt < 6 && !found; attempt++) {
    found = max30102_try_find();
    if (!found) {
      sl_sleeptimer_delay_millisecond(300);
    }
  }

  if (!found) {
    printf("[BloodOx] Module not found on I2C (address 0x57 did not ACK after 6 attempts at "
           "boot) - will AUTOMATICALLY RETRY periodically every %us; check the 3V3/GND supply "
           "and the SDA/SCL wiring if it's still not found after a few attempts\r\n", MAX30102_PROBE_INTERVAL_MS / 1000U);
    /* Do NOT return early: max30102_inited stays false, and
     * sh_hr_state()/sh_spo2_state() will report CH_LOST until
     * max30102_poll() finds it and turns it on by itself. */
  }
#endif
}

static void max30102_poll(void)
{
#if HR_ENABLED || SPO2_ENABLED
  if (!max30102_inited) {
    /* Module has never ACKed - automatically retry periodically (without
     * blocking the main loop), instead of giving up forever after the 6
     * boot-time attempts. */
    uint32_t now = now_ms();
    if (now - max30102_last_probe_ms >= MAX30102_PROBE_INTERVAL_MS) {
      max30102_last_probe_ms = now;
      max30102_try_find();
    }
    return;
  }

  /* Sample once every 0.5s, exactly like the .ino reference - reading on
   * every single main-loop tick (as before) is both unnecessary (the module
   * itself only refreshes its internal result ~every 4s) and defeats the
   * point of averaging several reads together. */
  uint32_t now = now_ms();
  if (now - max30102_last_sample_attempt_ms < MAX30102_SAMPLE_INTERVAL_MS) {
    return;
  }
  max30102_last_sample_attempt_ms = now;

  uint8_t buf[BLOODOX_RESULT_LEN];
  if (bloodox_read(BLOODOX_REG_RESULT, buf, BLOODOX_RESULT_LEN)) {
    int     spo2_raw      = buf[0];
    int32_t heartbeat_raw = ((int32_t)buf[2] << 24) | ((int32_t)buf[3] << 16)
                           | ((int32_t)buf[4] << 8)  | (int32_t)buf[5];

    /* Discard obviously implausible reads (no finger, not enough data yet,
     * or a corrupted transaction) instead of letting them drag the average
     * off - same bounds as the .ino's isValidHeartRate()/isValidSpO2(). */
    if (heartbeat_raw >= MAX30102_HR_MIN && heartbeat_raw <= MAX30102_HR_MAX
        && max30102_hr_valid_count < MAX30102_SAMPLE_COUNT) {
      max30102_hr_samples[max30102_hr_valid_count++] = heartbeat_raw;
    }
    if (spo2_raw >= MAX30102_SPO2_MIN && spo2_raw <= MAX30102_SPO2_MAX
        && max30102_spo2_valid_count < MAX30102_SAMPLE_COUNT) {
      max30102_spo2_samples[max30102_spo2_valid_count++] = spo2_raw;
    }
  }
  /* A transient bus error just means this attempt contributes no sample -
   * it still counts toward the 2-second window, same as the .ino. */

  max30102_attempt_count++;
  if (max30102_attempt_count < MAX30102_SAMPLE_COUNT) {
    return;   /* window not finished yet (4 attempts = ~2s) */
  }
  max30102_attempt_count = 0;

  /* Publish the single sample from this window that sits closest to the
   * others (not an average) - and reset the buffers for the next 2-second
   * window. If NO sample was valid this window, keep the previous published
   * value and let VITAL_TIMEOUT_MS/sh_*_state() decide when the signal has
   * truly been lost for good, rather than flickering to "no data" every 2
   * seconds. */
  if (max30102_hr_valid_count > 0) {
    hr_bpm   = (float)pick_closest_to_others(max30102_hr_samples, max30102_hr_valid_count);
    hr_valid = true;
    max30102_last_sample_ms = now;
  }
  if (max30102_spo2_valid_count > 0) {
    spo2_pct = (float)pick_closest_to_others(max30102_spo2_samples, max30102_spo2_valid_count);
    max30102_last_sample_ms = now;
  }
  max30102_hr_valid_count   = 0;
  max30102_spo2_valid_count = 0;
#endif
}

/* ============================================================================
 *  Alarm indicators — 3 LEDs + buzzer (see the wiring comment in sensor_hub.h)
 *
 *  Ported from the ESP8266 reference sketch
 *  (he_thong_giam_sat_dich_truyen_3in1_canh_bao.ino), keeping its exact
 *  behavior: exactly ONE LED lit at a time (green/yellow/red), and the buzzer
 *  beeping intermittently - fast (300ms) on RED, slow (1000ms) on YELLOW,
 *  silent on GREEN.
 *
 *  Difference vs. the .ino: the beep toggling there ran inside the Arduino
 *  loop(); here it runs from sh_alert_poll() (called every sensor_hub_poll()),
 *  so it stays fully NON-BLOCKING - no delay() may ever be used, since the
 *  Zigbee stack and the drop sensor both need the main loop to keep turning.
 * ========================================================================== */
#define ALERT_ACTIVE_HIGH   1     /* Governs the 3 LEDs. 0 if your LED modules are active-low. */
#define BUZZER_ACTIVE_HIGH  0     /* The buzzer module is wired active-LOW - it sounds when the
                                   * pin is pulled LOW, opposite of the LEDs. Kept as its own
                                   * macro instead of sharing ALERT_ACTIVE_HIGH, since the two
                                   * polarities differ on this board. */

#define LED_GREEN_PORT   gpioPortA
#define LED_GREEN_PIN    7        /* mikroBUS PWM = PA07 */
#define LED_YELLOW_PORT  gpioPortA
#define LED_YELLOW_PIN   4        /* mikroBUS TX  = PA04 */
#define LED_RED_PORT     gpioPortA
#define LED_RED_PIN      5        /* mikroBUS RX  = PA05 */
#define BUZZER_PORT      gpioPortC
#define BUZZER_PIN_NUM   6        /* PC06 - moved off CS/PC04, which is no
                                   * longer used for anything. NOTE: this
                                   * does not match the "RST = PD03" entry in
                                   * the mikroBUS pinout table below,
                                   * confirmed intentional. */

/* --- No two peripherals may share a pin ------------------------------------
 *
 * This project has lost time to pin confusion more than once, and the failure
 * is always silent: the pin gets configured by whichever driver initialises
 * last, and the other peripheral simply never works. A buzzer wired to the
 * load cell's data line is exactly this - the pin is held as an INPUT for the
 * scale, so nothing ever drives the buzzer, and it stays quiet with no error
 * anywhere.
 *
 * The mikroBUS header on BRD2709A, so the names on the silkscreen can be
 * checked against the numbers here without opening the SDK:
 *
 *     AN  = PD02      PWM  = PA07
 *     RST = PD03      INT  = PA06
 *     CS  = PC04      RX   = PA05
 *     SCK = PC03      TX   = PA04
 *     MISO= PC01      SCL  = PC05
 *     MOSI= PC02      SDA  = PC07
 *
 * MISO/MOSI ở đây lấy theo SƠ ĐỒ CHÂN CỦA BOARD, và nó NGƯỢC với cách đọc
 * "SPI master thì TX = MOSI" từ sl_spidrv_usart_mikroe_config.h (file đó ghi
 * TX = PC01, RX = PC02). Đã suy luận sai một lần theo hướng kia rồi: SDK chỉ
 * nói TX/RX, không nói MOSI/MISO, nên đừng suy ra — tra sơ đồ chân.
 *
 * A collision now fails the build with the two names in the message, instead
 * of failing on a bench with neither. */
#define SH_PIN_ID(port, pin)  ((int)(port) * 32 + (int)(pin))

#define SH_PINS_DIFFER(a_port, a_pin, b_port, b_pin) \
  (SH_PIN_ID(a_port, a_pin) != SH_PIN_ID(b_port, b_pin))

_Static_assert(SH_PINS_DIFFER(BUZZER_PORT, BUZZER_PIN_NUM,
                              HX711_DOUT_PORT, HX711_DOUT_PIN),
               "Buzzer and HX711 DOUT are on the same pin. On BRD2709A the "
               "load cell data line is PC01 = mikroBUS MISO; the buzzer is on "
               "PC06. Move the wire, or move the scale.");
_Static_assert(SH_PINS_DIFFER(BUZZER_PORT, BUZZER_PIN_NUM,
                              HX711_SCK_PORT, HX711_SCK_PIN),
               "Buzzer and HX711 SCK are on the same pin (PC03 = mikroBUS SCK).");
_Static_assert(SH_PINS_DIFFER(BUZZER_PORT, BUZZER_PIN_NUM, SENSOR_PORT, SENSOR_PIN),
               "Buzzer and the drop sensor are on the same pin (PD02 = mikroBUS AN).");
_Static_assert(SH_PINS_DIFFER(BUZZER_PORT, BUZZER_PIN_NUM, I2CBB_SCL_PORT, I2CBB_SCL_PIN)
               && SH_PINS_DIFFER(BUZZER_PORT, BUZZER_PIN_NUM, I2CBB_SDA_PORT, I2CBB_SDA_PIN),
               "Buzzer is on an I2C line (PC05/PC07) shared by the OLED and the "
               "MAX30102. Those two may share with each other - I2C is a bus with "
               "addresses - but a buzzer has no address and would be driven by "
               "every transaction on it.");
_Static_assert(SH_PINS_DIFFER(LED_GREEN_PORT, LED_GREEN_PIN,
                              LED_YELLOW_PORT, LED_YELLOW_PIN)
               && SH_PINS_DIFFER(LED_GREEN_PORT, LED_GREEN_PIN,
                                 LED_RED_PORT, LED_RED_PIN)
               && SH_PINS_DIFFER(LED_YELLOW_PORT, LED_YELLOW_PIN,
                                 LED_RED_PORT, LED_RED_PIN),
               "Two alarm lamps are on the same pin.");
_Static_assert(SH_PINS_DIFFER(HX711_DOUT_PORT, HX711_DOUT_PIN,
                              HX711_SCK_PORT, HX711_SCK_PIN),
               "HX711 DOUT and SCK are on the same pin.");

/* --- Còi kêu bằng TẦN SỐ, không phải bằng mức DC -----------------------------
 *
 * Có hai loại còi và chúng cần hai cách lái hoàn toàn khác nhau:
 *
 *   - Còi CHỦ ĐỘNG (active) có mạch dao động bên trong: cấp mức cao là kêu.
 *   - Còi THỤ ĐỘNG (passive) chỉ là một lá áp điện: cấp mức cao thì nó **cong
 *     một cái rồi đứng yên**. Nghe được đúng một tiếng "tách" ở mỗi lần đổi
 *     mức, và với nhịp bíp 0,3-1 giây thì tai người coi như KHÔNG NGHE THẤY GÌ.
 *
 * Trong lúc "đang bíp", chân còi được **đảo mức liên tục** để tạo ra âm thanh
 * thật. Cách này kêu được với **cả hai loại**: còi thụ động phát ra đúng tần
 * số này, còn còi chủ động bị băm nguồn ở tần số đó thì vẫn kêu.
 *
 * 350 µs mỗi nửa chu kỳ ≈ 1,4 kHz — nằm trong dải tai người nhạy nhất và trong
 * vùng cộng hưởng của hầu hết còi áp điện. */
#define BUZZER_TONE_HALF_PERIOD_US  350U

#define BUZZER_PERIOD_CRITICAL_MS 150U    /* CRITICAL: buzzer stays on continuously, only the red LED blinks at this rate */
#define BUZZER_YELLOW_ON_MS       500U    /* LINE_WARNING: short chirp... */
#define BUZZER_YELLOW_OFF_MS     7000U    /* ...then a long silence, so it reads as "pay attention when convenient", not an emergency. */

static alert_level_t alert_level        = ALERT_LEVEL_NORMAL;
static bool          buzzer_on          = false;
static uint32_t      buzzer_last_toggle = 0;

static void alert_pin_write(GPIO_Port_TypeDef port, unsigned int pin, bool on)
{
#if ALERT_ACTIVE_HIGH
  if (on) { GPIO_PinOutSet(port, pin); } else { GPIO_PinOutClear(port, pin); }
#else
  if (on) { GPIO_PinOutClear(port, pin); } else { GPIO_PinOutSet(port, pin); }
#endif
}

/* The buzzer's own polarity (BUZZER_ACTIVE_HIGH, active-LOW) - deliberately
 * not alert_pin_write(), which follows ALERT_ACTIVE_HIGH for the LEDs. The
 * two modules are wired with opposite polarity on this board. */
static void buzzer_write(bool on)
{
#if BUZZER_ACTIVE_HIGH
  if (on) { GPIO_PinOutSet(BUZZER_PORT, BUZZER_PIN_NUM); } else { GPIO_PinOutClear(BUZZER_PORT, BUZZER_PIN_NUM); }
#else
  if (on) { GPIO_PinOutClear(BUZZER_PORT, BUZZER_PIN_NUM); } else { GPIO_PinOutSet(BUZZER_PORT, BUZZER_PIN_NUM); }
#endif
}

/* Reads back the pin's own output register - the ground truth for "is this
 * lamp actually lit right now", independent of alert_level/AI. The buzzer
 * gate below is keyed off this, not off the level, so a green LED and a
 * sounding buzzer can never disagree no matter what upstream logic decided. */
static bool alert_pin_is_on(GPIO_Port_TypeDef port, unsigned int pin)
{
  uint32_t raw = GPIO_PinOutGet(port, pin);
#if ALERT_ACTIVE_HIGH
  return raw != 0U;
#else
  return raw == 0U;
#endif
}

static bool alert_green_led_on(void)
{
  return alert_pin_is_on(LED_GREEN_PORT, LED_GREEN_PIN);
}

static void alert_init(void)
{
  GPIO_PinModeSet(LED_GREEN_PORT,  LED_GREEN_PIN,  gpioModePushPull, 0);
  GPIO_PinModeSet(LED_YELLOW_PORT, LED_YELLOW_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(LED_RED_PORT,    LED_RED_PIN,    gpioModePushPull, 0);
  GPIO_PinModeSet(BUZZER_PORT,     BUZZER_PIN_NUM, gpioModePushPull, 0);

  /* --- Alarm self-test ---------------------------------------------------
   *
   * Each lamp in turn, then a short beep. It costs under a second at boot and
   * answers a question that cannot otherwise be answered from a chair: is the
   * annunciator working, or has this bed simply had nothing to alarm about?
   * A silent alarm and a working-but-quiet ward look identical until the
   * moment it matters.
   *
   * A blocking loop is acceptable HERE and nowhere else in this file: it runs
   * once, before the Zigbee stack is serving and before the drop sensor has
   * anything to miss. */
  alert_pin_write(LED_GREEN_PORT,  LED_GREEN_PIN,  false);
  alert_pin_write(LED_YELLOW_PORT, LED_YELLOW_PIN, false);
  alert_pin_write(LED_RED_PORT,    LED_RED_PIN,    false);
  buzzer_write(false);

  printf("[ALERT] Self-test: green, yellow, red, buzzer...\r\n");

  const struct { GPIO_Port_TypeDef port; unsigned int pin; } lamps[3] = {
    { LED_GREEN_PORT,  LED_GREEN_PIN  },
    { LED_YELLOW_PORT, LED_YELLOW_PIN },
    { LED_RED_PORT,    LED_RED_PIN    },
  };
  for (uint8_t i = 0; i < 3U; i++) {
    alert_pin_write(lamps[i].port, lamps[i].pin, true);
    sl_udelay_wait(200000U);                  /* 200 ms */
    alert_pin_write(lamps[i].port, lamps[i].pin, false);
  }

  printf("[ALERT] Buzzer test: tan so 1.4 kHz\r\n");
  for (uint32_t i = 0; i < 1700U; i++) {          /* ~600 ms ở 1,4 kHz */
    buzzer_write((i & 1U) == 0U);
    sl_udelay_wait(BUZZER_TONE_HALF_PERIOD_US);
  }
  buzzer_write(false);

  printf("[ALERT] Self-test done. No beep or no lamp here means WIRING, "
         "not the alarm logic.\r\n");

  /* Settle into the quiet state: green on, nothing wrong yet. */
  alert_pin_write(LED_GREEN_PORT, LED_GREEN_PIN, true);
}

void sh_alert_set_level(alert_level_t level)
{
  if (level == alert_level) {
    return;   /* unchanged - don't restart the beep phase on every AI tick */
  }
  alert_level = level;

  /* Re-arm the beep so a level change is heard immediately rather than
   * waiting out the remainder of the previous level's period. */
  buzzer_last_toggle = now_ms();

  /* EXACTLY ONE lamp is lit at a time.
   *
   * CRITICAL used to light RED and YELLOW together. Two lamps on at once reads
   * as "two separate faults" from across the room, and on this hardware it
   * mostly just looks like the panel is broken. So critical shows RED, and is
   * told apart from a patient alert by BLINKING it - handled in alert_poll(),
   * in step with the buzzer.
   *
   * Nothing is lost: red steady means the patient, red flashing means the
   * patient AND the line, and the buzzer cadence already differs between the
   * two. */
  alert_pin_write(LED_GREEN_PORT,  LED_GREEN_PIN,
                  level == ALERT_LEVEL_NORMAL);
  alert_pin_write(LED_YELLOW_PORT, LED_YELLOW_PIN,
                  level == ALERT_LEVEL_LINE_WARNING);
  alert_pin_write(LED_RED_PORT,    LED_RED_PIN,
                  level == ALERT_LEVEL_VITALS_ALERT
                  || level == ALERT_LEVEL_CRITICAL);

  /* Buzzer gate: the green LED's actual pin state, read back after writing
   * it above - not the level, not any AI/metric. Green lit means silent,
   * full stop, regardless of why the level engine chose this level. */
  buzzer_on = !alert_green_led_on();
  buzzer_write(buzzer_on);

  printf("[ALERT] Level -> %s\r\n", sh_alert_level_name(level));
}

alert_level_t sh_alert_level(void)
{
  return alert_level;
}

const char *sh_alert_level_name(alert_level_t level)
{
  switch (level) {
    case ALERT_LEVEL_CRITICAL:     return "CRITICAL (line + patient)";
    case ALERT_LEVEL_VITALS_ALERT: return "RED (patient)";
    case ALERT_LEVEL_LINE_WARNING: return "YELLOW (infusion line)";
    default:                       return "GREEN (normal)";
  }
}

static void alert_poll(void)
{
  /* Green lit -> silent, unconditionally. This reads the actual LED pin, not
   * alert_level, so a stale/incorrect level can never leave the buzzer
   * sounding next to a green lamp. */
  if (alert_green_led_on()) {
    if (buzzer_on) {
      buzzer_on = false;
      buzzer_write(false);
    }
    return;
  }

  uint32_t now = now_ms();

  /* Green (handled above) is silent. LINE_WARNING (yellow) chirps briefly
   * then falls silent for a long stretch - 0.5s on, 7s off - so it reads as
   * "pay attention when convenient" and not as a full emergency.
   * VITALS_ALERT (red) and CRITICAL are danger levels: the buzzer stays on
   * continuously, with no silent gaps, so it cannot be mistaken for the
   * intermittent warning chirp. */
  bool continuous = (alert_level == ALERT_LEVEL_VITALS_ALERT
                     || alert_level == ALERT_LEVEL_CRITICAL);

  if (continuous) {
    buzzer_on = true;
  } else {
    uint32_t phase_ms = buzzer_on ? BUZZER_YELLOW_ON_MS : BUZZER_YELLOW_OFF_MS;
    if (now - buzzer_last_toggle >= phase_ms) {
      buzzer_last_toggle = now;
      buzzer_on = !buzzer_on;
      /* Hết pha bíp thì tắt hẳn, đừng để chân dừng ở mức cao. */
      if (!buzzer_on) buzzer_write(false);
    }
  }

  /* Trong lúc "đang bíp" thì phát tần số, không giữ mức. Chạy mỗi vòng lặp và
   * không chặn gì cả — đảo mức xong là trả quyền điều khiển ngay. */
  if (buzzer_on) {
    static uint32_t tone_last_us = 0;
    static bool     tone_level   = false;
    uint32_t now_micros = now_us();
    if (now_micros - tone_last_us >= BUZZER_TONE_HALF_PERIOD_US) {
      tone_last_us = now_micros;
      tone_level = !tone_level;
      buzzer_write(tone_level);
    }
  }

  /* Critical is the one level that flashes its lamp, so that it needs only
   * one lamp to be distinguishable from a steady patient alert. The buzzer
   * is continuous for both, so the LED blink runs on its own timer instead
   * of riding on buzzer_on toggling. */
  if (alert_level == ALERT_LEVEL_CRITICAL) {
    static uint32_t led_last_toggle = 0;
    static bool     led_on          = true;
    if (now - led_last_toggle >= BUZZER_PERIOD_CRITICAL_MS) {
      led_last_toggle = now;
      led_on = !led_on;
      alert_pin_write(LED_RED_PORT, LED_RED_PIN, led_on);
    }
  }
}

#if DROPS_ENABLED
/* Works out whether a clear beam reads HIGH or LOW, by watching it for a
 * second and taking whichever level it spends most of its time at.
 *
 * Measured rather than assumed: which level means "clear" depends on how the
 * photodiode board is wired, and hard-coding the wrong one turns the detector
 * into an inverted one that times the gaps between drops instead of the drops.
 *
 * Sound because drops are rare and brief - even at 200 dpm with a generous
 * 50 ms shadow, the beam is clear for over 80% of the second. */
static void drop_calibrate_idle(void)
{
  uint32_t high = 0, low = 0;
  uint32_t start = now_ms();

  while (now_ms() - start < 1000U) {
    if (GPIO_PinInGet(SENSOR_PORT, SENSOR_PIN)) high++; else low++;
  }

  int idle = (high >= low) ? 1 : 0;
  drop_filter_init(&drop_filter, idle);

  printf("[Drop] Beam calibrated: idle = %s (high %lu / low %lu samples)\r\n",
         idle ? "HIGH" : "LOW", (unsigned long)high, (unsigned long)low);
}
#endif /* DROPS_ENABLED */

/* ============================================================================
 *  Public API
 * ========================================================================== */
void sensor_hub_init(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);

  /* Restore the doctor's last prescription BEFORE any channel starts
   * producing ratios, so the very first AI tick already judges against the
   * right target rather than briefly against the compile-time default. */
  targets_load();
#if DROPS_ENABLED
  GPIO_PinModeSet(SENSOR_PORT, SENSOR_PIN, gpioModeInput, 0);
  drop_calibrate_idle();   /* also initialises the filter */
#endif
#if FLOW_ENABLED
  hx711_gpio_init();
#endif
#if HR_ENABLED || SPO2_ENABLED
  max30102_init();
#endif
  alert_init();
}

void sensor_hub_poll(void)
{
#if DROPS_ENABLED
  drop_filter_step(&drop_filter,
                   GPIO_PinInGet(SENSOR_PORT, SENSOR_PIN),
                   now_ms(), now_us());

  /* Every 30 s, one line saying what the pin is really doing.
   *
   * Kept, at a rate that costs nothing, because this print is what turned
   * "the filter is broken" into "the pulses are 1-6 ms and my threshold was
   * 12 ms" in a single reading. Without it, a detector counting nothing and a
   * detector wired to a dead pin look exactly the same. */
  {
    static uint32_t drop_diag_ms = 0;
    uint32_t now = now_ms();
    if (now - drop_diag_ms >= 30000U) {
      drop_diag_ms = now;
      printf("[Drop] counted=%lu pulses=%lu width=%lu..%lu ms idle=%d\r\n",
             (unsigned long)drop_filter.total_drops,
             (unsigned long)drop_filter.pulses_seen,
             (unsigned long)(drop_filter.pulse_min_ms == 0xFFFFFFFFU
                             ? 0U : drop_filter.pulse_min_ms),
             (unsigned long)drop_filter.pulse_max_ms,
             drop_filter.idle_level);
    }
  }
#endif
#if FLOW_ENABLED
  tare_button_poll();
  hx711_poll();
#endif
#if HR_ENABLED || SPO2_ENABLED
  max30102_poll();
#endif
  /* Non-blocking: only toggles the buzzer when its period has elapsed. */
  alert_poll();
}

/* ---------------- HR ---------------- */
float sh_hr(void)
{
#if HR_ENABLED
  return hr_valid ? hr_bpm : HR_BASE_FILL;
#else
  return HR_BASE_FILL;        /* not connected -> baseline value (normalizes to ~0) */
#endif
}
ch_state_t sh_hr_state(void)
{
#if HR_ENABLED
  if (!max30102_inited || !hr_valid) { return CH_LOST; }
  return (now_ms() - max30102_last_sample_ms > VITAL_TIMEOUT_MS) ? CH_LOST : CH_OK;
#else
  return CH_DISABLED;
#endif
}

/* ---------------- SpO2 ---------------- */
float sh_spo2(void)
{
#if SPO2_ENABLED
  return (spo2_pct > 0.0f) ? spo2_pct : SPO2_BASE_FILL;
#else
  return SPO2_BASE_FILL;
#endif
}
ch_state_t sh_spo2_state(void)
{
#if SPO2_ENABLED
  if (!max30102_inited || spo2_pct <= 0.0f) { return CH_LOST; }
  return (now_ms() - max30102_last_sample_ms > VITAL_TIMEOUT_MS) ? CH_LOST : CH_OK;
#else
  return CH_DISABLED;
#endif
}

/* ---------------- FLOW (load cell) ---------------- */
float sh_flow_ratio(void)
{
#if FLOW_ENABLED
  return flow_ml_per_h / target_flow_ml_h;
#else
  return 1.0f;               /* not connected -> treat as exactly on target (ratio 1.0) */
#endif
}
ch_state_t sh_flow_state(void)
{
#if FLOW_ENABLED
  if (!hx711_have_sample) { return CH_LOST; }
  return (now_ms() - hx711_last_valid_ms > VITAL_TIMEOUT_MS) ? CH_LOST : CH_OK;
#else
  return CH_DISABLED;
#endif
}

/* Raw current weight on the scale, in grams (only meaningful once
 * sh_flow_state() == CH_OK - i.e. taring has completed and a fresh sample
 * has been read). */
float sh_flow_weight_g(void)
{
#if FLOW_ENABLED
  return hx711_weight_g;
#else
  return 0.0f;
#endif
}

/* Current doctor-set target infusion rate, ml/hour. */
float sh_target_flow_ml_h(void)
{
  return target_flow_ml_h;
}

/* Applies a new doctor-set target infusion rate (ml/hour). Called either
 * locally or from app.c's sl_zigbee_af_post_attribute_change_cb() when the
 * TargetFlowMlH Zigbee attribute is written from the network (i.e. the
 * doctor changed it from the HIS Server). Rejects non-positive values
 * (a target of 0 or less makes sh_flow_ratio() meaningless/divide-by-zero). */
void sh_set_target_flow_ml_h(float ml_per_h)
{
  if (ml_per_h > 0.0f && ml_per_h != target_flow_ml_h) {
    target_flow_ml_h = ml_per_h;
    targets_save();   /* survives a power cycle - see targets_save() */
  }
}

/* True for exactly the report cycle(s) while the scale is actively
 * averaging tare samples (button pressed but not yet finished). */
bool sh_flow_tare_in_progress(void)
{
#if FLOW_ENABLED
  return hx711_tare_started && !hx711_inited;
#else
  return false;
#endif
}

/* One-shot: returns true exactly once right after a tare finishes (button-
 * triggered or the initial power-on tare), then clears itself. app.c calls
 * this once per AI report tick to latch a "tare done" event into the alarm
 * bitmap without it getting stuck permanently true. */
bool sh_flow_tare_just_completed(void)
{
#if FLOW_ENABLED
  if (tare_just_completed) {
    tare_just_completed = false;
    return true;
  }
  return false;
#else
  return false;
#endif
}

/* Triggers a tare remotely - same effect as pressing BTN0, but callable
 * from app.c when the doctor sends a "reset scale" command from the HIS
 * Server (no physical button press needed). */
void sh_flow_trigger_tare(void)
{
#if FLOW_ENABLED
  hx711_trigger_tare("remote command");
#endif
}

/* Persistent count of completed tares (never resets) - see the comment on
 * tare_event_count above for why this exists alongside the one-shot pulse. */
uint8_t sh_flow_tare_event_count(void)
{
#if FLOW_ENABLED
  return tare_event_count;
#else
  return 0;
#endif
}

/* ---------------- DROPS (drop sensor) ---------------- */
float sh_drops_per_min(void)
{
#if DROPS_ENABLED
  /* Median-smoothed, and still decaying toward zero through a silence.
   * Both rules live in drop_filter.c. */
  return drop_filter_rate_dpm(&drop_filter, now_ms());
#else
  return target_drops_per_min;
#endif
}
float sh_drops_ratio(void)
{
  return sh_drops_per_min() / target_drops_per_min;
}
ch_state_t sh_drops_state(void)
{
#if DROPS_ENABLED
  return CH_OK;   /* sensor is running; "no drops" becomes an occlusion via the ratio rule */
#else
  return CH_DISABLED;
#endif
}

uint32_t sh_total_drops(void)
{
#if DROPS_ENABLED
  return drop_filter.total_drops;
#else
  return 0U;
#endif
}

/* Current doctor-set target drop rate, drops/min. */
float sh_target_drops_per_min(void)
{
  return target_drops_per_min;
}

/* Applies a new doctor-set target drop rate. Same idea as
 * sh_set_target_flow_ml_h() - called locally or from app.c's
 * sl_zigbee_af_post_attribute_change_cb() when TargetDropsPerMin is written
 * from the network. Rejects non-positive values (divide-by-zero guard). */
void sh_set_target_drops_per_min(float dpm)
{
  if (dpm > 0.0f && dpm != target_drops_per_min) {
    target_drops_per_min = dpm;
    targets_save();   /* survives a power cycle - see targets_save() */
  }
}
