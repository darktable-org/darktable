#ifndef OBJECT_DETECTION_H
#define OBJECT_DETECTION_H

#include "develop/tensor_box.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Run object detection inference on a region of interest (ROI).
 *
 * @param input_image const uint_8t* input image data
 * @param h const int height of the ROI
 * @param w const int width of the ROI
 * @param out float** output data
 * @param n_masks size_t* number of masks
 *
 * @return int 0 if successful, -1 otherwise
 *
 * The model input is resized to 1024x1024 and then run through the object detection model.
 * The output is a set of masks, where each mask is a 2D array of size h x w.
 * The number of masks is stored in the n_masks parameter.
 */
int run_inference(uint8_t* input_image, const int h, const int w,
                  float** out, size_t * n_masks);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#endif // OBJECT_DETECTION_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
