/* ============================================================================
 *  app.c — AI Smart IV project (BRD2709A / EFR32xG26). PASTE this into app.c.
 *
 *  Architecture as required:
 *    - Channels that ARE connected (see #define in sensor_hub.h) -> read real data.
 *    - Channels NOT yet connected -> reported as "not present" (DISABLED), NO false alarms.
 *    - Adding a sensor later -> just flip its #define -> automatically read + monitored.
 *
 *  Main loop:
 *    - sensor_hub_poll(): called EVERY iteration (reads drops continuously, never misses a pulse).
 *    - Every 1 second: ai_monitor_step() -> runs the autoencoder + rules -> prints status.
 *
 *  COMPONENTS to install (.slcp -> SOFTWARE COMPONENTS):
 *    - IO Stream: EUSART (instance "vcom")  + IO Stream: STDIO   -> printf over VCOM
 *    - Sleep Timer                                               -> replaces millis()
 *    - Tensorflow Lite Micro                                     -> runs the AI model
 *  (When enabling more channels: I2CSPM for MAX30102, GPIO/uDelay for HX711... add later)
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include "sl_sleeptimer.h"
#include "sensor_hub.h"
#include "ai_monitor.h"
#include "ts_monitor.h"
#include "oled_display.h"

#include "app/framework/include/af.h"
#include "network-steering.h"
#include "app/framework/plugin/reporting/reporting.h"

/* Lấy thẳng version từ config OTA thay vì khai báo một hằng số riêng, để số
 * nháy LED KHÔNG BAO GIỜ lệch với version mà thiết bị khai báo với
 * Zigbee2MQTT. Chỉ cần đổi FIRMWARE_VERSION trong
 * config/ota-client-policy-config.h là cả hai tự đồng bộ. */
#include "ota-client-policy-config.h"
#define FW_VERSION  SL_ZIGBEE_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION

#define AI_PERIOD_MS    1000U   // AI run period (1 second)
#define HR_CALIB_MS    60000U   // first 60s: calibrate the per-patient HR baseline

/* Endpoint sending data over Zigbee (defined in config/zcl/zcl_config.zap):
 * a single custom "Smart IV Vitals" cluster (ZCL_SMART_IV_VITALS_CLUSTER_ID,
 * 0xFC01, mfgCode 0x1049) on a single endpoint, with 5 properly-named
 * attributes (HeartRate/Spo2/FlowRatio/DropRatio/AlarmBitmap) - replacing the
 * old approach of "borrowing" 5 standard measurement clusters
 * (Temperature/Humidity/Flow/Pressure/Illuminance Measurement) across 5
 * separate endpoints and repurposing their MeasuredValue. See
 * config/zcl/smart-iv-vitals.xml for the cluster definition. */
#define ZB_EP_VITALS       2
#define SMART_IV_MFG_CODE  0x1049

/* "No real data" sentinel values when sending over Zigbee - following the
 * standard ZCL convention per attribute type (NOT using a plausible bpm/%
 * number like HR_BASE_FILL anymore, to avoid a gateway/log reader mistaking
 * it for a real reading):
 *   - Temperature Measurement (int16s)      : 0x8000 = invalid value
 *   - Relative Humidity Measurement (uint16): 0xFFFF = invalid value
 * Both are standard ZCL special values that never collide with any real bpm/%. */
#define ZCL_HR_INVALID     ((int16_t)0x8000)
#define ZCL_SPO2_INVALID   ((uint16_t)0xFFFF)

static bool zb_join_started = false;

static uint32_t now_ms(void)
{
  return sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
}

/* HR baseline calibration window - file-scope (not a function-local static)
 * so it can be reset by a remote "recalibrate HR baseline" command (see
 * app_trigger_hr_recalibration()), not just run once automatically at
 * boot. calib_start_ms is the reference point the 60s window counts from -
 * initially 0 (i.e. starts immediately at boot, same as before), but reset
 * to the current uptime whenever recalibration is triggered later. */
static bool     calib_done      = false;
static uint32_t calib_start_ms  = 0;

/* Persistent count of completed HR baseline calibrations (never resets) -
 * exists alongside the one-shot hr_baseline_just_completed pulse for the
 * same reason as sensor_hub.c's tare_event_count: a transient pulse can be
 * missed if it fires before Zigbee reporting has been configured (which
 * only happens after network join completes). The HIS Server instead
 * detects "count increased since last seen" and stamps its own wall-clock
 * timestamp when it does - this can't race with boot timing. */
static uint8_t  hr_baseline_event_count = 0;

/* Re-arms the 60s HR baseline calibration window - same effect as a fresh
 * boot's automatic calibration, but callable at any time (e.g. from
 * sl_zigbee_af_post_attribute_change_cb() when the doctor sends a
 * "recalibrate HR baseline" command from the HIS Server). */
static void app_trigger_hr_recalibration(void)
{
  calib_start_ms = now_ms();
  calib_done     = false;
  printf("[HR] Recalibrating baseline - measuring for the next 60s...\r\n");
}

/* Live countdown for the HIS Server dashboard, so the doctor can see "Xs
 * remaining" instead of guessing when the 60s baseline window will finish.
 * Returns 0 once calibration is done (or hasn't started counting down from
 * a sane reference yet). */
static uint8_t app_hr_baseline_seconds_remaining(void)
{
  if (calib_done) {
    return 0;
  }
  uint32_t elapsed = now_ms() - calib_start_ms;
  if (elapsed >= HR_CALIB_MS) {
    return 0;
  }
  return (uint8_t)((HR_CALIB_MS - elapsed) / 1000U);
}

