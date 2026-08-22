/* ============================================================================
 *  ai_engine.h — the three on-chip models, and nothing else
 *
 *  This layer does exactly one thing: turn normalised floats into model output.
 *  It holds no history, applies no threshold, and makes no clinical judgement -
 *  all of that lives in ai_fusion.c. Keeping inference separate from decision
 *  is what makes the decision logic testable without a chip.
 *
 *  THREE SEPARATE MODELS, THREE SEPARATE INTERPRETERS, THREE SEPARATE ARENAS.
 *  That is deliberate and costs a few KB of RAM. Merging them into one
 *  flatbuffer would mean one AllocateTensors() (all-or-nothing: too little
 *  memory loses all three) and one Invoke() (which stops at the first failing
 *  operator, so a fault in the drip branch would silently invalidate the vitals
 *  output). Independent failure is worth the RAM.
 *
 *  Every entry point returns bool. Nothing in this file ever hangs, asserts or
 *  loops forever: if a model cannot load, the device must keep measuring and
 *  keep alarming on the clinical rules. That is also why
 *  SL_TFLITE_MICRO_INTERPRETER_INIT_ENABLE is 0 in
 *  config/sl_tflite_micro_config.h - the SDK's own initialiser handles failure
 *  with `while (1)`.
 * ========================================================================== */
#ifndef AI_ENGINE_H
#define AI_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Window geometry. Must match ml/dataset/drip_common.py exactly. */
#define AI_WINDOW      64   /* seconds of history each forecaster consumes */
#define AI_HORIZON     16   /* seconds each forecaster predicts */

/* Static normalisation constants. These are duplicated from the training
 * pipeline (ml/common.py) on purpose - the model was quantised against these
 * exact numbers, and a mismatch produces no error, just quietly wrong answers.
 * Change one side and you must change the other. */
#define AI_DRIP_CENTRE   1.0f
#define AI_DRIP_SCALE    0.35f
#define AI_HR_CENTRE    80.0f
#define AI_HR_SCALE     20.0f
#define AI_SPO2_CENTRE  97.0f
#define AI_SPO2_SCALE    2.0f

static inline float ai_norm_drip(float ratio)
{
  return (ratio - AI_DRIP_CENTRE) / AI_DRIP_SCALE;
}
static inline float ai_denorm_drip(float n)
{
  return n * AI_DRIP_SCALE + AI_DRIP_CENTRE;
}
static inline float ai_norm_hr(float bpm)
{
  return (bpm - AI_HR_CENTRE) / AI_HR_SCALE;
}
static inline float ai_denorm_hr(float n)
{
  return n * AI_HR_SCALE + AI_HR_CENTRE;
}
static inline float ai_norm_spo2(float pct)
{
  return (pct - AI_SPO2_CENTRE) / AI_SPO2_SCALE;
}
static inline float ai_denorm_spo2(float n)
{
  return n * AI_SPO2_SCALE + AI_SPO2_CENTRE;
}
/* Model 3 takes heart rate as a DEVIATION from this patient's own baseline,
 * not as an absolute. Fed absolutes, it flagged 33% of normal snapshots from
 * unseen patients - it had learned the training patients' resting rates and
 * treated any other baseline as an anomaly. */
static inline float ai_norm_hr_dev(float bpm, float baseline_bpm)
{
  return (bpm - baseline_bpm) / AI_HR_SCALE;
}

/* Loads all three models. Returns true only if ALL THREE came up; check the
 * per-model helpers below to find out which one failed. Safe to call once at
 * boot; calling it twice is a no-op. */
bool ai_engine_init(void);

bool ai_engine_drip_ready(void);
bool ai_engine_vitals_ready(void);
bool ai_engine_ae_ready(void);

/* Arena bytes actually used, for the boot log. 0 if that model did not load. */
unsigned ai_engine_drip_arena_used(void);
unsigned ai_engine_vitals_arena_used(void);
unsigned ai_engine_ae_arena_used(void);

/* --- Model 1: drip forecaster --------------------------------------------
 * in : AI_WINDOW normalised drops_ratio samples, oldest first
 * out: AI_HORIZON normalised samples, out[0] = one second ahead */
bool ai_engine_run_drip(const float *window_norm, float *out_norm);

/* --- Model 2: vitals forecaster ------------------------------------------
 * in : AI_WINDOW * 2 normalised samples, interleaved [t*2+0]=HR, [t*2+1]=SpO2
 * out: AI_HORIZON * 2 normalised samples, same interleaving */
bool ai_engine_run_vitals(const float *window_norm, float *out_norm);

/* --- Model 3: vitals autoencoder -----------------------------------------
 * in : {hr_deviation_norm, spo2_norm}
 * out: reconstruction MSE - the anomaly score. Compare against
 *      AI_AE_THRESHOLD below. */
bool ai_engine_run_ae(const float *in_norm, float *recon_error);

/* Reconstruction-error threshold, measured as the 98th percentile of normal
 * validation snapshots ON THE INT8 MODEL (the float model gives 3.466246 - a
 * threshold calibrated against the wrong numerics is a silent mis-calibration).
 * See ml/out/vitals_ae_report.json.
 *
 * Deliberately loose: a flag here does not alarm on its own, it feeds
 * ai_fusion and still has to survive the K=11 persistence filter. */
#define AI_AE_THRESHOLD  3.460599f

#ifdef __cplusplus
}
#endif

#endif /* AI_ENGINE_H */
