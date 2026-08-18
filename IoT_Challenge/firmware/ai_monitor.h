/* ============================================================================
 *  ai_monitor.h — Anomaly-detection layer for Smart IV (ICTU team)
 *  Overall idea:
 *    1) sensor_hub provides each channel's value + STATE (DISABLED / OK / LOST)
 *    2) ai_monitor assembles 6 features -> normalises -> runs the autoencoder
 *    3) Combines the autoencoder WITH clinical rules (OR): alarm if any
 *       condition holds, favouring recall over precision.
 *
 *  The 6 features (KEEP this order - it matches scaler.json / the int8 model):
 *    [0] heart_rate    (bpm)
 *    [1] spo2          (oxygen saturation, %)
 *    [2] flow_ratio    (measured flow / doctor's target)
 *    [3] drops_ratio   (measured drops per minute / doctor's target)
 *    [4] vital_missing (flag: HR or SpO2 lost AFTER the sensor was attached)
 *    [5] line_missing  (flag: FLOW or DROPS signal lost)
 * ========================================================================== */
#ifndef AI_MONITOR_H
#define AI_MONITOR_H

#include <stdint.h>

/* ===== Clinical rule parameters (taken from threshold.json after training) = */
#define AI_HR_PCT       0.30f   // HR more than 30% off the patient's own baseline
#define AI_HR_ABS_LOW   45.0f   // absolute floor (severe bradycardia)
#define AI_HR_ABS_HIGH  150.0f  // absolute ceiling (severe tachycardia)
#define AI_SPO2_ABS     90.0f   // SpO2 < 90% -> desaturation (absolute limit)
#define AI_FLOW_HI      1.5f    // > 1.5x target -> free flow
#define AI_FLOW_LO      0.3f    // < 0.3x target -> occlusion

/* Autoencoder reconstruction-error threshold (from threshold.json) */
#define AI_AE_THRESHOLD 1.4335810089111334f

/* Result of one evaluation pass */
typedef struct {
  int    alarm;          // 1 = alarm, 0 = normal
  float  recon_error;    // autoencoder reconstruction error (for debugging)
  uint8_t reason_ae;     // autoencoder above threshold
  uint8_t reason_hr;     // abnormal HR (percentage or absolute)
  uint8_t reason_spo2;   // SpO2 below threshold
  uint8_t reason_flow;   // flow/drops ratio outside its band
  uint8_t reason_missing;// signal lost (vitals or infusion line)
  float   feat[6];       // the 6 raw features (assembled, before normalising)
} ai_result_t;

/* Init: load the TFLM model and set the HR baseline (median of the first ~60s) */
void ai_monitor_init(void);

/* Call periodically (e.g. once a second): gather features, run, return result */
void ai_monitor_step(ai_result_t *out);

/* Calibrate the patient's personal HR baseline: call during the first ~60s
 * after attaching the sensor. If never called, the baseline defaults to
 * scaler.mean[0] (81.68). */
void ai_monitor_set_hr_baseline(float hr_base);

/* Current active HR baseline (bpm) - the value locked in at the last
 * calibration, so the HIS Server can show it alongside "captured at". */
float ai_monitor_get_hr_baseline(void);

#endif /* AI_MONITOR_H */
