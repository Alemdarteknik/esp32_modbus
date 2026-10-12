#pragma once
#include "../../energy_transport_stubs/freertos/stream_buffer.h"
inline void vStreamBufferDelete(StreamBufferHandle_t stream) { stream->bytes.clear(); }
