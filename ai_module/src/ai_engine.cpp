/* ============================================================================
 *  ai_engine.cpp — three TFLM interpreters, three arenas, no shared state
 *
 *  The model byte arrays and the opcode resolvers in firmware/models/ are
 *  GENERATED, by ml/export_c_headers.py driving Silicon Labs' own MLTK
 *  flatbuffer converter. The resolvers matter more than the arrays: their
 *  contents are derived by parsing each flatbuffer, so they list exactly the
 *  operators that model uses and are sized exactly right. Changing a model's
 *  architecture and forgetting to add an operator - the classic way to make a
 *  TFLM project fail at AllocateTensors, at the bedside rather than at build
 *  time - is not possible here.
 *
 *  Quantisation is done by hand rather than through a float input tensor: the
 *  models are fully int8, so the interpreter's input tensor IS int8 and the
 *  scale/zero-point come from the model itself. Reading them from the tensor
 *  rather than hard-coding them means re-exporting a model cannot silently
 *  desynchronise the firmware from it.
 * ========================================================================== */

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <cmath>

#include "ai_engine.h"

#include "models/model_drip.h"
#include "models/model_drip_opcodes.h"
#include "models/model_vitals.h"
#include "models/model_vitals_opcodes.h"
#include "models/model_ae.h"
#include "models/model_ae_opcodes.h"

/* Arena sizes. Measured on-chip figures are printed at boot by
 * ai_engine_init(); these are set with headroom above them. Raise one if
 * AllocateTensors starts failing - it will say so on the console rather than
 * hang the device. */
constexpr int kDripArena   = 4 * 1024;
constexpr int kVitalsArena = 4 * 1024;
constexpr int kAeArena     = 2 * 1024;

alignas(16) static uint8_t s_drip_arena[kDripArena];
alignas(16) static uint8_t s_vitals_arena[kVitalsArena];
alignas(16) static uint8_t s_ae_arena[kAeArena];

namespace {

/* One model's runtime state. Bundling it keeps the three instances from
 * sharing anything by accident - the entire point of the split. */
struct Model {
  const char*              name;
  tflite::MicroInterpreter* interp = nullptr;
  TfLiteTensor*             in     = nullptr;
  TfLiteTensor*             out    = nullptr;
  bool                      ready  = false;

  bool quantise(const float* src, int count) const
  {
    if (!ready || in->bytes < (size_t)count) return false;
    const float scale = in->params.scale;
    const int   zp    = in->params.zero_point;
    for (int i = 0; i < count; i++) {
      int q = (int)lroundf(src[i] / scale) + zp;
      if (q < -128) q = -128;
      if (q > 127)  q = 127;
      in->data.int8[i] = (int8_t)q;
    }
    return true;
  }

  bool invoke() const
  {
    if (!ready) return false;
    if (interp->Invoke() != kTfLiteOk) {
      MicroPrintf("[AI] %s: Invoke failed", name);
      return false;
    }
    return true;
  }

