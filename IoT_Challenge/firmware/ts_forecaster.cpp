/* ============================================================================
 *  ts_forecaster.cpp — TFLM runner for the Smart IV time-series FORECASTER
 *
 *  Fundamentally different from the older model_runner.cpp: that one takes 6
 *  numbers from a SINGLE instant and returns a reconstruction error. This one
 *  takes a 64-SECOND WINDOW x 4 channels and returns a FORECAST of the next 16
 *  seconds, which yields three things an instantaneous threshold cannot give:
 *  trend (rising/falling), early warning (the forecast crosses a limit before
 *  reality does), and anomaly detection (forecast error).
 *
 *  Why this architecture is fast on the xG26: the model is only 6 operators
 *  (CONV_2D x3, RESHAPE, FULLY_CONNECTED x2), every one of which is in the set
 *  the MVP accelerator can run. Deliberately avoided:
 *    - Keras Conv1D  -> TFLite expands each layer into
 *                       EXPAND_DIMS/CONV_2D/RESHAPE, inflating the model to 15
 *                       operators, most of them not accelerated.
 *    - LSTM/GRU      -> present in TFLM but NOT MVP-accelerated.
 *    - dilated conv  -> the MVP does not support dilation at all.
 *
 *  Exposed to C through extern "C" so app.c / ai_monitor.c can call it.
 * ========================================================================== */

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <cmath>
#include "model_data_ts.h"

/* Dedicated arena for the forecaster. Measured on the chip, this model uses
 * only 2964 bytes; 8 KB is allocated (nearly 3x headroom) rather than the 24 KB
 * originally estimated on paper, returning 16 KB of RAM to the rest of the
 * system. Raise it if AllocateTensors ever fails. */
constexpr int kTsArenaSize = 8 * 1024;
alignas(16) static uint8_t s_ts_arena[kTsArenaSize];

static const tflite::Model*      s_ts_model  = nullptr;
static tflite::MicroInterpreter* s_ts_interp = nullptr;
static TfLiteTensor*             s_ts_in     = nullptr;
static TfLiteTensor*             s_ts_out    = nullptr;

/* Three operator types: CONV_2D, RESHAPE, FULLY_CONNECTED */
using TsOpResolver = tflite::MicroMutableOpResolver<3>;
static TsOpResolver s_ts_resolver;

extern "C" bool ts_forecaster_init(void)
{
  s_ts_model = tflite::GetModel(g_ts_model_data);
  if (s_ts_model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("[TS] Model schema version mismatch!");
    return false;
  }

  s_ts_resolver.AddConv2D();
  s_ts_resolver.AddReshape();
  s_ts_resolver.AddFullyConnected();

  static tflite::MicroInterpreter static_interp(
      s_ts_model, s_ts_resolver, s_ts_arena, kTsArenaSize);
  s_ts_interp = &static_interp;

  if (s_ts_interp->AllocateTensors() != kTfLiteOk) {
    MicroPrintf("[TS] AllocateTensors failed - increase kTsArenaSize!");
    s_ts_interp = nullptr;
    return false;
  }

  s_ts_in  = s_ts_interp->input(0);
  s_ts_out = s_ts_interp->output(0);

  MicroPrintf("[TS] Forecaster ready: %u byte model, arena using %u/%u byte",
              (unsigned)g_ts_model_data_len,
              (unsigned)s_ts_interp->arena_used_bytes(),
              (unsigned)kTsArenaSize);
  return true;
}

/* Arena bytes actually used - for reporting; 0 if init has not run. */
extern "C" unsigned ts_forecaster_arena_used(void)
{
  return (s_ts_interp == nullptr) ? 0u
         : (unsigned)s_ts_interp->arena_used_bytes();
}

/*
 * Runs the forecast.
 *   window_norm : TS_WINDOW * TS_N_CH NORMALISED floats, ordered [t][channel]
 *                 (channels: 0=HR, 1=SpO2, 2=drops_ratio, 3=relative weight)
 *   out_norm    : TS_OUT_LEN NORMALISED floats, indexed [h*TS_N_CH + c]
 * Returns false if the model is not initialised or Invoke fails.
 */
extern "C" bool ts_forecaster_run(const float* window_norm, float* out_norm)
{
  if (s_ts_interp == nullptr || s_ts_in == nullptr) return false;

  const int n_in = TS_WINDOW * TS_N_CH;
  for (int i = 0; i < n_in; i++) {
    int q = (int)lroundf(window_norm[i] / TS_IN_SCALE) + TS_IN_ZP;
    if (q < -128) q = -128;
    if (q >  127) q =  127;
    s_ts_in->data.int8[i] = (int8_t)q;
  }

  if (s_ts_interp->Invoke() != kTfLiteOk) {
    MicroPrintf("[TS] Invoke failed!");
    return false;
  }

  for (int i = 0; i < TS_OUT_LEN; i++) {
    out_norm[i] = ((int)s_ts_out->data.int8[i] - TS_OUT_ZP) * TS_OUT_SCALE;
  }
  return true;
}
