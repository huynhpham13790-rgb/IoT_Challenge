/* ============================================================================
 *  ts_monitor.c — Monitoring built on the time-series forecaster (see .h)
 *
 *  Every normalisation constant and threshold below MUST match exactly what was
 *  used during training, otherwise the model receives inputs from a different
 *  distribution and its forecasts are meaningless:
 *      normalisation    -> ai_timeseries/train_forecaster.py
 *      error std-devs   -> ai_timeseries/out/fw_thresholds.txt (measured on the
 *                          VALIDATION split, never on the test split)
 * ========================================================================== */
#include "ts_monitor.h"
#include "ts_forecaster.h"
#include "sensor_hub.h"
#include "ai_monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Normalisation: MUST MATCH train_forecaster.py ----------------------- */
#define HR_CENTER    80.0f
#define HR_SCALE     20.0f
#define SPO2_CENTER  97.0f
#define SPO2_SCALE    2.0f
#define DR_CENTER     1.0f
#define DR_SCALE      0.35f
#define WREL_SCALE    5.0f

/* Fill value used when a vitals channel loses signal. It equals the
 * normalisation centre, so it normalises to exactly 0.0 - the model sees a flat
 * line, forecasts a flat line, and the error stays near zero. Such channels are
 * ALSO excluded from the anomaly score (see the validity mask below), so they
 * cannot manufacture a false alarm. */
#define HR_BASE_FILL    80.0f
#define SPO2_BASE_FILL  97.0f

/* ---- Per-channel forecast-error std-dev, measured on NORMAL data ----------
 * Used to normalise the error before taking the MAX across channels. Without
 * it, a desaturation event (visible only on the SpO2 channel) would be diluted
 * by the three channels that are behaving perfectly well. */
static const float TS_ERR_SD[TS_FC_N_CH] = {
  0.180175f,   /* HR    */
  0.266739f,   /* SpO2  */
  0.105932f,   /* giot  */
  0.128137f    /* can   */
};

/* Score threshold and persistence length, both selected on the VALIDATION
 * split under the rule "maximise recall subject to the transient false-alarm
 * rate being no worse than the old threshold-only approach". */
#define TS_POINT_THR   5.6111f
#define TS_PERSIST_K   11

/* Trend deadband. Evaluation showed the direction is only trustworthy from
 * about +-10 bpm/min upwards (66% correct on real data); below that, short-term
 * heart-rate direction is mostly noise. */
#define TS_TREND_DEADBAND_BPM_PER_MIN  10.0f

/* Deadband for the drip-rate trend. NOTE: unlike the +-10 bpm/min figure above
 * (which came out of the evaluation on real data), this +-5 drops/min has NOT
 * been measured for accuracy on the test set - it is simply a change large
 * enough not to be mechanical jitter of the drip chamber. It would need its own
 * evaluation before feeding an alarm decision; today it only drives DISPLAY. */
#define TS_TREND_DEADBAND_DPM_PER_MIN  5.0f

/* ---- Clamp for normalised values -----------------------------------------
 * The model input is int8-quantised with IN_SCALE=0.01137, ZP=-5, so the
 * REPRESENTABLE range is only about [-1.40, +1.50]: anything outside it is
 * clipped at quantisation time and the model NEVER "sees" it.
 *
 * Without this clamp we reproduced a real bug on the bench: one noisy load-cell
 * reading (thousands of grams between consecutive seconds - physically
 * impossible) produced a raw `truth` of 828 while the forecast was bounded to
 * +-1.25, sending the anomaly score to thousands of times the threshold.
 * Comparing a CLAMPED forecast against an UNCLAMPED measurement is simply the
 * wrong comparison.
 *
 * +-4.0 is chosen rather than +-1.5 so genuine pathological states survive:
 *   full occlusion   -> drops_ratio = (0-1)/0.35 = -2.86
 *   free flow at 2x  -> drops_ratio = (2-1)/0.35 = +2.86
 * both fit inside +-4, so no clinical information is lost. */
#define TS_NORM_CLAMP  4.0f

static inline float clamp_norm(float v)
{
  if (v >  TS_NORM_CLAMP) return  TS_NORM_CLAMP;
  if (v < -TS_NORM_CLAMP) return -TS_NORM_CLAMP;
  if (!(v == v)) return 0.0f;          /* NaN -> treat as the baseline value */
  return v;
}

