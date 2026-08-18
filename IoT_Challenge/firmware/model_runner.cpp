/* ============================================================================
 *  model_runner.cpp — TensorFlow Lite Micro glue for the int8 autoencoder
 *  Written in C++ (TFLM is a C++ API) and exposed to C through extern "C":
 *      void  ai_model_init(void);
 *      float ai_recon_error(const float feat6[6]);  // reconstruction MSE
 *
 *  What ai_recon_error() does:
 *    1) Take the already-NORMALISED input (StandardScaler) from ai_monitor.c
 *    2) Quantise -> int8 using (IN_SCALE, IN_ZP)
 *    3) Invoke() the model
 *    4) Dequantise the int8 output -> float using (OUT_SCALE, OUT_ZP)
 *    5) Return the MSE between the normalised input and the reconstruction
 *
 *  Quantisation constants come from autoencoder_int8_pct.tflite.
 * ========================================================================== */

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <cmath>          // lroundf
#include "model_data.h"   // g_model_data[], g_model_data_len

/* ===== Quantisation constants (from the int8 model) ===== */
static const float IN_SCALE  = 0.02220776304602623f;
static const int   IN_ZP     = -66;
static const float OUT_SCALE = 0.021855410188436508f;
static const int   OUT_ZP    = -72;

#define NUM_FEAT 6

/* TFLM arena. The model is tiny (~3 KB) so 8 KB is plenty; raise it on error. */
constexpr int kArenaSize = 8 * 1024;
alignas(16) static uint8_t s_arena[kArenaSize];

static const tflite::Model*       s_model       = nullptr;
static tflite::MicroInterpreter*  s_interpreter = nullptr;
static TfLiteTensor*              s_input       = nullptr;
static TfLiteTensor*              s_output      = nullptr;

/* Verified with the tflite tooling: this model uses exactly ONE operator type,
 * FULLY_CONNECTED. ReLU is fused into it, and input/output are int8 directly
 * (we quantise by hand below), so no separate Quantize/Dequantize op is
 * needed. */
using OpResolver = tflite::MicroMutableOpResolver<1>;
static OpResolver s_resolver;

extern "C" void ai_model_init(void)
{
  s_model = tflite::GetModel(g_model_data);
  if (s_model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model schema version mismatch!");
    return;
  }

  s_resolver.AddFullyConnected();

  static tflite::MicroInterpreter static_interpreter(
      s_model, s_resolver, s_arena, kArenaSize);
  s_interpreter = &static_interpreter;

  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed (increase kArenaSize)!");
    return;
  }
  s_input  = s_interpreter->input(0);
  s_output = s_interpreter->output(0);
}

extern "C" float ai_recon_error(const float feat6[NUM_FEAT])
{
  if (s_interpreter == nullptr || s_input == nullptr) return 0.0f;

  /* 1) Quantise the (already normalised) float input -> int8 */
  for (int i = 0; i < NUM_FEAT; i++) {
    int q = (int)lroundf(feat6[i] / IN_SCALE) + IN_ZP;
    if (q < -128) q = -128;
    if (q >  127) q =  127;
    s_input->data.int8[i] = (int8_t)q;
  }

  /* 2) Run the model */
  if (s_interpreter->Invoke() != kTfLiteOk) {
    MicroPrintf("Invoke failed!");
    return 0.0f;
  }

  /* 3) Dequantise the output and compute the MSE against the normalised input */
  float mse = 0.0f;
  for (int i = 0; i < NUM_FEAT; i++) {
    float recon = ((int)s_output->data.int8[i] - OUT_ZP) * OUT_SCALE;
    float diff  = recon - feat6[i];
    mse += diff * diff;
  }
  return mse / (float)NUM_FEAT;
}