/* Maps the AI result onto the 3-level local indicator (green/yellow/red LED +
 * buzzer, driven by sensor_hub.c). The ESP8266 reference sketch distinguished
 * "warning" from "danger" using two separate HR bands; ai_monitor gives a
 * single binary reason_hr flag instead, so the same distinction is
 * reconstructed here from the raw HR against the ABSOLUTE limits in
 * ai_monitor.h:
 *   RED    - signal lost on an installed sensor, low SpO2, an infusion-line
 *            problem (occlusion / free-flow / drops stopped), or HR past the
 *            absolute bradycardia/tachycardia bounds. All of these need
 *            someone at the bedside NOW.
 *   YELLOW - "worth a look" only: HR merely deviating >AI_HR_PCT from the
 *            patient's own baseline while still inside the absolute bounds,
 *            or the autoencoder flagging an anomaly no explicit rule caught.
 *   GREEN  - no alarm at all.
 * Note this is deliberately INDEPENDENT of the alarm bitmap sent to the HIS
 * Server: the bitmap keeps carrying every individual reason, while the LEDs
 * only convey urgency. */
static alert_level_t alert_level_from_result(const ai_result_t *r,
                                            const ts_result_t *ts)
{
  /* ---- TIER 1: ABSOLUTE safety net - alarms IMMEDIATELY, no persistence ----
   * These three must never be delayed: loss of signal on a sensor that IS
   * attached, SpO2 below its absolute limit, and heart rate past its absolute
   * floor/ceiling. The alarm-fatigue literature supports delaying conditions
   * where a transient excursion is harmless - but that argument does NOT apply
   * to these three. */
  bool hr_critical = r->reason_hr
                     && (sh_hr_state() == CH_OK)
                     && (r->feat[0] < AI_HR_ABS_LOW || r->feat[0] > AI_HR_ABS_HIGH);

  if (r->reason_missing || r->reason_spo2 || hr_critical) {
    return ALERT_RED;
  }

  /* ---- TIER 2: anomaly CONFIRMED through persistence -----------------------
   * The forecaster's anomaly score must stay above threshold for TS_PERSIST_K
   * CONSECUTIVE steps before it may alarm. Measured on real ICU data (BIDMC):
   * deciding instantly gives a 17.6% false-alarm rate on transients - worse
   * than the old threshold-only approach at 17.2% - while persistence brings it
   * down to 2.3%. A sustained infusion-line anomaly needs someone at the
   * bedside, so it maps to RED. */
  if (ts->anomaly_confirmed) {
    return ALERT_RED;
  }

  /* Drip/flow ratio out of band, sustained (clinical rule) -> RED. */
  if (r->reason_flow) {
    return ALERT_RED;
  }

  /* ---- TIER 3: "getting worse, not yet dangerous" -> YELLOW ---------------
   * Early warning is the forecaster's own contribution: the forecast crosses a
   * clinical limit within the next 16 seconds even though the CURRENT reading
   * is still inside it. An instantaneous threshold cannot know this. It maps to
   * YELLOW rather than RED because it is a prediction, not yet a fact. */
  if (ts->early_warning) {
    return ALERT_YELLOW;
  }

  /* HR well off the patient's own baseline (but still inside the absolute
   * limits), or the older autoencoder flagging something -> worth a look. */
  if (r->alarm) {
    return ALERT_YELLOW;
  }

  return ALERT_GREEN;
}

/* Default reporting configuration for the attributes carrying AI data, so
 * the device automatically sends reports to the coordinator (zigbee2mqtt)
 * even before the Pi-side external converter gets a chance to configure its
 * own reporting. All share the same endpoint + cluster (only the
 * attributeId differs), and must declare the correct manufacturerCode since
 * this is a manufacturer-specific cluster. WeightG/DropsPerMin are raw
 * telemetry (so a doctor viewing the bed dashboard can see actual numbers,
 * not just percentages); TargetFlowMlH is reported too so the dashboard can
 * confirm a rate change was actually applied by the device. */
/* Upper bound on maxInterval for EVERY attribute below. The HIS Server flags
 * a bed "Offline" after Offline.ThresholdSeconds (90s, see appsettings.json)
 * without a single update, so a quiet attribute must still report well
 * inside that window or a perfectly healthy bed would look disconnected. */
#define ZB_REPORT_MAX_INTERVAL_S   60

/* Per-attribute reporting cadence. A single flat setting for all 13
 * attributes (previously minInterval=1s + reportableChange=1 across the
 * board) made the device transmit almost continuously: the load cell's
 * reading jitters by ~1g even under a perfectly steady load, so WeightG
 * alone re-reported every second, and the coordinator answers each report
 * with its own Default Response. That is wasted 2.4GHz airtime shared with
 * WiFi, it raises the collision rate, and it scales badly once several beds
 * share one coordinator.
 *
 * The fix is NOT a uniformly slower cadence - that would delay alarms, which
 * is unacceptable here. Instead each attribute is tuned to its actual role:
 *
 *   - AlarmBitmap keeps minInterval=1 / change=1: a patient alarm must go
 *     out on the very next tick it appears, no throttling whatsoever.
 *   - Vitals (HR/SpO2) stay responsive but are bounded to one report per 2s,
 *     which is already the rate the MAX30102 window publishes at anyway.
 *   - Continuous telemetry (ratios, weight, drop rate) gets a deadband
 *     LARGER than the sensor's own noise, so a steady reading stops
 *     re-reporting: 5g on the load cell sits above its ~1g jitter, and 5
 *     percentage points on the ratios is well below anything clinically
 *     interesting (the alarm thresholds are 30%/150% of target).
 *   - Settings/counters (targets, baseline, tare/recalibration counts)
 *     change only when a doctor acts, so they keep minInterval=1 / change=1
 *     and confirm back to the dashboard instantly - they cost nothing when
 *     idle precisely because they rarely change.
 *
 * maxInterval stays at 60s for everything, so even a completely unchanging
 * attribute still refreshes the server's "last seen" clock. */
typedef struct {
  uint16_t attributeId;
  uint16_t minIntervalS;
  uint16_t reportableChange;
} zb_report_cfg_t;

