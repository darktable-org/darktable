#ifndef OBJECT_DETECTION_H
#define OBJECT_DETECTION_H

#include "onnxruntime_c_api.h"
#include "develop/tensor_boxes.h"

#include <stdio.h>

#define tcscmp strcmp

#define ORT_ABORT_ON_ERROR(expr)                             \
  do {                                                       \
    OrtStatus* onnx_status = (expr);                         \
    if (onnx_status != NULL) {                               \
      const char* msg = g_ort->GetErrorMessage(onnx_status); \
      fprintf(stderr, "%s\n", msg);                          \
      g_ort->ReleaseStatus(onnx_status);                     \
      abort();                                               \
    }                                                        \
  } while (0);


extern const OrtApi* g_ort;

/**
 * Processes input masks by applying mask prototypes and resizing them to the desired output dimensions.
 * Allocates memory for intermediate and output masks and performs matrix multiplication followed by bilinear interpolation.
 *
 * @param protos       Pointer to the mask prototypes with dimensions [mask_dim, mask_h, mask_w].
 * @param masks_in     Pointer to input masks with dimensions [n, mask_dim].
 * @param boxes        Pointer to an array of TensorBoxes, each associated with a mask.
 * @param n            Number of input masks.
 * @param mask_dim     Number of channels in the masks (dimension of protos).
 * @param mask_h       Height of the prototype masks.
 * @param mask_w       Width of the prototype masks.
 * @param output_h     Height of the output masks.
 * @param output_w     Width of the output masks.
 * @param output_masks Pointer to the output masks with dimensions [output_h, output_w, n], where each mask is boolean.
 *
 * The function performs matrix multiplication of `masks_in` with flattened `protos` to generate intermediate masks,
 * followed by bilinear interpolation to resize the masks to the specified output dimensions. The output masks are
 * thresholded to boolean values based on the interpolation results.
 */
void process_mask_native( float *protos, float *masks_in, TensorBoxes* boxes,
                          int n, int mask_dim, int mask_h, int mask_w, int output_h,
                          int output_w, float *output_masks); 


/**
 * @brief Prepares the output data for the object detection model
 * 
 * This function takes the output of the object detection model and prepares 
 * it for the rest of the program. It first selects the bounding boxes with 
 * a score higher than a certain threshold, then applies non-maximum suppression
 * and finally generates the corresponding masks.
 * 
 * @param input_data The output of the object detection model
 * @param definition_size The size of the definition of the model
 * @param numb_boxes The number of bounding boxes in the output
 * @param output A pointer to a float array that will contain the masks
 * @param output_height The height of the masks
 * @param output_width The width of the masks
 * @param n_masks A pointer to a size_t that will contain the number of masks
 */
void prep_out_data( float* input_data[6], int64_t definition_size, int64_t numb_boxes,
                    float** output, size_t output_height, size_t output_width, size_t* n_masks);
 
/**
 * @brief Resize an image
 *
 * Resizes an image from (input_height, input_width) to (output_height, output_width).
 * The resized image is stored in a float array of size (output_height, output_width, 3).
 *
 * @param[in] input The input image as a float array of size (input_height, input_width, 3).
 * @param[in] input_height The height of the input image.
 * @param[in] input_width The width of the input image.
 * @param[out] out The resized image as a float array of size (output_height, output_width, 3).
 * @param[in] output_height The height of the output image.
 * @param[in] output_width The width of the output image.
 * @param[out] output_count The size of the output array.
 */
void resize_image(const float** input, const int input_height, const int input_width,
                  float** out, size_t output_height, size_t output_width,
                  size_t* output_count);

/**
 * @brief Convert an image in HWC format to CHW format.
 *
 * This function takes an image in HWC (Height, Width, Channels) format and
 * converts it to CHW (Channels, Height, Width) format. The input image is
 * expected to be in RGBA format, and the output image is in RGB format.
 *
 * @param input The input image in HWC format.
 * @param h The height of the input image.
 * @param w The width of the input image.
 * @param output The output image in CHW format.
 * @param output_count The number of elements in the output image.
 */
void hwc_to_chw(const uint8_t* input, const int h, const int w,
                float** output, size_t* output_count);

/**
 * @brief Run object detection inference on a region of interest (ROI).
 *
 * @param session OrtSession* to run the inference on
 * @param input_image const float* input image data
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
 int run_inference(OrtSession* session, const float* input_image, const int h, const int w,
                   float** out, size_t * n_masks);

#endif