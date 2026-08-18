/* ============================================================================
 *  ts_monitor.h — Monitoring layer built on the time-series FORECASTER
 *
 *  Difference from ai_monitor (the older model): ai_monitor looks at 6 features
 *  from a SINGLE instant. ts_monitor keeps a 64-SECOND WINDOW, runs the model
 *  once a second to forecast the next 16 seconds, and derives:
 *      - trend         : is the heart rate climbing / falling / steady
 *      - early warning : the forecast crosses a clinical limit BEFORE the
 *                        actual reading does
 *      - anomaly       : error between the forecast made one second ago and the
 *                        value actually measured now
 *
 *  Safety-critical detail: an anomaly may only raise an alarm after the score
 *  has stayed above threshold for K CONSECUTIVE steps (persistence). Evaluated
 *  on real ICU data (BIDMC): deciding instantly gives a 17.6% false-alarm rate
 *  on transient blips - worse than the old plain-threshold approach - while
 *  adding persistence brings it down to 2.3%. See
 *  AI_TIME_SERIES_TAT_TAN_TAT.md section 4.5.1.
 * ========================================================================== */
#ifndef TS_MONITOR_H
#define TS_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  TS_TREND_STEADY = 0,
  TS_TREND_RISING = 1,
  TS_TREND_FALLING = 2
} ts_trend_t;

typedef struct {
  bool  ready;              /* has the window collected its first 64 samples */
  bool  have_forecast;      /* did the most recent model run succeed */

  float anomaly_score;      /* per-channel forecast error, normalised, max over channels */
  bool  anomaly_confirmed;  /* score stayed above threshold for K consecutive steps */
  uint8_t persist_count;    /* how many consecutive steps are currently above threshold */

  ts_trend_t hr_trend;
  float hr_trend_bpm_per_min;   /* negative = falling */

  bool  early_warning;      /* forecast crosses a clinical limit within the next 16s */

  /* Forecast for the 16th second ahead.
   *
   * hr_valid / spo2_valid: whether that channel's forecast means anything at
   * all. When the PPG sensor loses signal, the channel is filled with a
   * baseline value so the model can still run for the remaining channels - the
   * model STILL emits a number for heart rate, but it is a forecast made from a
   * fake flat line, not from the patient. Showing that on a dashboard reads
   * exactly like a real measurement, so the validity flag has to travel with
   * the number. */
  bool  hr_valid;
  bool  spo2_valid;
  float hr_forecast_16s;    /* forecast heart rate at second 16 (bpm) */
  float spo2_forecast_16s;  /* forecast SpO2 at second 16 (%) */

  /* Drip rate - the most valuable trend for an infusion device: drops slowing
   * down is the early signature of an occlusion forming, and it shows up well
   * before the drop ratio falls past its alarm threshold. */
  ts_trend_t drops_trend;
  float drops_trend_dpm_per_min;   /* negative = slowing down */
  float drops_forecast_16s;        /* forecast drops per minute at second 16 */

  /* Is that channel's forecast TRUSTWORTHY?
   *
   * The model only ever learned NORMAL behaviour, so when a channel sits in a
   * sustained abnormal state it does not predict "what will happen" - it pulls
   * back towards "the normal level there should be". That is exactly what makes
   * anomaly DETECTION work (the gap is the signal), but presenting it on a
   * dashboard labelled "forecast for the next 16 seconds" misleads the reader
   * completely. Measured on the bench: with drips falling 34 -> 27 the model
   * kept reporting ~49, because 49 is the normal level for the configured
   * target.
   *
   * This flag means "that channel's own forecast error is still below
   * threshold", i.e. the model is tracking reality and the number can be read
   * as a real forecast. When it is false, the UI must relabel the figure as
   * "expected if normal" rather than "forecast". */
  bool hr_forecast_trusted;
  bool drops_forecast_trusted;
} ts_result_t;

/* Loads the model. Returns false on failure - the system then keeps running on
 * the clinical rules alone and merely loses the forecast (the AI is never
 * allowed to be a single point of failure for raising alarms). */
bool ts_monitor_init(void);

/* Call EXACTLY ONCE per AI cycle (1 second). Reads the current values from
 * sensor_hub, pushes them into the window, runs the model, updates *out. */
void ts_monitor_step(ts_result_t *out);

/* Most recent result without re-running the model (for logging / reporting). */
const ts_result_t *ts_monitor_last(void);

#endif /* TS_MONITOR_H */