static void zb_configure_reporting(void)
{
  static const zb_report_cfg_t reportCfgs[] = {
    /* Alarms: never throttled. */
    { ZCL_ALARM_BITMAP_ATTRIBUTE_ID,                1,  1 },

    /* Vitals: responsive, but no faster than the sensor actually updates. */
    { ZCL_HEART_RATE_ATTRIBUTE_ID,                  2,  1 },
    { ZCL_SPO2_ATTRIBUTE_ID,                        2,  1 },

    /* Continuous telemetry: deadband set above each sensor's own noise. */
    { ZCL_FLOW_RATIO_ATTRIBUTE_ID,                  5,  5 },   /* units of 1% */
    { ZCL_DROP_RATIO_ATTRIBUTE_ID,                  5,  5 },   /* units of 1% */
    { ZCL_WEIGHT_G_ATTRIBUTE_ID,                   10,  5 },   /* grams */
    { ZCL_DROPS_PER_MIN_ATTRIBUTE_ID,               5,  1 },   /* drops/min */

    /* Settings + event counters: only move when a doctor acts -> instant. */
    { ZCL_TARGET_FLOW_ML_H_ATTRIBUTE_ID,            1,  1 },
    { ZCL_TARGET_DROPS_PER_MIN_ATTRIBUTE_ID,        1,  1 },
    { ZCL_HR_BASELINE_BPM_ATTRIBUTE_ID,             1,  1 },
    { ZCL_TARE_EVENT_COUNT_ATTRIBUTE_ID,            1,  1 },
    { ZCL_HR_BASELINE_EVENT_COUNT_ATTRIBUTE_ID,     1,  1 },

    /* Live 60s countdown - ticks once a second while calibrating, then sits
     * at 0. Bounded to 5s so the dashboard shows progress without sending a
     * frame for every single second of the window. */
    { ZCL_HR_BASELINE_SECONDS_REMAINING_ATTRIBUTE_ID, 5, 1 },

    /* Forecaster output. TsFlags carries the trend/anomaly STATE, so it goes
     * out as soon as it changes, like any other alarm-adjacent value. The
     * numbers next to it are re-forecast every second and would otherwise
     * undo the 120 -> 12 messages/minute reduction described above all by
     * themselves, so each gets a deadband big enough that only a movement
     * worth reading crosses it: 2 bpm on a heart-rate forecast, 1% on SpO2,
     * 2 drops/min, 5 bpm/min of slope, and 0.5 (50 = 0.50 x100) of anomaly
     * score against a threshold of 5.61. */
    { ZCL_TS_FLAGS_ATTRIBUTE_ID,                    1,  1 },
    { ZCL_HR_FORECAST_16S_ATTRIBUTE_ID,             5,  2 },   /* bpm */
    { ZCL_SPO2_FORECAST_16S_ATTRIBUTE_ID,           5,  1 },   /* % */
    { ZCL_HR_TREND_BPM_PER_MIN_ATTRIBUTE_ID,        5,  5 },   /* bpm/min */
    { ZCL_TS_ANOMALY_SCORE_X100_ATTRIBUTE_ID,       5, 50 },   /* score x100 */
    { ZCL_DROPS_FORECAST_16S_ATTRIBUTE_ID,          5,  2 },   /* drops/min */
    { ZCL_DROPS_TREND_DPM_PER_MIN_ATTRIBUTE_ID,     5,  5 },   /* dpm/min */
  };

  for (uint8_t i = 0; i < sizeof(reportCfgs) / sizeof(reportCfgs[0]); i++) {
    sl_zigbee_af_plugin_reporting_entry_t reportingEntry;
    reportingEntry.direction = SL_ZIGBEE_ZCL_REPORTING_DIRECTION_REPORTED;
    reportingEntry.endpoint = ZB_EP_VITALS;
    reportingEntry.clusterId = ZCL_SMART_IV_VITALS_CLUSTER_ID;
    reportingEntry.attributeId = reportCfgs[i].attributeId;
    reportingEntry.mask = CLUSTER_MASK_SERVER;
    reportingEntry.manufacturerCode = SMART_IV_MFG_CODE;
    reportingEntry.data.reported.minInterval = reportCfgs[i].minIntervalS;
    reportingEntry.data.reported.maxInterval = ZB_REPORT_MAX_INTERVAL_S;
    reportingEntry.data.reported.reportableChange = reportCfgs[i].reportableChange;
    sl_zigbee_af_reporting_configure_reported_attribute(&reportingEntry);
  }
}

/* Writes the AI values + raw telemetry into the custom Smart IV Vitals
 * cluster -> the Reporting plugin will automatically send a report when a
 * value changes (per the configuration above). Uses the "manufacturer
 * specific" write function (as opposed to the regular one) since the
 * cluster has an mfgCode. */
static void zb_report_ai_result(int16_t hr, uint16_t spo2, uint16_t flow_x100,
                                 int16_t drop_x100, uint16_t alarm_bitmap,
                                 uint16_t weight_g, uint16_t drops_per_min,
                                 uint16_t target_flow_ml_h, uint16_t target_drops_per_min,
                                 uint8_t hr_baseline_seconds_remaining, uint16_t hr_baseline_bpm,
                                 uint8_t tare_event_count, uint8_t hr_baseline_event_count)
{
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HEART_RATE_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr, ZCL_INT16S_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_SPO2_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&spo2, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_FLOW_RATIO_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&flow_x100, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_DROP_RATIO_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&drop_x100, ZCL_INT16S_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_ALARM_BITMAP_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&alarm_bitmap, ZCL_BITMAP16_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_WEIGHT_G_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&weight_g, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_DROPS_PER_MIN_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&drops_per_min, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TARGET_FLOW_ML_H_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&target_flow_ml_h, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TARGET_DROPS_PER_MIN_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&target_drops_per_min, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_BASELINE_SECONDS_REMAINING_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr_baseline_seconds_remaining, ZCL_INT8U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_BASELINE_BPM_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr_baseline_bpm, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TARE_EVENT_COUNT_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&tare_event_count, ZCL_INT8U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_BASELINE_EVENT_COUNT_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr_baseline_event_count, ZCL_INT8U_ATTRIBUTE_TYPE);
}

