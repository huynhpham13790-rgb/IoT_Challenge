/* ============================================================================
 *  ts_forecaster.h — C API for the time-series forecaster (see .cpp)
 * ========================================================================== */
#ifndef TS_FORECASTER_H
#define TS_FORECASTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These match model_data_ts.h (emitted by the training script). Repeated here
 * so C code does not have to include the whole 30 KB byte array just to learn
 * the tensor dimensions. */
#define TS_FC_WINDOW   64   /* past samples (seconds, @1Hz) */
#define TS_FC_HORIZON  16   /* seconds forecast ahead */
#define TS_FC_N_CH      4   /* 0=HR, 1=SpO2, 2=drops_ratio, 3=relative weight */
#define TS_FC_OUT_LEN  (TS_FC_HORIZON * TS_FC_N_CH)

/* Loads the model and allocates tensors. Returns false on failure - the system
 * then keeps running on the clinical rules and merely loses the forecast. */
bool     ts_forecaster_init(void);

/* Arena bytes actually used (0 before init) - for reporting / sanity checks. */
unsigned ts_forecaster_arena_used(void);

/* Runs the forecast. window_norm: TS_FC_WINDOW*TS_FC_N_CH normalised floats
 * ordered [t][channel]. out_norm: TS_FC_OUT_LEN floats, indexed
 * [h*TS_FC_N_CH + c]. */
bool     ts_forecaster_run(const float* window_norm, float* out_norm);

#ifdef __cplusplus
}
#endif

#endif /* TS_FORECASTER_H */
