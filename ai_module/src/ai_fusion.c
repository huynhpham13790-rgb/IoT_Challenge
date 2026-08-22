/* ============================================================================
 *  ai_fusion.c — see ai_fusion.h for the design and the reasoning
 * ========================================================================== */

#include "ai_fusion.h"
#include "ai_engine.h"
#include "clinical_limits.h"
#include "sensor_hub.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  History. TWO SEPARATE RING BUFFERS, filled from two separate sensors, read
 *  by two separate models. Nothing here is shared between the line side and
 *  the patient side - that separation is the whole design, and merging these
 *  two buffers "to save RAM" would undo it.
 * ------------------------------------------------------------------------- */
static float   s_drip_win[AI_WINDOW];        /* normalised drops_ratio */
static uint8_t s_drip_n, s_drip_head;

static float   s_vit_win[AI_WINDOW * 2];     /* interleaved HR, SpO2 */
static uint8_t s_vit_n, s_vit_head;

/* Last tick's one-second-ahead forecast, so this tick can compare a prediction
 * against what actually arrived. This is the only residual the chip can
 * compute: it cannot see the future, so it grades yesterday's homework. */
static float s_drip_pred_next;
static bool  s_drip_pred_valid;
static float s_vit_pred_next[2];
static bool  s_vit_pred_valid;

static uint8_t s_drip_persist, s_vit_persist, s_ae_persist;

static float s_hr_baseline = 80.0f;   /* until calibration runs */

void ai_fusion_set_hr_baseline(float bpm)
{
  if (bpm > 20.0f && bpm < 220.0f) {
    s_hr_baseline = bpm;
    printf("[AI] HR baseline locked at %d bpm\r\n", (int)(bpm + 0.5f));
  }
}

float ai_fusion_get_hr_baseline(void) { return s_hr_baseline; }

void ai_fusion_init(void)
{
  memset(s_drip_win, 0, sizeof(s_drip_win));
  memset(s_vit_win, 0, sizeof(s_vit_win));
  s_drip_n = s_drip_head = s_vit_n = s_vit_head = 0;
  s_drip_pred_valid = s_vit_pred_valid = false;
  s_drip_persist = s_vit_persist = s_ae_persist = 0;

  line_rules_init();
  ai_engine_init();       /* logs per-model status; failure is not fatal */
}

/* Copies a ring buffer out in chronological order, which is what the model
 * expects. Feeding it in ring order instead would present the window with a
 * discontinuity that walks forward one sample per second - a defect that looks
 * like plausible noise and never throws an error. */
static void flatten(const float *ring, uint8_t head, int stride, float *dst)
{
  for (int t = 0; t < AI_WINDOW; t++) {
    const int src = ((head + t) % AI_WINDOW) * stride;
    for (int c = 0; c < stride; c++) {
      dst[t * stride + c] = ring[src + c];
    }
  }
}

/* Advances a persistence counter and reports whether it has reached K. */
static bool persist(uint8_t *counter, bool flagged)
{
  if (flagged) {
    if (*counter < AI_PERSIST_K) (*counter)++;
  } else {
    *counter = 0;
  }
  return *counter >= AI_PERSIST_K;
}

