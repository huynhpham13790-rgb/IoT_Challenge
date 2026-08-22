/* GENERATED FILE - DO NOT EDIT.
 *
 * Produced by ml/export_c_headers.py from ml/models/vitals_forecaster_int8.tflite, using Silicon Labs' own MLTK
 * flatbuffer converter (the same tool Simplicity Studio runs on a .tflite placed
 * in config/tflite/). Regenerate with:
 *
 *     .venv-ai/bin/python ml/export_c_headers.py
 */
// Auto-generated macro to instanciate and initialize opcode resolver based on TFLite flatbuffers in config directory
#ifndef VITALS_OPCODE_RESOLVER_H
#define VITALS_OPCODE_RESOLVER_H

#define VITALS_OPCODE_RESOLVER(vitals_opcode_resolver) \
static tflite::MicroMutableOpResolver<3> vitals_opcode_resolver; \
vitals_opcode_resolver.AddConv2D(); \
vitals_opcode_resolver.AddReshape(); \
vitals_opcode_resolver.AddFullyConnected(); \


#endif // VITALS_OPCODE_RESOLVER_H