/* Writes the on-chip forecaster's output into the same custom cluster.
 *
 * Kept separate from zb_report_ai_result() only because that function's
 * argument list is already at thirteen; both run on the same 1s tick.
 *
 * Before these attributes existed the forecaster's result reached the server
 * ONLY through the USB-serial fallback gateway (tools/serial_gateway.py): the
 * Zigbee path carried four trend bits inside AlarmBitmap and no numbers at
 * all, so a bed running the normal way - through the Pi - left the "AI
 * forecast (on-chip)" card stuck on "Collecting the first 64 seconds..."
 * forever, no matter how long the device had been up.
 *
 * A forecast the model itself cannot vouch for is sent as TS_FORECAST_INVALID
 * rather than as a number. When a PPG channel drops out it is filled with a
 * baseline so the model can keep running for the other channels; it still
 * emits a heart rate, but one computed from a flat fake line. On a dashboard
 * that is indistinguishable from a real reading - the same reason the serial
 * JSON path sends null there. */
#define TS_FORECAST_INVALID  0xFFFFu

static void zb_report_ts_result(const ts_result_t *ts)
{
  const bool usable = (ts->ready && ts->have_forecast);

  uint16_t flags = (uint16_t)((usable                 ? 0x0001u : 0u)
                              | (ts->anomaly_confirmed ? 0x0002u : 0u)
                              | (ts->early_warning     ? 0x0004u : 0u)
                              | (((uint16_t)ts->hr_trend    & 0x3u) << 3)
                              | (((uint16_t)ts->drops_trend & 0x3u) << 5)
                              | (ts->hr_forecast_trusted    ? 0x0080u : 0u)
                              | (ts->drops_forecast_trusted ? 0x0100u : 0u));

  uint16_t hr_forecast = (usable && ts->hr_valid)
                         ? (uint16_t)(ts->hr_forecast_16s + 0.5f) : TS_FORECAST_INVALID;
  uint16_t spo2_forecast = (usable && ts->spo2_valid)
                           ? (uint16_t)(ts->spo2_forecast_16s + 0.5f) : TS_FORECAST_INVALID;
  uint16_t drops_forecast = usable
                            ? (uint16_t)(ts->drops_forecast_16s + 0.5f) : TS_FORECAST_INVALID;
  int16_t  hr_slope    = (int16_t)ts->hr_trend_bpm_per_min;
  int16_t  drops_slope = (int16_t)ts->drops_trend_dpm_per_min;
  /* x100 keeps two decimals over an integer-only wire format, matching the
   * tsAnomalyScoreX100 the server already parses from the serial path. */
  uint16_t score_x100  = (uint16_t)(ts->anomaly_score * 100.0f + 0.5f);

  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TS_FLAGS_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&flags, ZCL_BITMAP16_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_FORECAST_16S_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr_forecast, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_SPO2_FORECAST_16S_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&spo2_forecast, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_TREND_BPM_PER_MIN_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&hr_slope, ZCL_INT16S_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TS_ANOMALY_SCORE_X100_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&score_x100, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_DROPS_FORECAST_16S_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&drops_forecast, ZCL_INT16U_ATTRIBUTE_TYPE);
  sl_zigbee_af_write_manufacturer_specific_server_attribute(
      ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_DROPS_TREND_DPM_PER_MIN_ATTRIBUTE_ID,
      SMART_IV_MFG_CODE, (uint8_t *)&drops_slope, ZCL_INT16S_ATTRIBUTE_TYPE);
}

/** @brief Post Attribute Change
 *
 * Fires for ANY attribute write, regardless of cluster - including a
 * network "Write Attributes" command from the coordinator (i.e. the doctor
 * changing a setting or triggering an action from the HIS Server, relayed
 * down through zigbee2mqtt as a Zigbee attribute write). We only care about
 * writes to our own cluster's settings/command attributes:
 *   - TargetFlowMlH / TargetDropsPerMin: apply the new target immediately.
 *   - TareCommand / HrRecalibrateCommand: any nonzero write is a "fire" -
 *     trigger the action, then write the attribute back to 0 so the NEXT
 *     write of the same value (e.g. the doctor pressing the button again)
 *     is still seen as a 0->nonzero change and fires again.
 */