void ai_fusion_step(fusion_result_t *out)
{
  memset(out, 0, sizeof(*out));
  out->level = ALERT_LEVEL_NORMAL;
  out->headline = "MONITORING";

  /* ======================================================================
   *  1. HARD CLINICAL RULES. Evaluated first and independently of every
   *     model, because they must still work when no model loaded at all.
   * ==================================================================== */
  const ch_state_t hr_state    = sh_hr_state();
  const ch_state_t spo2_state  = sh_spo2_state();
  const ch_state_t drops_state = sh_drops_state();
  const ch_state_t flow_state  = sh_flow_state();

  const float hr    = sh_hr();
  const float spo2  = sh_spo2();
  const float dratio = sh_drops_ratio();

  /* "Signal lost" only counts for a sensor that WAS working. A channel that was
   * never enabled reads zero, and treating that zero as a measurement is how an
   * empty bed ends up screaming "SpO2 0% - critical". */
  out->rule_missing = (hr_state == CH_LOST) || (spo2_state == CH_LOST)
                      || (drops_state == CH_LOST) || (flow_state == CH_LOST);

  out->rule_spo2 = (spo2_state == CH_OK) && (spo2 < AI_SPO2_ABS);

  if (hr_state == CH_OK) {
    const float dev = fabsf(hr - s_hr_baseline) / s_hr_baseline;
    out->rule_hr = (hr < AI_HR_ABS_LOW) || (hr > AI_HR_ABS_HIGH)
                   || (dev > AI_HR_PCT);
  }

  out->rule_flow = (drops_state == CH_OK)
                   && (dratio < AI_FLOW_LO || dratio > AI_FLOW_HI);

  /* ======================================================================
   *  2. LOAD-CELL RULES. Arithmetic, not a model - see line_rules.h.
   * ==================================================================== */
  line_rules_step(&out->line);

  /* ======================================================================
   *  3. MODEL 1 - drip forecaster.
   * ==================================================================== */
  if (drops_state == CH_OK) {
    s_drip_win[s_drip_head] = ai_norm_drip(dratio);
    s_drip_head = (uint8_t)((s_drip_head + 1) % AI_WINDOW);
    if (s_drip_n < AI_WINDOW) s_drip_n++;
  }

  bool drip_flag = false;
  if (s_drip_n >= AI_WINDOW && ai_engine_drip_ready()) {
    out->drip_ready = true;

    /* Grade the previous tick's prediction BEFORE overwriting it. */
    if (s_drip_pred_valid) {
      out->drip_residual = fabsf(s_drip_pred_next - ai_norm_drip(dratio));
      drip_flag = out->drip_residual > AI_DRIP_RESIDUAL_THRESHOLD;
    }

    float win[AI_WINDOW], fc[AI_HORIZON];
    flatten(s_drip_win, s_drip_head, 1, win);
    if (ai_engine_run_drip(win, fc)) {
      s_drip_pred_next  = fc[0];
      s_drip_pred_valid = true;
      out->drip_forecast_16s =
          ai_denorm_drip(fc[AI_HORIZON - 1]) * sh_target_drops_per_min();
      out->drip_forecast_trusted = !drip_flag;

      const float dpm_now = sh_drops_per_min();
      const float slope = (out->drip_forecast_16s - dpm_now)
                          * (60.0f / (float)AI_HORIZON);
      out->drops_trend_dpm_per_min = (int16_t)slope;
      out->drops_trend = (slope > 5.0f) ? 1u : (slope < -5.0f) ? 2u : 0u;

      /* Early warning on the line: the forecast ratio leaves the prescribed
       * band even though the current one has not. */
      const float target = sh_target_drops_per_min();
      if (target > 1.0f) {
        const float fc_ratio = out->drip_forecast_16s / target;
        if ((fc_ratio < AI_FLOW_LO || fc_ratio > AI_FLOW_HI) && !out->rule_flow) {
          out->early_warning = true;
        }
      }
    } else {
      s_drip_pred_valid = false;
    }
  }
  out->drip_anomaly = persist(&s_drip_persist, drip_flag);
  out->drip_persist = s_drip_persist;

  /* ======================================================================
   *  4. MODEL 2 - vitals forecaster.
   * ==================================================================== */
  const bool vitals_live = (hr_state == CH_OK) && (spo2_state == CH_OK);
  if (vitals_live) {
    s_vit_win[s_vit_head * 2 + 0] = ai_norm_hr(hr);
    s_vit_win[s_vit_head * 2 + 1] = ai_norm_spo2(spo2);
    s_vit_head = (uint8_t)((s_vit_head + 1) % AI_WINDOW);
    if (s_vit_n < AI_WINDOW) s_vit_n++;
  } else {
    /* Do NOT pad the window with a fabricated value to keep the model running.
     * v1 did that, and the model went on emitting a heart-rate forecast derived
     * from a flat line nobody measured - indistinguishable, on a dashboard,
     * from a real one. No signal means no forecast. */
    s_vit_pred_valid = false;
    s_vit_persist = 0;
  }

  bool vit_flag = false;
  if (vitals_live && s_vit_n >= AI_WINDOW && ai_engine_vitals_ready()) {
    out->vitals_ready = true;

    if (s_vit_pred_valid) {
      const float e_hr   = fabsf(s_vit_pred_next[0] - ai_norm_hr(hr));
      const float e_spo2 = fabsf(s_vit_pred_next[1] - ai_norm_spo2(spo2));
      out->vitals_residual = (e_hr > e_spo2) ? e_hr : e_spo2;
      vit_flag = out->vitals_residual > AI_VITALS_RESIDUAL_THRESHOLD;
    }

    float win[AI_WINDOW * 2], fc[AI_HORIZON * 2];
    flatten(s_vit_win, s_vit_head, 2, win);
    if (ai_engine_run_vitals(win, fc)) {
      s_vit_pred_next[0] = fc[0];
      s_vit_pred_next[1] = fc[1];
      s_vit_pred_valid   = true;
      out->hr_forecast_16s   = ai_denorm_hr(fc[(AI_HORIZON - 1) * 2 + 0]);
      out->spo2_forecast_16s = ai_denorm_spo2(fc[(AI_HORIZON - 1) * 2 + 1]);
      out->vitals_forecast_trusted = !vit_flag;

      const float slope = (out->hr_forecast_16s - hr) * (60.0f / (float)AI_HORIZON);
      out->hr_trend_bpm_per_min = (int16_t)slope;
      out->hr_trend = (slope > 10.0f) ? 1u : (slope < -10.0f) ? 2u : 0u;

      /* Early warning on the patient. Checked across the WHOLE horizon, not
       * just its last sample: a dip that recovers by second 16 still has to be
       * seen, and it is precisely the early one that buys time. */
      for (int h = 0; h < AI_HORIZON; h++) {
        const float f_hr   = ai_denorm_hr(fc[h * 2 + 0]);
        const float f_spo2 = ai_denorm_spo2(fc[h * 2 + 1]);
        if ((f_spo2 < AI_SPO2_ABS && spo2 >= AI_SPO2_ABS)
            || (f_hr < AI_HR_ABS_LOW && hr >= AI_HR_ABS_LOW)
            || (f_hr > AI_HR_ABS_HIGH && hr <= AI_HR_ABS_HIGH)) {
          out->early_warning = true;
          break;
        }
      }
    } else {
      s_vit_pred_valid = false;
    }
  }
  out->vitals_anomaly = persist(&s_vit_persist, vit_flag);
  out->vitals_persist = s_vit_persist;

  /* ======================================================================
   *  5. MODEL 3 - vitals autoencoder. Judges the INSTANT, not the dynamics.
   *
   *     This is the model that answers "is the patient themselves all right",
   *     and it is the reason the line branch and the patient branch can be
   *     told apart at all. It sees no drip data by design: with the drip
   *     channel inside the network, an occluded line would raise the
   *     reconstruction error and contaminate the verdict on the PATIENT -
   *     which is precisely the v1 defect this rewrite exists to remove.
   * ==================================================================== */
  bool ae_flag = false;
  if (vitals_live && ai_engine_ae_ready()) {
    const float in[2] = { ai_norm_hr_dev(hr, s_hr_baseline),
                          ai_norm_spo2(spo2) };
    if (ai_engine_run_ae(in, &out->ae_error)) {
      ae_flag = out->ae_error > AI_AE_THRESHOLD;
    }
  }
  out->ae_anomaly = persist(&s_ae_persist, ae_flag);
  out->ae_persist = s_ae_persist;

  /* ======================================================================
   *  6. THE TWO BRANCHES, then the level.
   * ==================================================================== */
  const bool line_hw_fault = out->line.valid
                             && (out->line.state == LINE_OCCLUSION
                                 || out->line.state == LINE_FREE_FLOW);

  /* A flow-ratio breach only counts toward the LINE branch when the load cell
   * has not already explained it. A bag running dry slows the drops through
   * ordinary physics - the fluid column is shorter, so the pressure is lower -
   * and alarming on that is how a ward learns to ignore the device. */
  const bool flow_rule_unexplained =
      out->rule_flow
      && !(out->line.valid && (out->line.state == LINE_RUNNING_LOW
                               || out->line.state == LINE_EMPTY));

  out->line_branch = out->drip_anomaly || line_hw_fault || flow_rule_unexplained;

  out->patient_branch = out->vitals_anomaly || out->ae_anomaly
                        || out->rule_spo2 || out->rule_hr;

  /* The LEVEL is decided by the most severe thing that is true - unchanged.
   * What is new is that every active fault is also recorded, so the bedside
   * screen can show them all in turn instead of hiding everything behind the
   * worst one. */
  if (out->line_branch && out->patient_branch) {
    out->level = ALERT_LEVEL_CRITICAL;
  } else if (out->patient_branch) {
    out->level = ALERT_LEVEL_VITALS_ALERT;
  } else if (out->line_branch || out->rule_missing || out->early_warning
             || (out->line.valid && (out->line.state == LINE_SENSOR_MISMATCH
                                     || out->line.state == LINE_EMPTY))) {
    /* A dead sensor is not a well patient, and neither is a drop sensor
     * disagreeing with the scale. Someone has to look, but none of it is a
     * clinical emergency, so it warns rather than alarms. Early warning is a
     * prediction, not a fact, and sits here for the same reason. */
    out->level = ALERT_LEVEL_LINE_WARNING;
  }

  /* Causes, most severe first. Kept in the same order the level test above
   * uses, so the first entry is always the one that set the level and the
   * screen's first frame agrees with the buzzer. */
  out->cause_count = 0U;
  #define ADD_CAUSE(text)                                            \
    do {                                                             \
      if (out->cause_count < AI_MAX_CAUSES) {                        \
        out->causes[out->cause_count++] = (text);                    \
      }                                                              \
    } while (0)

  if (out->line_branch && out->patient_branch) {
    /* Must fit 21 characters - the font is 6 px wide on a 128 px panel, and
     * draw_text_centered silently clips anything longer. The old wording,
     * "DANGER: FLUID OVERLOAD?", was 23 and lost its last two characters on
     * the actual screen. */
    ADD_CAUSE("DANGER: OVERLOAD");
  }
  if (out->patient_branch)  ADD_CAUSE("PATIENT ALERT");
  if (out->line_branch)     ADD_CAUSE("CHECK IV LINE");
  if (out->rule_missing)    ADD_CAUSE("SENSOR SIGNAL LOST");
  if (out->line.valid && out->line.state == LINE_SENSOR_MISMATCH) {
    ADD_CAUSE("DROP SENSOR FAULT");
  }
  if (out->line.valid && out->line.state == LINE_EMPTY) ADD_CAUSE("BAG EMPTY");
  if (out->early_warning)                               ADD_CAUSE("EARLY WARNING");
  if (out->line.valid && out->line.state == LINE_RUNNING_LOW) {
    /* Information, not an alarm: the infusion is behaving exactly as it
     * should. The level stays NORMAL so the buzzer keeps quiet; this is here
     * for the nurse who happens to walk past. */
    ADD_CAUSE("BAG RUNNING LOW");
  }
  #undef ADD_CAUSE

  if (out->cause_count > 0U) out->headline = out->causes[0];
}
