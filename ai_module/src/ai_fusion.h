/* ============================================================================
 *  ai_fusion.h — turns three independent model outputs into ONE alert level
 *
 *  Replaces ai_monitor.c + ts_monitor.c, which between them ran a 6-feature
 *  autoencoder and a 4-channel forecaster and produced a single anomaly score.
 *
 *  --- What the split buys, and why the combining happens HERE -------------
 *
 *  v1 could tell you something was wrong. It could not tell you WHAT, because
 *  one network consumed vitals and drip together and emitted one number.
 *  Measured on the .tflite that was actually running on the chip, perturbing
 *  only the drip channels moved the heart-rate forecast by about 2 bpm - a
 *  kinked line shifting the patient's predicted pulse.
 *
 *  Three separate models give three attributable signals, which is what makes
 *  the question a nurse asks first answerable: IS IT THE LINE OR IS IT THE
 *  PATIENT? Those have completely different responses, and v1 gave them the
 *  same alarm.
 *
 *  The combining is written as explicit rules in ai_fusion.c rather than learnt
 *  by a fourth network, and that is a requirement rather than a shortcut: a
 *  nurse can be told "the line is blocked but your patient is fine" and can
 *  check that claim. A weight matrix cannot be checked at three in the morning.
 *
 *  --- The two branches ----------------------------------------------------
 *
 *      LINE branch     = drip forecaster anomaly  OR  load-cell rules say
 *                        occluded / free flow
 *      PATIENT branch  = vitals forecaster anomaly  OR  vitals autoencoder
 *                        anomaly  OR  a hard clinical limit is breached
 *
 *      line only              -> ALERT_LEVEL_LINE_WARNING   "check the tubing"
 *      patient only           -> ALERT_LEVEL_VITALS_ALERT   "go to the bedside"
 *      both at once           -> ALERT_LEVEL_CRITICAL       "suspected overload"
 *
 *  --- Persistence, and the one thing it must never delay ------------------
 *
 *  A model anomaly must hold for AI_PERSIST_K consecutive seconds before it may
 *  raise anything. Two- to six-second blips are coughs, arm movement, a drop
 *  falling off-centre; alarming on them is how a ward learns to ignore the
 *  device. Replaying real recordings at 1 Hz, this takes the drip branch from
 *  29.0 false alarms/hour to 0.0, and the vitals branch from 47.6 to 0.0.
 *
 *  HARD CLINICAL LIMITS DO NOT GO THROUGH THE FILTER. SpO2 below 90% alarms on
 *  the tick it is seen. The alarm-fatigue argument justifies waiting out a
 *  transient anomaly; it does not justify waiting out a desaturation.
 * ========================================================================== */
#ifndef AI_FUSION_H
#define AI_FUSION_H

#include <stdbool.h>
#include <stdint.h>

#include "ai_engine.h"     /* AI_AE_THRESHOLD and the normalisation constants:
                           * callers that log a threshold should get all three
                           * from one include, not two. */
#include "line_rules.h"
#include "sensor_hub.h"

/* Consecutive seconds an AI anomaly must persist before it may alarm. */
#define AI_PERSIST_K   11

/* --- What a forecast residual can and cannot detect ----------------------
 *
 * Worth understanding before changing anything here. The forecasters were
 * trained to be invariant to the operating LEVEL (see the level-augmentation
 * note in ml/train_drip_forecaster.py - without it the model transferred from
 * simulated data to real hardware five times worse than a persistence
 * baseline). A consequence follows directly: once a fault settles into a NEW
 * STEADY STATE, the model predicts that state perfectly well and the residual
 * returns to zero.
 *
 * So the residual detects TRANSITIONS - a line beginning to occlude, a rate
 * starting to run away - and it detects them early, which is its whole value.
 * It does NOT hold an alarm up through a sustained fault, and it was never
 * going to.
 *
 * Holding the alarm up is the job of the things that look at the CURRENT state
 * rather than its dynamics: the hard clinical limits, the load-cell rules, and
 * the vitals autoencoder. The branch logic below ORs them together for exactly
 * this reason - remove one and a fault that stabilises goes quiet while the
 * patient is still in it.
 */

/* Forecast-residual thresholds, the 98th percentile of the one-step-ahead
 * residual on NORMAL validation data, measured on the int8 models that ship.
 * See ml/out/evaluation.json. Raising these trades recall for quiet; lowering
 * them does the reverse. They are not tuning knobs to be adjusted until the
 * demo looks good. */
