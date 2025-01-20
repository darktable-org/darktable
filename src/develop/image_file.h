#pragma once
#include "onnxruntime_c_api.h"
#ifdef __cplusplus
extern "C" {
#endif

int write_image_file(_In_ uint8_t* model_output_bytes, unsigned int height,
                     unsigned int width, _In_z_ const ORTCHAR_T* output_file);

#ifdef __cplusplus
}
#endif