/* ---- Sliding window ------------------------------------------------------
 * The first three channels are stored ALREADY normalised; the weight is stored
 * RAW (grams) in a separate array.
 *
 * Why weight is kept separate: channel 3 is a value RELATIVE to the start of
 * the window, so every time the window slides by one sample the reference point
 * moves. Storing it pre-normalised means recomputing the whole column every
 * second, which is very easy to get wrong - the first version of this code hit
 * two bugs doing exactly that (wrong channel index, and mutating the very
 * element being used as the reference midway through the loop). Storing raw and
 * converting at inference time is straightforward and has nothing to get
 * wrong. */
static float    s_win_n[TS_FC_WINDOW][3];      /* HR, SpO2, drops - normalised */
static float    s_win_w[TS_FC_WINDOW];         /* raw weight, grams */
static uint16_t s_filled = 0;

/* Scratch buffer holding the assembled model input (flat, [t][channel]). */
static float s_model_in[TS_FC_WINDOW * TS_FC_N_CH];

/* Forecast from the PREVIOUS run, compared against what was just measured. */
static float s_prev_fc[TS_FC_OUT_LEN];
static bool  s_have_prev_fc = false;

static ts_result_t s_last;

bool ts_monitor_init(void)
{
  memset(&s_last, 0, sizeof(s_last));
  s_filled = 0;
  s_have_prev_fc = false;
  return ts_forecaster_init();
}

const ts_result_t *ts_monitor_last(void)
{
  return &s_last;
}

/* Pushes one new sample onto the end of the window (FIFO).
 *
 * O(WINDOW) copies per second - negligible next to the 4.77 ms an inference
 * takes, and it buys code with no ring-buffer index arithmetic to get wrong. */
static void window_push(const float ch3[3], float weight_g_now)
{
  if (s_filled < TS_FC_WINDOW) {
    s_win_n[s_filled][0] = ch3[0];
    s_win_n[s_filled][1] = ch3[1];
    s_win_n[s_filled][2] = ch3[2];
    s_win_w[s_filled]    = weight_g_now;
    s_filled++;
    return;
  }

  memmove(&s_win_n[0][0], &s_win_n[1][0],
          (size_t)(TS_FC_WINDOW - 1) * 3 * sizeof(float));
  memmove(&s_win_w[0], &s_win_w[1],
          (size_t)(TS_FC_WINDOW - 1) * sizeof(float));

  s_win_n[TS_FC_WINDOW - 1][0] = ch3[0];
  s_win_n[TS_FC_WINDOW - 1][1] = ch3[1];
  s_win_n[TS_FC_WINDOW - 1][2] = ch3[2];
  s_win_w[TS_FC_WINDOW - 1]    = weight_g_now;
}

/* Assembles the model input: weight converted to a value RELATIVE to the
 * start of the window. */
static void build_model_input(void)
{
  const float w0 = s_win_w[0];
  for (int t = 0; t < TS_FC_WINDOW; t++) {
    float *slot = &s_model_in[t * TS_FC_N_CH];
    slot[0] = s_win_n[t][0];
    slot[1] = s_win_n[t][1];
    slot[2] = s_win_n[t][2];
    slot[3] = clamp_norm((s_win_w[t] - w0) / WREL_SCALE);
  }
}

