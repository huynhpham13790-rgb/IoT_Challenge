/* GENERATED FILE - DO NOT EDIT.
 *
 * Produced by ml/export_c_headers.py from ml/models/vitals_ae_int8.tflite, using Silicon Labs' own MLTK
 * flatbuffer converter (the same tool Simplicity Studio runs on a .tflite placed
 * in config/tflite/). Regenerate with:
 *
 *     .venv-ai/bin/python ml/export_c_headers.py
 */
// Auto-generated macro to instanciate and initialize opcode resolver based on TFLite flatbuffers in config directory
#ifndef AE_OPCODE_RESOLVER_H
#define AE_OPCODE_RESOLVER_H

#define AE_OPCODE_RESOLVER(ae_opcode_resolver) \
static tflite::MicroMutableOpResolver<1> ae_opcode_resolver; \
ae_opcode_resolver.AddFullyConnected(); \


#endif // AE_OPCODE_RESOLVER_H