  void dequantise(float* dst, int count) const
  {
    const float scale = out->params.scale;
    const int   zp    = out->params.zero_point;
    for (int i = 0; i < count; i++) {
      dst[i] = ((int)out->data.int8[i] - zp) * scale;
    }
  }
};

Model s_drip{"drip"};
Model s_vitals{"vitals"};
Model s_ae{"ae"};

/* Shared bring-up. Returns false instead of hanging, on every failure path. */
bool load(Model& m, const uint8_t* data, tflite::MicroOpResolver& resolver,
          uint8_t* arena, int arena_size, tflite::MicroInterpreter* storage)
{
  const tflite::Model* model = tflite::GetModel(data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("[AI] %s: schema version %d, expected %d - model NOT loaded",
                m.name, (int)model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  m.interp = new (storage) tflite::MicroInterpreter(model, resolver,
                                                    arena, arena_size);
  if (m.interp->AllocateTensors() != kTfLiteOk) {
    MicroPrintf("[AI] %s: AllocateTensors failed - raise its arena (now %d B)",
                m.name, arena_size);
    m.interp = nullptr;
    return false;
  }

  m.in    = m.interp->input(0);
  m.out   = m.interp->output(0);
  m.ready = true;

  /* No %f here. TFLM's MicroPrintf implements its own tiny formatter and does
   * NOT support floating point: the literal ".6f" is emitted and the argument
   * is left unconsumed, so every later specifier reads the wrong slot. Observed
   * on the board as "in scale .6f zp 1610612736" - a garbage zero-point that
   * looks like a real diagnostic. The scale is printed scaled to integers
   * instead, in the same units the training pipeline reports. */
  MicroPrintf("[AI] %s ready: arena %u/%u B, in scale %u/1e6, zp %d",
              m.name, (unsigned)m.interp->arena_used_bytes(),
              (unsigned)arena_size,
              (unsigned)(m.in->params.scale * 1000000.0f + 0.5f),
              m.in->params.zero_point);
  return true;
}

}  // namespace

extern "C" bool ai_engine_init(void)
{
  if (s_drip.ready || s_vitals.ready || s_ae.ready) {
    return s_drip.ready && s_vitals.ready && s_ae.ready;   /* already done */
  }

  /* The resolver macros are generated per model and declare a static local, so
   * each has to be expanded in its own scope. Storage for the interpreters is
   * static too: TFLM must not allocate, and these objects outlive this call. */
  static uint8_t drip_storage[sizeof(tflite::MicroInterpreter)]
      alignas(alignof(tflite::MicroInterpreter));
  static uint8_t vitals_storage[sizeof(tflite::MicroInterpreter)]
      alignas(alignof(tflite::MicroInterpreter));
  static uint8_t ae_storage[sizeof(tflite::MicroInterpreter)]
      alignas(alignof(tflite::MicroInterpreter));

  {
    DRIP_OPCODE_RESOLVER(r);
    load(s_drip, g_drip_model_array, r, s_drip_arena, kDripArena,
         reinterpret_cast<tflite::MicroInterpreter*>(drip_storage));
  }
  {
    VITALS_OPCODE_RESOLVER(r);
    load(s_vitals, g_vitals_model_array, r, s_vitals_arena, kVitalsArena,
         reinterpret_cast<tflite::MicroInterpreter*>(vitals_storage));
  }
  {
    AE_OPCODE_RESOLVER(r);
    load(s_ae, g_ae_model_array, r, s_ae_arena, kAeArena,
         reinterpret_cast<tflite::MicroInterpreter*>(ae_storage));
  }

  const bool all = s_drip.ready && s_vitals.ready && s_ae.ready;
  if (!all) {
    /* Not fatal, and deliberately so. Whatever loaded still runs, and the
     * clinical rules in ai_fusion.c do not depend on any of it. */
    MicroPrintf("[AI] WARNING: running with drip=%d vitals=%d ae=%d - "
                "clinical rules remain active",
                (int)s_drip.ready, (int)s_vitals.ready, (int)s_ae.ready);
  }
  return all;
}

extern "C" bool ai_engine_drip_ready(void)   { return s_drip.ready; }
extern "C" bool ai_engine_vitals_ready(void) { return s_vitals.ready; }
extern "C" bool ai_engine_ae_ready(void)     { return s_ae.ready; }

extern "C" unsigned ai_engine_drip_arena_used(void)
{
  return s_drip.ready ? (unsigned)s_drip.interp->arena_used_bytes() : 0u;
}
extern "C" unsigned ai_engine_vitals_arena_used(void)
{
  return s_vitals.ready ? (unsigned)s_vitals.interp->arena_used_bytes() : 0u;
}
extern "C" unsigned ai_engine_ae_arena_used(void)
{
  return s_ae.ready ? (unsigned)s_ae.interp->arena_used_bytes() : 0u;
}

extern "C" bool ai_engine_run_drip(const float* window_norm, float* out_norm)
{
  if (!s_drip.quantise(window_norm, AI_WINDOW)) return false;
  if (!s_drip.invoke()) return false;
  s_drip.dequantise(out_norm, AI_HORIZON);
  return true;
}

extern "C" bool ai_engine_run_vitals(const float* window_norm, float* out_norm)
{
  if (!s_vitals.quantise(window_norm, AI_WINDOW * 2)) return false;
  if (!s_vitals.invoke()) return false;
  s_vitals.dequantise(out_norm, AI_HORIZON * 2);
  return true;
}

extern "C" bool ai_engine_run_ae(const float* in_norm, float* recon_error)
{
  if (!s_ae.quantise(in_norm, 2)) return false;
  if (!s_ae.invoke()) return false;

  float recon[2];
  s_ae.dequantise(recon, 2);

  /* Mean squared error over the two channels - the same formula
   * ml/train_vitals_ae.py used to set AI_AE_THRESHOLD. */
  const float e0 = recon[0] - in_norm[0];
  const float e1 = recon[1] - in_norm[1];
  *recon_error = 0.5f * (e0 * e0 + e1 * e1);
  return true;
}