void sl_zigbee_af_post_attribute_change_cb(uint8_t endpoint,
                                           sl_zigbee_af_cluster_id_t clusterId,
                                           sl_zigbee_af_attribute_id_t attributeId,
                                           uint8_t mask,
                                           uint16_t manufacturerCode,
                                           uint8_t type,
                                           uint8_t size,
                                           uint8_t *value)
{
  (void)mask;
  (void)type;
  (void)size;

  if (endpoint != ZB_EP_VITALS
      || clusterId != ZCL_SMART_IV_VITALS_CLUSTER_ID
      || manufacturerCode != SMART_IV_MFG_CODE) {
    return;
  }

  if (attributeId == ZCL_TARGET_FLOW_ML_H_ATTRIBUTE_ID) {
    uint16_t new_target_ml_h;
    memcpy(&new_target_ml_h, value, sizeof(new_target_ml_h));

    /* zb_report_ai_result() also writes this SAME attribute every AI tick
     * (just to report the currently-active value back up for dashboard
     * confirmation), which would otherwise trigger this callback and log a
     * misleading "doctor changed it" message once a second even when
     * nothing changed. Only react when the incoming value actually differs
     * from what's already active. */
    if ((uint16_t)(sh_target_flow_ml_h() + 0.5f) == new_target_ml_h) {
      return;
    }
    sh_set_target_flow_ml_h((float)new_target_ml_h);
    printf("[ZB] Doctor set a new target infusion rate: %u ml/h\r\n", (unsigned)new_target_ml_h);
    return;
  }

  if (attributeId == ZCL_TARGET_DROPS_PER_MIN_ATTRIBUTE_ID) {
    uint16_t new_target_dpm;
    memcpy(&new_target_dpm, value, sizeof(new_target_dpm));

    if ((uint16_t)(sh_target_drops_per_min() + 0.5f) == new_target_dpm) {
      return;   // same "reported back every tick" self-write issue as above
    }
    sh_set_target_drops_per_min((float)new_target_dpm);
    printf("[ZB] Doctor set a new target drop rate: %u dpm\r\n", (unsigned)new_target_dpm);
    return;
  }

  if (attributeId == ZCL_TARE_COMMAND_ATTRIBUTE_ID) {
    if (*value != 0) {
      sh_flow_trigger_tare();
      uint8_t reset_value = 0;
      sl_zigbee_af_write_manufacturer_specific_server_attribute(
          ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_TARE_COMMAND_ATTRIBUTE_ID,
          SMART_IV_MFG_CODE, &reset_value, ZCL_INT8U_ATTRIBUTE_TYPE);
    }
    return;
  }

  if (attributeId == ZCL_HR_RECALIBRATE_COMMAND_ATTRIBUTE_ID) {
    if (*value != 0) {
      app_trigger_hr_recalibration();
      uint8_t reset_value = 0;
      sl_zigbee_af_write_manufacturer_specific_server_attribute(
          ZB_EP_VITALS, ZCL_SMART_IV_VITALS_CLUSTER_ID, ZCL_HR_RECALIBRATE_COMMAND_ATTRIBUTE_ID,
          SMART_IV_MFG_CODE, &reset_value, ZCL_INT8U_ATTRIBUTE_TYPE);
    }
    return;
  }
}

/** @brief Stack Status
 *
 * Starts joining the network (Network Steering) when there is no network
 * yet; once the network is up, configures reporting for the 5 AI attributes.
 */
void sl_zigbee_af_stack_status_cb(sl_status_t status)
{
  if (status == SL_STATUS_NETWORK_DOWN) {
    if (!zb_join_started) {
      zb_join_started = true;
      sl_status_t joinStatus = sl_zigbee_af_network_steering_start();
      printf("[ZB] Starting network join: 0x%02X\r\n", (unsigned)joinStatus);
    }
  } else if (status == SL_STATUS_NETWORK_UP) {
    zb_configure_reporting();
    printf("[ZB] Joined the Zigbee network, reporting configured.\r\n");
  }
}

/** @brief Network Steering Complete
 *
 * Allows retrying the join if this attempt failed (e.g. permit-join was not
 * enabled on the coordinator).
 */
void sl_zigbee_af_network_steering_complete_cb(sl_status_t status,
                                               uint8_t totalBeacons,
                                               uint8_t joinAttempts,
                                               uint8_t finalState)
{
  (void)totalBeacons;
  (void)joinAttempts;
  (void)finalState;

  printf("[ZB] Network join result: 0x%02X\r\n", (unsigned)status);
  if (status != SL_STATUS_OK) {
    zb_join_started = false;   // allow sl_zigbee_af_stack_status_cb to retry
  }
}

/* ============================================================================
 *  Bedside OLED (1.3" 128x64, I2C, shares PC05/PC07 with the MAX30102)
 *
 *  Driven from here rather than from sensor_hub.c because what belongs on the
 *  screen is the same thing that goes into the alarm bitmap and out over
 *  Zigbee - the readings AND the reasons - and this function is where those
 *  already exist. sensor_hub only lends out the bus (sh_i2c_write).
 *
 *  A missing or unplugged panel must never affect monitoring: if the module
 *  does not ACK, oled_ready stays false, one line is printed, and the rest of
 *  the firmware carries on untouched. Re-init is retried on a slow timer so a
 *  panel plugged in later (or one that dropped off the bus) comes back on its
 *  own, without a reset - a reset would cost the HR baseline and the tare.
 * ========================================================================== */
#define OLED_RETRY_INTERVAL_MS  10000U

static oled_display_t bedside_oled;
static bool           oled_ready         = false;
static uint32_t       oled_last_retry_ms = 0;

static bool oled_bus_write(void *context, uint8_t address, uint8_t control,
                           const uint8_t *data, uint8_t length)
{
  (void)context;
  return sh_i2c_write(address, control, data, length);
}

static void oled_try_init(void)
{
  /* The retry below runs every 10s forever when no panel is fitted, so the
   * "not found" line is printed once per absence, not once per attempt: a
   * repeating line would bury the AI/HX711 output that VCOM is actually for. */
  static bool reported_missing = false;

  const oled_bus_t bus = { .write = oled_bus_write, .context = NULL };
  oled_ready = oled_display_init(&bedside_oled, &bus);
  if (oled_ready) {
    reported_missing = false;
    printf("[OLED] Bedside display ready (128x64)\r\n");
  } else if (!reported_missing) {
    reported_missing = true;
    printf("[OLED] No panel at 0x3C/0x3D - running without the bedside screen\r\n");
  }
}