void ts_monitor_step(ts_result_t *out)
{
  /* ---- Read the four channels ----------------------------------------------
   * Deliberately does NOT require every channel to have signal. The infusion
   * line channels (drips, weight) are perfectly valid while no finger is on the
   * PPG sensor - insisting that both HR and SpO2 be CH_OK would kill the entire
   * forecaster whenever the vitals sensor is unattached, including the occlusion
   * monitoring that has nothing to do with heart rate. (That was a real bug on
   * the first bench run: the window never filled.)
   *
   * A channel without signal is filled with its BASELINE value so the model sees
   * a flat line, AND is excluded from the anomaly score below - loss of signal
   * is already handled by tier-1 clinical rules, which alarm immediately, so
   * ts_monitor does not need to cover it. */
  bool hr_ok   = (sh_hr_state()   == CH_OK);
  bool spo2_ok = (sh_spo2_state() == CH_OK);

  float target_dpm = sh_target_drops_per_min();
  if (target_dpm <= 0.0f) target_dpm = 1.0f;

  float ch3[3];
  ch3[0] = clamp_norm(((hr_ok   ? sh_hr()   : HR_BASE_FILL)   - HR_CENTER)   / HR_SCALE);
  ch3[1] = clamp_norm(((spo2_ok ? sh_spo2() : SPO2_BASE_FILL) - SPO2_CENTER) / SPO2_SCALE);
  ch3[2] = clamp_norm(((sh_drops_per_min() / target_dpm) - DR_CENTER) / DR_SCALE);

  float w_now = sh_flow_weight_g();

  /* Which channels count towards the anomaly score on this tick. */
  bool ch_valid[TS_FC_N_CH] = {
    hr_ok,
    spo2_ok,
    (sh_drops_state() == CH_OK),
    (sh_flow_state()  == CH_OK)
  };

  /* ---- 1) Anomaly score: LAST second's forecast vs what was measured NOW --- */
  float score = 0.0f;
  float ch_err[TS_FC_N_CH] = { 0.0f, 0.0f, 0.0f, 0.0f };
  if (s_have_prev_fc) {
    float wrel_now = (s_filled == 0) ? 0.0f
                     : clamp_norm((w_now - s_win_w[0]) / WREL_SCALE);
    float truth[TS_FC_N_CH] = { ch3[0], ch3[1], ch3[2], wrel_now };

    for (int c = 0; c < TS_FC_N_CH; c++) {
      if (!ch_valid[c]) { ch_err[c] = 0.0f; continue; }  /* no signal -> skip */
      float pred = s_prev_fc[0 * TS_FC_N_CH + c];   /* horizon step h = 0 */
      float e = fabsf(pred - truth[c]) / TS_ERR_SD[c];
      ch_err[c] = e;
      if (e > score) score = e;
    }
  }
  s_last.anomaly_score = score;

  if (score > TS_POINT_THR) {
    if (s_last.persist_count < 255) s_last.persist_count++;
  } else {
    s_last.persist_count = 0;
  }
  s_last.anomaly_confirmed = (s_last.persist_count >= TS_PERSIST_K);

  /* ---- 2) Push the new sample into the window ----------------------------- */
  window_push(ch3, w_now);
  s_last.ready = (s_filled >= TS_FC_WINDOW);

  if (!s_last.ready) {
    s_last.have_forecast = false;
    if (out) *out = s_last;
    return;
  }

  /* ---- 3) Forecast the next 16 seconds ------------------------------------ */
  build_model_input();

  float fc[TS_FC_OUT_LEN];
  if (!ts_forecaster_run(s_model_in, fc)) {
    s_last.have_forecast = false;
    s_have_prev_fc = false;
    if (out) *out = s_last;
    return;
  }
  memcpy(s_prev_fc, fc, sizeof(fc));
  s_have_prev_fc = true;
  s_last.have_forecast = true;

  /* ---- 4) Heart-rate trend -------------------------------------------------
   * Average the first four horizon steps against the last four, then express it
   * in bpm/min. A raw slope over a noisy signal is close to meaningless;
   * smoothing first is what makes this match "trend" in the clinical sense.
   * Only meaningful while the HR channel actually has signal. */
  if (hr_ok) {
    float hr_first = 0.0f, hr_last = 0.0f;
    for (int h = 0; h < 4; h++) {
      hr_first += fc[h * TS_FC_N_CH + 0];
      hr_last  += fc[(TS_FC_HORIZON - 1 - h) * TS_FC_N_CH + 0];
    }
    hr_first = hr_first / 4.0f * HR_SCALE + HR_CENTER;
    hr_last  = hr_last  / 4.0f * HR_SCALE + HR_CENTER;

    s_last.hr_trend_bpm_per_min = (hr_last - hr_first) / 12.0f * 60.0f;
    if (s_last.hr_trend_bpm_per_min > TS_TREND_DEADBAND_BPM_PER_MIN) {
      s_last.hr_trend = TS_TREND_RISING;
    } else if (s_last.hr_trend_bpm_per_min < -TS_TREND_DEADBAND_BPM_PER_MIN) {
      s_last.hr_trend = TS_TREND_FALLING;
    } else {
      s_last.hr_trend = TS_TREND_STEADY;
    }
  } else {
    s_last.hr_trend = TS_TREND_STEADY;
    s_last.hr_trend_bpm_per_min = 0.0f;
  }

  /* Flag whether the HR/SpO2 forecasts mean anything - see the long comment in
   * ts_monitor.h. The value is still computed when the channel has no signal
   * (handy when debugging), but the validity flag goes false so the layer above
   * does NOT put the number on screen. */
  s_last.hr_valid   = hr_ok;
  s_last.spo2_valid = spo2_ok;
  /* Only counts as a "real forecast" while the model is tracking that channel
   * (its own error below threshold). Full rationale in ts_monitor.h. */
  s_last.hr_forecast_trusted    = hr_ok && (ch_err[0] <= TS_POINT_THR);
  s_last.drops_forecast_trusted = (ch_err[2] <= TS_POINT_THR);
  s_last.hr_forecast_16s =
      fc[(TS_FC_HORIZON - 1) * TS_FC_N_CH + 0] * HR_SCALE + HR_CENTER;
  s_last.spo2_forecast_16s =
      fc[(TS_FC_HORIZON - 1) * TS_FC_N_CH + 1] * SPO2_SCALE + SPO2_CENTER;

  /* ---- Drip rate: trend + forecast ----------------------------------------
   * The model has emitted this channel (index 2) all along; it simply was not
   * surfaced. For an infusion device this is the most valuable trend of all:
   * drips slowing down signals a forming occlusion EARLIER than waiting for the
   * drop ratio to fall past its threshold.
   *
   * Channel 2 is a RATIO against the doctor's target, so it has to be multiplied
   * by target_dpm to get real drops per minute. */
  {
    float dr_first = 0.0f, dr_last = 0.0f;
    for (int h = 0; h < 4; h++) {
      dr_first += fc[h * TS_FC_N_CH + 2];
      dr_last  += fc[(TS_FC_HORIZON - 1 - h) * TS_FC_N_CH + 2];
    }
    /* normalised -> ratio -> drops per minute */
    dr_first = ((dr_first / 4.0f) * DR_SCALE + DR_CENTER) * target_dpm;
    dr_last  = ((dr_last  / 4.0f) * DR_SCALE + DR_CENTER) * target_dpm;

    s_last.drops_trend_dpm_per_min = (dr_last - dr_first) / 12.0f * 60.0f;
    if (s_last.drops_trend_dpm_per_min > TS_TREND_DEADBAND_DPM_PER_MIN) {
      s_last.drops_trend = TS_TREND_RISING;
    } else if (s_last.drops_trend_dpm_per_min < -TS_TREND_DEADBAND_DPM_PER_MIN) {
      s_last.drops_trend = TS_TREND_FALLING;
    } else {
      s_last.drops_trend = TS_TREND_STEADY;
    }

    s_last.drops_forecast_16s =
        (fc[(TS_FC_HORIZON - 1) * TS_FC_N_CH + 2] * DR_SCALE + DR_CENTER) * target_dpm;
    if (s_last.drops_forecast_16s < 0.0f) s_last.drops_forecast_16s = 0.0f;
  }

  /* ---- 5) Early warning ----------------------------------------------------
   * The forecast crosses a clinical limit within the next 16 seconds even though
   * the CURRENT reading is still inside it. This is the thing an instantaneous
   * threshold fundamentally cannot do.
   * Only channels with real signal are considered - otherwise the baseline fill
   * would be read as a "forecast" and manufacture a false warning. */
  s_last.early_warning = false;
  for (int h = 0; h < TS_FC_HORIZON; h++) {
    if (hr_ok) {
      float hr_h = fc[h * TS_FC_N_CH + 0] * HR_SCALE + HR_CENTER;
      if (hr_h < AI_HR_ABS_LOW || hr_h > AI_HR_ABS_HIGH) {
        s_last.early_warning = true;
        break;
      }
    }
    if (spo2_ok) {
      float sp_h = fc[h * TS_FC_N_CH + 1] * SPO2_SCALE + SPO2_CENTER;
      if (sp_h < AI_SPO2_ABS) {
        s_last.early_warning = true;
        break;
      }
    }
  }

  if (out) *out = s_last;
}