#define AI_DRIP_RESIDUAL_THRESHOLD    0.0662f
#define AI_VITALS_RESIDUAL_THRESHOLD  0.5295f

/* Most faults that can be true at once. Six covers every combination the
 * decision below can produce; the array is bounded so nothing here can grow
 * without someone noticing. */
#define AI_MAX_CAUSES 6U

typedef struct {
  /* ---- final answer ---- */
  alert_level_t level;
  const char   *headline;      /* the single most severe cause */

  /* Every active fault, most severe first.
   *
   * The bedside screen used to be told only the worst one, which is fine when
   * there is one and misleading when there are three: a nurse fixes the named
   * problem, sees the alarm persist, and has no way to find out what else is
   * wrong without walking to the console. The display cycles through these.
   *
   * All uppercase A-Z, digits and space: the 5x7 font has no lowercase, and an
   * unmapped character renders as a BLANK rather than as anything visible.
   * tools/oled_test.c enforces this. */
  const char   *causes[AI_MAX_CAUSES];
  uint8_t       cause_count;

  /* ---- branch verdicts (what made the decision) ---- */
  bool line_branch;
  bool patient_branch;

  /* ---- individual contributions, for the log and the HIS Server ---- */
  bool drip_anomaly;           /* Model 1, persistence-confirmed */
  bool vitals_anomaly;         /* Model 2, persistence-confirmed */
  bool ae_anomaly;             /* Model 3, persistence-confirmed */
  bool rule_spo2;              /* hard limits - never filtered */
  bool rule_hr;
  bool rule_flow;
  bool rule_missing;

  /* ---- scores, for debugging and the dashboard ---- */
  float drip_residual;
  float vitals_residual;
  float ae_error;
  uint8_t drip_persist;        /* current run length, 0..AI_PERSIST_K */
  uint8_t vitals_persist;
  uint8_t ae_persist;

  /* ---- forecasts, only meaningful when the matching *_valid is true ---- */
  bool  drip_ready;            /* the 64 s window has filled */
  bool  vitals_ready;
  float drip_forecast_16s;     /* drops per minute */
  float hr_forecast_16s;       /* bpm */
  float spo2_forecast_16s;     /* % */

  /* A channel's forecast is only a FORECAST while the model is tracking
   * reality. Once a channel sits in a sustained abnormal state the model stops
   * predicting what will happen and reverts toward what normal would look like
   * - which is exactly what makes the residual a detector, and exactly what
   * makes the number a lie if the UI labels it "forecast". When this is false
   * the dashboard must relabel it "expected if normal". */
  bool  drip_forecast_trusted;
  bool  vitals_forecast_trusted;

  /* --- Early warning: the FORECAST crosses a clinical limit before reality
   * does. This is a separate detector from the residual, and it exists because
   * of a measured gap rather than for symmetry.
   *
   * The residual fires on TRANSITIONS. A slow, smooth deterioration is highly
   * predictable, so the model tracks it happily and the residual stays near
   * zero - the model is right, the patient is still sinking. Checking whether
   * the model's own forecast breaches a limit catches exactly that case, and it
   * is the only mechanism here that can warn AHEAD of a gradual desaturation.
   *
   * It maps to a warning rather than an alarm: it is a prediction, not a fact. */
  bool  early_warning;

  /* Trend over the forecast horizon, for the dashboard. Negative = falling. */
  int16_t hr_trend_bpm_per_min;
  int16_t drops_trend_dpm_per_min;
  uint8_t hr_trend;      /* 0 steady, 1 rising, 2 falling */
  uint8_t drops_trend;

  /* ---- load-cell verdict, passed through ---- */
  line_result_t line;
} fusion_result_t;

/* Loads the models and clears the history buffers. Never fails fatally: if a
 * model does not load, its branch simply never fires and the clinical rules
 * carry the device. */
void ai_fusion_init(void);

/* Call once per second, after sensor_hub_poll(). */
void ai_fusion_step(fusion_result_t *out);

/* Patient's own heart-rate baseline, captured over the first ~60 s after the
 * sensor is attached. Both the hard HR rule and Model 3 are relative to it. */
void  ai_fusion_set_hr_baseline(float bpm);
float ai_fusion_get_hr_baseline(void);

#endif /* AI_FUSION_H */
