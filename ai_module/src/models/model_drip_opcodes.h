/* GENERATED FILE - DO NOT EDIT.
 *
 * Produced by ml/export_c_headers.py from ml/models/drip_forecaster_int8.tflite, using Silicon Labs' own MLTK
 * flatbuffer converter (the same tool Simplicity Studio runs on a .tflite placed
 * in config/tflite/). Regenerate with:
 *
 *     .venv-ai/bin/python ml/export_c_headers.py
 */
// Auto-generated macro to instanciate and initialize opcode resolver based on TFLite flatbuffers in config directory
#ifndef DRIP_OPCODE_RESOLVER_H
#define DRIP_OPCODE_RESOLVER_H

#define DRIP_OPCODE_RESOLVER(drip_opcode_resolver) \
static tflite::MicroMutableOpResolver<3> drip_opcode_resolver; \
drip_opcode_resolver.AddConv2D(); \
drip_opcode_resolver.AddReshape(); \
drip_opcode_resolver.AddFullyConnected(); \


#endif // DRIP_OPCODE_RESOLVER_H
