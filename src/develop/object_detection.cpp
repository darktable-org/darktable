#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/dnn/all_layers.hpp>

#include "develop/object_detection.h"

using namespace std;
using namespace cv;
using namespace dnn;

void process_mask_native( float *protos, float *masks_in, TensorBoxes* boxes,
                          int n, int mask_dim, int mask_h, int mask_w, int output_h,
                          int output_w, float *output_masks)
{
  // Allocate intermediate storage for masks [n, mask_h, mask_w]
  float *masks = (float *)malloc(n * mask_h * mask_w * sizeof(float));
  if (!masks) {
    fprintf(stderr, "Memory allocation failed\n");
    exit(EXIT_FAILURE);
  }
  // Flattened version of `protos` reshaped to [mask_dim, mask_h * mask_w]
  float *protos_flat = (float *)malloc(mask_dim * mask_h * mask_w * sizeof(float));
  if (!protos_flat) {
    fprintf(stderr, "Memory allocation failed\n");
    free(masks);
    exit(EXIT_FAILURE);
  }
  // Flatten protos
  for (int c = 0; c < mask_dim; ++c) {
    for (int i = 0; i < mask_h * mask_w; ++i) {
      protos_flat[c * (mask_h * mask_w) + i] = protos[c * mask_h * mask_w + i];
    }
  }
  // Perform masks_in @ protos
  for (int i = 0; i < n; ++i) {
      for (int j = 0; j < mask_h * mask_w; ++j) {
          masks[i * mask_h * mask_w + j] = 0.0f;
          for (int k = 0; k < mask_dim; ++k) {
              masks[i * mask_h * mask_w + j] += masks_in[i * mask_dim + k] * protos_flat[k * (mask_h * mask_w) + j];
          }
      }
  }
  float *max_value = (float *)malloc(n  * sizeof(float));
  float *min_value = (float *)malloc(n  * sizeof(float));
  // Threshold and create masks
  for (int i = 0; i < n; ++i) {
    max_value[i] = 0;
    min_value[i] = 0;
    TensorBoxes current_box = boxes[i];
    if (i == 0){
      printf("x1 %f, x2 %f, y1 %f, y2 %f\n", current_box.x1, current_box.x2, current_box.y1, current_box.y2);
    }
    for (int y = 0; y < output_h; ++y) {
      for (int x = 0; x < output_w; ++x) {
        // Calculate the corresponding coordinates in the original mask
        float src_x = (float)x * mask_w / output_w;
        float src_y = (float)y * mask_h / output_h;
        // Find the four nearest neighbors
        int x1 = (int)src_x;
        int y1 = (int)src_y;
        int x2 = (x1 + 1 < mask_w) ? x1 + 1 : x1;
        int y2 = (y1 + 1 < mask_h) ? y1 + 1 : y1;
        // Calculate the distances (weights) for interpolation
        float dx = src_x - x1;
        float dy = src_y - y1;
        // Get the pixel values from the original mask
        float top_left = masks[i * mask_h * mask_w + y1 * mask_w + x1];
        float top_right = masks[i * mask_h * mask_w + y1 * mask_w + x2];
        float bottom_left = masks[i * mask_h * mask_w + y2 * mask_w + x1];
        float bottom_right = masks[i * mask_h * mask_w + y2 * mask_w + x2];
        // Perform bilinear interpolation
        float interpolated_value = (1 - dx) * (1 - dy) * top_left +
                                            dx * (1 - dy) * top_right +
                                            (1 - dx) * dy * bottom_left +
                                            dx * dy * bottom_right;
        // Set the value in the output mask
        int idx_out = i * output_h * output_w + y * output_w + x;
        // FIXME
        if (((src_x*4) < current_box.x1) || ((src_x*4) > current_box.x2) || ((src_y*4) < current_box.y1) || ((src_y*4) > current_box.y2))
          interpolated_value = 0.0f;
        output_masks[idx_out] = interpolated_value;
        if (interpolated_value > max_value[i]) max_value[i] = output_masks[idx_out];
        if (interpolated_value < min_value[i]) min_value[i] = output_masks[idx_out];
      }
    }
  }
  printf("Loaded masks");
  for (int i = 0; i < n; ++i) {
    for (int y = 0; y < output_h; ++y) {
      for (int x = 0; x < output_w; ++x) {
        int idx_out = i * output_h * output_w + y * output_w + x;
        if (output_masks[idx_out] > 0.0f)
          output_masks[idx_out] = 1.0; // output_masks[idx_out] / max_value[i];
        else
          output_masks[idx_out] = 0.0f;
      }
    }
  }
  printf("Refined masks");
  // Free allocated memory
  free(masks);
  free(protos_flat);
  free(min_value);
  free(max_value);
}
void prep_out_data( vector<Mat> input_data, int64_t definition_size, int64_t numb_boxes,
                    float** output, size_t output_height, size_t output_width, size_t* n_masks)
{
  float* mask = (float*)input_data[0].data;
  
  TensorBoxes* boxes = (TensorBoxes*)valloc(numb_boxes * sizeof(TensorBoxes));
  
  size_t coordinates_count = 4;
  size_t class_count = 1;
  size_t mask_dim = definition_size - coordinates_count - class_count;
  size_t b_stride = numb_boxes;
  size_t counter = 0;
  for (int64_t i = 0; i < numb_boxes; i++) {
    float score = mask[i + 4 * b_stride];
    if (score < CONF) {
      continue;
    }
    float w = mask[i + (2 * b_stride)];
    float h = mask[i + (3 * b_stride)];
    if (w < 0 || h < 0) {
      continue;
    }
    boxes[counter].x1 = mask[i + (0 * b_stride) ] - (w / 2);
    boxes[counter].y1 = mask[i + (1 * b_stride) ] - (h / 2);
    boxes[counter].x2 = mask[i + (0 * b_stride) ] + (w / 2);
    boxes[counter].y2 = mask[i + (1 * b_stride) ] + (h / 2);
    
    boxes[counter].score = score;
    boxes[counter].mask = (float*)malloc(mask_dim * sizeof(float));
    for (size_t j = 0; j < mask_dim; j++) {
      boxes[counter].mask[j] = mask[i  + (5 + j) * b_stride ];
    }
    counter++;
  }
  printf("counter: %ld\n", counter);
  if (counter == 0){
    return;
  }
  boxes = (TensorBoxes*)realloc(boxes, counter * sizeof(TensorBoxes));
  sort_tensor_boxes_by_score(boxes, counter);
  
  TensorBoxes* output_boxes = (TensorBoxes*)malloc(counter * sizeof(TensorBoxes));
  size_t num_boxes = NMS(boxes, counter, output_boxes);
  printf("num_boxes: %ld\n", num_boxes);
  output_boxes = (TensorBoxes*)realloc(output_boxes, num_boxes * sizeof(TensorBoxes));
  int mask_h = 256, mask_w = 256;
  // Allocate and initialize inputs
  float *protos = (float*)input_data[5].data;
  float *masks_in = (float *)malloc(num_boxes * mask_dim * sizeof(float));
  float *output_masks = (float *)malloc(output_height * output_width * num_boxes * sizeof(float));
  for (size_t i = 0; i < num_boxes; ++i){
    for (size_t j = 0; j < mask_dim; ++j){
      masks_in[i * mask_dim + j] = output_boxes[i].mask[j];
    }
  }
  
  process_mask_native(protos, masks_in, output_boxes, num_boxes, mask_dim, mask_h, mask_w, output_height, output_width, output_masks);
  *output = output_masks;
  *n_masks = num_boxes;
  for (size_t i = 0; i < counter; i++) {
    free(boxes[i].mask);
  }
  free(boxes);
}

int run_inference(uint8_t* input_image, const int h, const int w,
                  float** out, size_t * n_masks) {
  auto model = readNetFromONNX("/home/miko/Documents/OpenSourceProjects/opencv-dnn/fast_sam_1024_simp.onnx");

  Mat image = Mat(h, w, CV_8UC4, input_image);
  Mat imageRGB;

  cvtColor(image, imageRGB, COLOR_RGBA2RGB);

  Mat blob = blobFromImage(imageRGB, 1.0f/255.0f, Size(1024,1024), Scalar(), true, false);

  model.setInput(blob);

  vector<Mat> outputs;
  const vector<String> output_names = {"output0", "output1", "onnx::Reshape_1276", "onnx::Reshape_1295", "onnx::Concat_1237", "1191"};
  model.forward(outputs, output_names);

  prep_out_data(outputs, outputs[0].size[1], outputs[0].size[2], out, image.rows, image.cols, n_masks);
  
  return 0;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