static void oled_update(const ai_result_t *r, const ts_result_t *ts,
                        bool hr_ok, int hr, bool spo2_ok, int spo2,
                        int flow_pct, uint32_t now)
{
  if (!oled_ready) {
    if (now - oled_last_retry_ms >= OLED_RETRY_INTERVAL_MS) {
      oled_last_retry_ms = now;
      oled_try_init();
    }
    return;
  }

  oled_vitals_t v = {
    .hr_valid   = hr_ok,
    .hr_bpm     = (uint16_t)(hr   < 0 ? 0 : hr),
    .spo2_valid = spo2_ok,
    .spo2_pct   = (uint16_t)(spo2 < 0 ? 0 : spo2),
    .flow_valid = (sh_flow_state() == CH_OK),
    .flow_pct   = (int16_t)flow_pct,
    /* Same condition app.c uses for the serial "*** ALARM ***" line and the
     * alarm bitmap, so the screen, the log and the ward console can never
     * disagree about whether this bed is alarming. */
    .alarm          = (r->alarm || ts->anomaly_confirmed || ts->early_warning),
    .reason_spo2    = r->reason_spo2,
    .reason_hr      = r->reason_hr,
    .reason_flow    = r->reason_flow,
    .reason_missing = r->reason_missing,
    .reason_ai      = (r->reason_ae || ts->anomaly_confirmed || ts->early_warning)
  };

  if (!oled_display_show_vitals(&bedside_oled, &v)) {
    /* Stopped ACKing - cable pulled, or the bus glitched. Fall back to the
     * retry path above instead of writing into a panel that isn't there. */
    oled_ready = false;
    oled_last_retry_ms = now;
    printf("[OLED] Display stopped responding - will retry\r\n");
  }
}

/* ================= CALLED ONCE AT STARTUP ================= */
void app_init(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);   // disable buffering -> printf shows up immediately

  sensor_hub_init();

  /* Dấu hiệu nhận biết version bằng mắt: nháy cả 3 LED đúng bằng số version
   * đang chạy (v3 -> 3 nháy). Nhìn là biết OTA đã lên bản mới hay chưa mà
   * không cần cắm VCOM. Phải gọi SAU sensor_hub_init() vì chân LED được cấu
   * hình trong đó. Tốn FW_VERSION * 300 ms ở lúc boot, chấp nhận được. */
  printf("\r\n[BOOT] Smart IV firmware v%d - nhay %d lan\r\n",
         (int)FW_VERSION, (int)FW_VERSION);
  sh_alert_blink_all((uint8_t)FW_VERSION, 150, 150);

  ai_monitor_init();
  oled_try_init();

  /* Load the time-series forecaster. On failure the system STILL runs normally
   * on the clinical rules and merely loses trend / early warning. The AI must
   * never be a single point of failure for the device's ability to alarm. */
  if (ts_monitor_init()) {
    printf("[TS] Time-series forecasting: ON (64s window, 16s horizon)\r\n");
  } else {
    printf("[TS] Forecaster failed to load - running on clinical rules only\r\n");
  }


  printf("\r\n=== Smart IV - AI module ready ===\r\n");
  printf("Channels: DROPS=%s HR=%s SpO2=%s FLOW=%s\r\n",
         DROPS_ENABLED ? "ON" : "OFF",
         HR_ENABLED    ? "ON" : "OFF",
         SPO2_ENABLED  ? "ON" : "OFF",
         FLOW_ENABLED  ? "ON" : "OFF");
  printf("Model: int8 6-feature autoencoder + clinical rules.\r\n\r\n");
}

