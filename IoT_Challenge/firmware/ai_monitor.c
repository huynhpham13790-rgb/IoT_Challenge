/* ============================================================================
 *  ai_monitor.c — Feature assembly -> autoencoder + clinical rules -> alarm
 *  The AI half of Smart IV. Reads each channel's state from sensor_hub:
 *    - CH_DISABLED: no sensor connected -> fill the BASELINE value -> normalises
 *      to ~0 -> the autoencoder reconstructs it almost exactly -> NO false
 *      alarm, and the missing flag stays 0.
 *    - CH_OK      : use the real reading.
 *    - CH_LOST    : connected but signal lost -> raise the missing flag -> ALARM.
 * ========================================================================== */
#include "ai_monitor.h"
#include "sensor_hub.h"
#include <math.h>

/* TFLM glue written in C++ (model_runner.cpp), exposed to C */
extern void  ai_model_init(void);
extern float ai_recon_error(const float feat6[6]);  // reconstruction MSE (normalised)

/* ===== Normalisation parameters (StandardScaler) — MATCHES scaler.json =====
 * Only the 4 continuous channels are normalised; the 2 flags stay as 0/1. */
static const float MEAN[4]  = { 81.68079988f, 97.80115772f, 0.99432714f, 0.99472891f };
static const float SCALE[4] = {  6.61951456f,  0.95788093f, 0.06244021f, 0.06359013f };

/* Patient's personal HR baseline (median of the first ~60s). Default = MEAN[0]. */
static float hr_baseline = 81.68079988f;

void ai_monitor_set_hr_baseline(float hr_base)
{
  if (hr_base > 30.0f && hr_base < 200.0f) hr_baseline = hr_base;
}

float ai_monitor_get_hr_baseline(void)
{
  return hr_baseline;
}

void ai_monitor_init(void)
{
  ai_model_init();
}

/* Assemble the 6 raw features from sensor_hub and set the two missing flags
 * according to each channel's STATE. */
static void gather_features(float feat[6], uint8_t *vital_missing, uint8_t *line_missing)
{
  ch_state_t hr_s   = sh_hr_state();
  ch_state_t spo2_s = sh_spo2_state();
  ch_state_t flow_s = sh_flow_state();
  ch_state_t drop_s = sh_drops_state();

  /* Raw values: for a DISABLED channel sensor_hub already returns a neutral
   * BASELINE value. For LOST we also take the baseline (so the autoencoder is
   * not pushed off-distribution) and raise the missing flag below - the alarm
   * then comes from the rules, not from the autoencoder. */
  feat[0] = sh_hr();          // heart_rate
  feat[1] = sh_spo2();        // spo2
  feat[2] = sh_flow_ratio();  // flow_ratio
  feat[3] = sh_drops_ratio(); // drops_ratio

  /* The SIGNAL-LOST flags are raised ONLY for a channel that was connected and
   * then lost it (CH_LOST). CH_DISABLED (never connected) does NOT raise them,
   * so an unequipped bed never produces a false alarm. */
  *vital_missing = (hr_s == CH_LOST || spo2_s == CH_LOST) ? 1 : 0;
  *line_missing  = (flow_s == CH_LOST || drop_s == CH_LOST) ? 1 : 0;

  feat[4] = (float)(*vital_missing);
  feat[5] = (float)(*line_missing);
}

/* Clinical rules: set the individual reason flags. */
static void clinical_rules(const float feat[6], ai_result_t *r)
{
  float hr   = feat[0];
  float spo2 = feat[1];
  float flow = feat[2];
  float drop = feat[3];

  /* HR: a percentage deviation from the patient's own baseline, OR breaching
   * the absolute floor/ceiling. Only evaluated while the HR channel actually
   * has data (CH_OK). */
  r->reason_hr = 0;
  if (sh_hr_state() == CH_OK) {
    float dev = fabsf(hr - hr_baseline) / (hr_baseline > 1.0f ? hr_baseline : 1.0f);
    if (dev > AI_HR_PCT || hr < AI_HR_ABS_LOW || hr > AI_HR_ABS_HIGH)
      r->reason_hr = 1;
  }

  /* SpO2: an ABSOLUTE limit (not a percentage). Only evaluated when CH_OK. */
  r->reason_spo2 = 0;
  if (sh_spo2_state() == CH_OK && spo2 < AI_SPO2_ABS)
    r->reason_spo2 = 1;

  /* Infusion line: ratio outside [LO, HI]. Only for channels that are running. */
  r->reason_flow = 0;
  if (sh_flow_state() == CH_OK && (flow > AI_FLOW_HI || flow < AI_FLOW_LO))
    r->reason_flow = 1;
  if (sh_drops_state() == CH_OK && (drop > AI_FLOW_HI || drop < AI_FLOW_LO))
    r->reason_flow = 1;

  /* Signal lost (was connected, then dropped out) */
  r->reason_missing = (feat[4] > 0.5f || feat[5] > 0.5f) ? 1 : 0;
}

void ai_monitor_step(ai_result_t *out)
{
  uint8_t vm = 0, lm = 0;
  float feat[6];
  gather_features(feat, &vm, &lm);

  /* Normalise the 4 continuous channels for the autoencoder; flags stay 0/1. */
  /* Clamp the normalised values before feeding the model.
   *
   * The model input is int8-quantised (IN_SCALE=0.0222, IN_ZP=-66), so the
   * REPRESENTABLE range is only about [-1.4, +4.3]: anything outside is clipped
   * at quantisation time and the model NEVER "sees" it. But ai_recon_error()
   * compares the (already clipped) reconstruction against the UNCLAMPED `norm`,
   * so the error grows without bound. Observed for real on the chip:
   * err = 2147483647 (INT32_MAX) whenever the load cell drifted. Clamping at
   * +-4 preserves every genuine pathological state (a full occlusion normalises
   * flow_ratio to -15.9 and is still clipped, but the clinical rules already
   * catch that case through their absolute limits, independently of the
   * autoencoder). */
  float norm[6];
  for (int i = 0; i < 4; i++) {
    float v = (feat[i] - MEAN[i]) / SCALE[i];
    if (v >  4.0f) v =  4.0f;
    if (v < -4.0f) v = -4.0f;
    if (!(v == v)) v = 0.0f;          /* NaN -> baseline value */
    norm[i] = v;
  }
  norm[4] = feat[4];
  norm[5] = feat[5];

  /* Autoencoder: reconstruction error (model_runner handles quantise/dequantise). */
  float err = ai_recon_error(norm);

  /* Clinical rules */
  ai_result_t r;
  for (int i = 0; i < 6; i++) r.feat[i] = feat[i];
  r.recon_error = err;
  r.reason_ae = (err > AI_AE_THRESHOLD) ? 1 : 0;
  clinical_rules(feat, &r);

  /* Alarm = OR of all reasons (recall is prioritised over precision) */
  r.alarm = (r.reason_ae || r.reason_hr || r.reason_spo2 ||
             r.reason_flow || r.reason_missing) ? 1 : 0;

  *out = r;
}