/* ====== CALLED REPEATEDLY (like Arduino's loop()) ====== */
void app_process_action(void)
{
  /* 1) Continuously read the drop sensor — NO delay here (drop pulses are very short). */
  sensor_hub_poll();

  uint32_t now = now_ms();

  /* 2) Calibrate the HR baseline during the first 60s after boot OR after a
   *    remote recalibration trigger (only meaningful when HR_ENABLED=1).
   *    While the HR channel isn't connected, sh_hr() returns the baseline
   *    value so the baseline stays at the default. calib_done/calib_start_ms
   *    are file-scope (see above) so app_trigger_hr_recalibration() can
   *    re-arm this window at any time, not just once at boot. */
  static bool hr_baseline_just_completed = false;
  if (!calib_done && (now - calib_start_ms) < HR_CALIB_MS) {
    /* Waiting for the settling window; currently HR is DISABLED so this is safely skipped. */
  } else if (!calib_done) {
    ai_monitor_set_hr_baseline(sh_hr());  // lock in the baseline after 60s
    calib_done = true;
    hr_baseline_just_completed = true;   // one-shot pulse, consumed below in the same tick it fires
    hr_baseline_event_count++;           // persistent, wraps 255->0 harmlessly - see comment above
    printf("[HR] 60s baseline sample complete.\r\n");
  }

  /* 3) Every AI_PERIOD_MS: run the AI + print status. */
  static uint32_t last_ai = 0;
  if (now - last_ai >= AI_PERIOD_MS) {
    last_ai = now;

    ai_result_t r;
    ai_monitor_step(&r);

    /* Time-series forecaster: pushes this second's sample into the 64s window,
     * forecasts the next 16s, and reports trend / early warning / a
     * persistence-confirmed anomaly. Must run BEFORE the alarm decision below,
     * which consumes its result. */
    ts_result_t ts;
    ts_monitor_step(&ts);

    /* Drive the local LEDs/buzzer straight away, BEFORE the printf/Zigbee
     * work below - the bedside indicator should never wait on the network. */
    sh_alert_set_level(alert_level_from_result(&r, &ts));

    /* Only treat HR/SpO2 as REAL numbers when the channel is CH_OK (fresh
     * sample from a real chip). CH_LOST/CH_DISABLED -> do NOT print/send the
     * fake baseline numbers (81/97) anymore, since that would look like a
     * real reading while nothing has actually been read yet - print "--"
     * and send the standard ZCL "invalid" value instead. */
    bool hr_ok   = (sh_hr_state()   == CH_OK);
    bool spo2_ok = (sh_spo2_state() == CH_OK);

    /* Compact print: values + result. Scaled by *1000 / rounded to avoid printf %f. */
    int hr   = (int)(r.feat[0] + 0.5f);
    int spo2 = (int)(r.feat[1] + 0.5f);
    int flow = (int)(r.feat[2] * 100.0f + 0.5f);   // %
    int drop = (int)(r.feat[3] * 100.0f + 0.5f);   // %
    int err  = (int)(r.recon_error * 1000.0f + 0.5f);

    char hr_str[8], spo2_str[8];
    if (hr_ok)   { snprintf(hr_str,   sizeof(hr_str),   "%d", hr);   } else { snprintf(hr_str,   sizeof(hr_str),   "--"); }
    if (spo2_ok) { snprintf(spo2_str, sizeof(spo2_str), "%d", spo2); } else { snprintf(spo2_str, sizeof(spo2_str), "--"); }

    /* Bedside screen, updated on the same tick and from the same values as
     * the serial log below and the Zigbee report further down. It only
     * repaints when the displayed content actually changes, so a steady bed
     * costs nothing on the shared I2C bus. */
    oled_update(&r, &ts, hr_ok, hr, spo2_ok, spo2, flow, now);

    printf("[AI] HR=%s SpO2=%s Flow=%d%% Drop=%d%% (dpm=%d) | err=%d/1000 | ",
           hr_str, spo2_str, flow, drop, (int)(sh_drops_per_min() + 0.5f), err);

    if (r.alarm || ts.anomaly_confirmed || ts.early_warning) {
      printf("*** ALARM ***");
      if (r.reason_missing)     printf(" [SIGNAL_LOST]");
      if (r.reason_spo2)        printf(" [LOW_SpO2]");
      if (r.reason_hr)          printf(" [HEART_RATE]");
      if (r.reason_flow)        printf(" [INFUSION_LINE]");
      if (r.reason_ae)          printf(" [AE]");
      if (ts.anomaly_confirmed) printf(" [TS_ANOMALY]");
      if (ts.early_warning)     printf(" [EARLY_WARN]");
      printf("\r\n");
    } else {
      printf("Normal\r\n");
    }

    /* A dedicated log line for the forecaster: trend, anomaly score and how
     * many consecutive steps are above threshold. Very useful when diagnosing
     * at the bedside - it shows what the model is "thinking" before it
     * alarms. */
    if (ts.ready && ts.have_forecast) {
      const char *tr = (ts.hr_trend == TS_TREND_RISING)  ? "TANG"
                     : (ts.hr_trend == TS_TREND_FALLING) ? "GIAM" : "on dinh";
      printf("[TS] HR trend %s (%d bpm/min) | forecast +16s: HR=%d SpO2=%d"
             " | score=%d/1000 (threshold %d) | persist %u/%u\r\n",
             tr, (int)ts.hr_trend_bpm_per_min,
             (int)(ts.hr_forecast_16s + 0.5f), (int)(ts.spo2_forecast_16s + 0.5f),
             (int)(ts.anomaly_score * 1000.0f), (int)(5.6111f * 1000.0f),
             (unsigned)ts.persist_count, (unsigned)11u);
    } else if (!ts.ready) {
      printf("[TS] Filling the 64-second window...\r\n");
    }

    /* Alarm bitmap: bit0=signal lost, bit1=low SpO2, bit2=heart rate,
     * bit3=infusion line, bit4=autoencoder.
     * bit5-8: per-channel CONNECTION status (1 = has signal/CH_OK, 0 = not
     * yet connected or signal lost) - so the gateway/app can tell "sensor
     * not installed yet" apart from "installed but currently alarming".
     * bit9=loadcell tare in progress, bit10=tare just completed (one-shot),
     * bit11=HR 60s baseline just completed (one-shot) - lets the doctor/
     * nurse see these events on the bed dashboard instead of only in the
     * device's own serial log.
     * bit12-15: TIME-SERIES forecaster output (new). Packed into the SPARE bits
     * of the existing bitmap on purpose - no ZCL schema change, so the gateway
     * and server keep working unchanged, and the converter only needs new
     * decode lines. Dedicated numeric attributes (HrTrendBpmPerMin,
     * AnomalyScore) can come later if the dashboard needs the actual figures.
     *   bit12 = persistence-confirmed anomaly
     *   bit13 = HR trending UP     (> +10 bpm/min over the forecast)
     *   bit14 = HR trending DOWN   (< -10 bpm/min)
     *   bit15 = early warning: forecast crosses a clinical limit within 16s */
    bool tare_just_done = sh_flow_tare_just_completed();   // one-shot: consumes itself here
    uint16_t alarm_bitmap = (uint16_t)((r.reason_missing ? 0x01 : 0)
                                        | (r.reason_spo2    ? 0x02 : 0)
                                        | (r.reason_hr      ? 0x04 : 0)
                                        | (r.reason_flow    ? 0x08 : 0)
                                        | (r.reason_ae      ? 0x10 : 0)
                                        | (sh_hr_state()    == CH_OK ? 0x20  : 0)
                                        | (sh_spo2_state()  == CH_OK ? 0x40  : 0)
                                        | (sh_flow_state()  == CH_OK ? 0x80  : 0)
                                        | (sh_drops_state() == CH_OK ? 0x100 : 0)
                                        | (sh_flow_tare_in_progress() ? 0x200 : 0)
                                        | (tare_just_done              ? 0x400 : 0)
                                        | (hr_baseline_just_completed  ? 0x800 : 0)
                                        | (ts.anomaly_confirmed        ? 0x1000 : 0)
                                        | (ts.hr_trend == TS_TREND_RISING  ? 0x2000 : 0)
                                        | (ts.hr_trend == TS_TREND_FALLING ? 0x4000 : 0)
                                        | (ts.early_warning            ? 0x8000 : 0));
    hr_baseline_just_completed = false;   // one-shot: consumed here, same tick it was set

    /* ===== JSON telemetry line on VCOM =======================================
     * One JSON line per AI cycle so a gateway running on a PC can read it
     * straight off USB-serial and forward it to the HIS Server - a fallback path
     * for when the Raspberry Pi
     * (which normally runs zigbee2mqtt + the gateway) is unavailable. The
     * Zigbee path keeps running alongside, unaffected.
     *
     * Field names deliberately match the existing zigbee2mqtt payload: the
     * server-side BedDataParser already accepts those aliases, so nothing on
     * the server needs changing. A channel without signal is sent as null, not
     * 0, following the convention used throughout: 0 bpm reads like a patient
     * in cardiac arrest, whereas null means "no measurement". */
    printf("[JSON]{\"heart_rate\":");
    if (hr_ok) printf("%d", hr); else printf("null");
    printf(",\"spo2\":");
    if (spo2_ok) printf("%d", spo2); else printf("null");
    printf(",\"flow\":%d,\"drop_rate\":%d,\"drops_per_min\":%d,\"weight_g\":%d"
           ",\"target_flow_ml_h\":%d,\"target_drops_per_min\":%d"
           ",\"hr_signal\":%s,\"spo2_signal\":%s,\"flow_signal\":%s,\"drops_signal\":%s"
           ",\"line_blocked\":%s,\"ae_alarm\":%s,\"alarm\":%s"
           ",\"tare_in_progress\":%s,\"hr_baseline_bpm\":%d"
           ",\"hr_baseline_seconds_remaining\":%d"
           ",\"tare_event_count\":%d,\"hr_baseline_event_count\":%d"
           ",\"ts_anomaly\":%s,\"ts_trend\":%d,\"ts_early_warning\":%s"
           ",\"ts_ready\":%s,\"hr_trend_bpm_per_min\":%d,\"ts_anomaly_score\":%d"
           ",\"drops_trend\":%d,\"drops_trend_dpm_per_min\":%d"
           ",\"drops_forecast_16s\":%d"
           ",\"hr_forecast_trusted\":%s,\"drops_forecast_trusted\":%s",
           flow, drop,
           (int)(sh_drops_per_min() + 0.5f),
           (int)(sh_flow_weight_g() < 0.0f ? 0.0f : sh_flow_weight_g() + 0.5f),
           (int)(sh_target_flow_ml_h() + 0.5f),
           (int)(sh_target_drops_per_min() + 0.5f),
           (sh_hr_state()    == CH_OK) ? "true" : "false",
           (sh_spo2_state()  == CH_OK) ? "true" : "false",
           (sh_flow_state()  == CH_OK) ? "true" : "false",
           (sh_drops_state() == CH_OK) ? "true" : "false",
           r.reason_flow    ? "true" : "false",
           r.reason_ae      ? "true" : "false",
           (r.alarm || ts.anomaly_confirmed) ? "true" : "false",
           sh_flow_tare_in_progress() ? "true" : "false",
           (int)(ai_monitor_get_hr_baseline() + 0.5f),
           (int)app_hr_baseline_seconds_remaining(),
           (int)sh_flow_tare_event_count(),
           (int)hr_baseline_event_count,
           ts.anomaly_confirmed ? "true" : "false",
           (int)ts.hr_trend,
           ts.early_warning ? "true" : "false",
           (ts.ready && ts.have_forecast) ? "true" : "false",
           (int)ts.hr_trend_bpm_per_min,
           /* score x100 to keep 2 decimals over an integer-only wire format */
           (int)(ts.anomaly_score * 100.0f + 0.5f),
           (int)ts.drops_trend,
           (int)ts.drops_trend_dpm_per_min,
           (int)(ts.drops_forecast_16s + 0.5f),
           ts.hr_forecast_trusted    ? "true" : "false",
           ts.drops_forecast_trusted ? "true" : "false");

    /* The HR/SpO2 forecasts are only sent as NUMBERS while that channel really
     * has signal; otherwise null. When the PPG sensor drops out, the channel is
     * filled with a baseline value so the model can still run for the other
     * channels - the model STILL emits a number for HR, but it is a forecast
     * made from a fake flat line, not from the patient. Shown on a dashboard it
     * looks exactly like a real reading (observed: taking a finger off the
     * sensor while "HR in 16s" kept changing), so null is sent instead and the
     * UI hides it. */
    printf(",\"hr_forecast_16s\":");
    if (ts.ready && ts.have_forecast && ts.hr_valid) {
      printf("%d", (int)(ts.hr_forecast_16s + 0.5f));
    } else { printf("null"); }
    printf(",\"spo2_forecast_16s\":");
    if (ts.ready && ts.have_forecast && ts.spo2_valid) {
      printf("%d", (int)(ts.spo2_forecast_16s + 0.5f));
    } else { printf("null"); }
    printf("}\r\n");

    int16_t  hr_report   = hr_ok   ? (int16_t)hr    : ZCL_HR_INVALID;
    uint16_t spo2_report = spo2_ok ? (uint16_t)spo2  : ZCL_SPO2_INVALID;
    uint16_t weight_g_report = (uint16_t)(sh_flow_weight_g() < 0.0f ? 0 : sh_flow_weight_g() + 0.5f);
    uint16_t drops_per_min_report = (uint16_t)(sh_drops_per_min() + 0.5f);
    uint16_t target_flow_report = (uint16_t)(sh_target_flow_ml_h() + 0.5f);
    uint16_t target_drops_report = (uint16_t)(sh_target_drops_per_min() + 0.5f);
    uint16_t hr_baseline_bpm_report = (uint16_t)(ai_monitor_get_hr_baseline() + 0.5f);
    zb_report_ai_result(hr_report, spo2_report, (uint16_t)flow,
                        (int16_t)drop, alarm_bitmap,
                        weight_g_report, drops_per_min_report,
                        target_flow_report, target_drops_report,
                        app_hr_baseline_seconds_remaining(), hr_baseline_bpm_report,
                        sh_flow_tare_event_count(), hr_baseline_event_count);
    zb_report_ts_result(&ts);
  }
}
