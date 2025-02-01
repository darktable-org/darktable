#include "develop/object_detection.h"

const OrtApi* g_ort = NULL;

void process_mask_native( float *protos, float *masks_in, TensorBoxes* boxes,
                          int n, int mask_dim, int mask_h, int mask_w, int output_h,
                          int output_w, float *output_masks)
{
  // Allocate intermediate storage for masks [n, mask_h, mask_w]
  float *masks = (float *)malloc(n * mask_h * mask_w * sizeof(float));
  printf("Allocated masks");
  if (!masks) {
    fprintf(stderr, "Memory allocation failed\n");
    exit(EXIT_FAILURE);
  }

  // Flattened version of `protos` reshaped to [mask_dim, mask_h * mask_w]
  float *protos_flat = (float *)malloc(mask_dim * mask_h * mask_w * sizeof(float));
  printf("Allocated protos");
  if (!protos_flat) {
    fprintf(stderr, "Memory allocation failed\n");
    free(masks);
    exit(EXIT_FAILURE);
  }

  printf("Allocated everything");

  // Flatten protos
  for (int c = 0; c < mask_dim; ++c) {
    for (int i = 0; i < mask_h * mask_w; ++i) {
      protos_flat[c * (mask_h * mask_w) + i] = protos[c * mask_h * mask_w + i];
    }
  }

  printf("Flattened protos");

  // Perform masks_in @ protos
  for (int i = 0; i < n; ++i) {
      for (int j = 0; j < mask_h * mask_w; ++j) {
          masks[i * mask_h * mask_w + j] = 0.0f;
          for (int k = 0; k < mask_dim; ++k) {
              masks[i * mask_h * mask_w + j] += masks_in[i * mask_dim + k] * protos_flat[k * (mask_h * mask_w) + j];
          }
      }
  }

  printf("Created masks");

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

void prep_out_data( float* input_data[6], int64_t definition_size, int64_t numb_boxes,
                    float** output, size_t output_height, size_t output_width, size_t* n_masks)
{
  float* mask = input_data[0];
  
  TensorBoxes* boxes = malloc(numb_boxes * sizeof(TensorBoxes));

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
  boxes = realloc(boxes, counter * sizeof(TensorBoxes));

  sort_tensor_boxes_by_score(boxes, counter);
  
  TensorBoxes* output_boxes = (TensorBoxes*)malloc(counter * sizeof(TensorBoxes));
  size_t num_boxes = NMS(boxes, counter, output_boxes);

  printf("num_boxes: %ld\n", num_boxes);

  output_boxes = realloc(output_boxes, num_boxes * sizeof(TensorBoxes));

  int mask_h = 256, mask_w = 256;

  // Allocate and initialize inputs
  float *protos = input_data[5];
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

void resize_image(const float** input, const int input_height, const int input_width,
                  float** out, size_t output_height, size_t output_width,
                  size_t* output_count)
{
  float* output_data = (float*)malloc(3 * output_width * output_width * sizeof(float));
  size_t out_stride = output_height * output_width;
  size_t in_stride = input_height * input_width;
  float height_ratio = (float)input_height / (float)output_height;
  float width_ratio = (float)input_width / (float)output_width;

  for (size_t c = 0; c < 3; c++){
    for (size_t i = 0; i < output_height; i++){
      for (size_t j = 0; j < output_width; j++){
        size_t input_j = (size_t)((float)j * width_ratio);
        size_t input_i = (size_t)((float)i * height_ratio);
        float input_d = (*input)[c * in_stride + input_i * input_width + input_j];
        output_data[c * out_stride + i * output_width + j] = input_d;
      }
    }
  }
  *out = output_data;
  *output_count = out_stride * 3;
};

void hwc_to_chw(const uint8_t* input, const int h, const int w,
                float** output, size_t* output_count) {
  size_t stride = h * w;
  *output_count = stride * 3;
  float* output_data = (float*)malloc(3* stride * sizeof(float));
  if (!output_data) return;

  for (size_t i = 0; i != stride; ++i) {
    for (size_t c = 0; c != 3; ++c) {
      
      output_data[c * stride + i] = ((float)input[i * 3 + c])/255.0; // I'm also converting from 0-255 to 0-1 and RGBA to RGB
    }
  }
  *output = output_data;
}


int run_inference(OrtSession* session, const float* input_image, const int h, const int w,
                  float** out, size_t * n_masks) {
  const int input_height = h;
  const int input_width = w;
  printf("Roi h:%d, w:%d\n", h, w);
  float* model_input;
  size_t model_input_ele_count = 1024 * 1024;

  resize_image(&input_image, input_height, input_width, &model_input, 1024, 1024, &model_input_ele_count);

  OrtMemoryInfo* memory_info;
  ORT_ABORT_ON_ERROR(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info));
  const int64_t input_shape[] = {1, 3, 1024, 1024};
  const size_t input_shape_len = sizeof(input_shape) / sizeof(input_shape[0]);
  const size_t model_input_len = model_input_ele_count * sizeof(float);

  OrtValue* input_tensor = NULL;
  ORT_ABORT_ON_ERROR(g_ort->CreateTensorWithDataAsOrtValue(memory_info, model_input, model_input_len, input_shape,
                                                           input_shape_len, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                           &input_tensor));
  
  int is_tensor;
  ORT_ABORT_ON_ERROR(g_ort->IsTensor(input_tensor, &is_tensor));
  
 
  OrtAllocator* allocator;
  ORT_ABORT_ON_ERROR(g_ort->GetAllocatorWithDefaultOptions(&allocator))

  const char* input_names[] = {"images"};
  const char* output_names[] = {"output0", "output1", "onnx::Shape_1304", "onnx::Shape_1323",
                                "onnx::Concat_1263", "onnx::Shape_1215"};
  
  OrtValue* output_tensor[6];

  for (int i = 0; i < 6; i++) {
    output_tensor[i] = NULL;
  }

  
  printf("Running inference\n");
  ORT_ABORT_ON_ERROR(g_ort->Run(session, NULL, input_names, (const OrtValue* const*)&input_tensor, 1, output_names, 6,
                                output_tensor));
  printf("Inference done\n");

  for (int i = 0; i < 6; i++) {
    ORT_ABORT_ON_ERROR(g_ort->IsTensor(output_tensor[i], &is_tensor));
  }

  OrtTensorTypeAndShapeInfo* tensor_info;
  ORT_ABORT_ON_ERROR(g_ort->GetTensorTypeAndShape(output_tensor[0], &tensor_info));

  // Get the shape dimensions
  size_t num_dims;
  ORT_ABORT_ON_ERROR(g_ort->GetDimensionsCount(tensor_info, &num_dims));

  int64_t* shape = (int64_t*)malloc(num_dims * sizeof(int64_t));
  ORT_ABORT_ON_ERROR(g_ort->GetDimensions(tensor_info, shape, num_dims));

  // Get tensor element type
  ONNXTensorElementDataType data_type;
  ORT_ABORT_ON_ERROR(g_ort->GetTensorElementType(tensor_info, &data_type));

  // Get the output tensor information
  int ret = 0;
  float* output_tensor_data = NULL;
  ORT_ABORT_ON_ERROR(g_ort->GetTensorMutableData(output_tensor[0], (void**)&output_tensor_data));

  float* output_tensor_data_t[6];
  for (int i = 0; i < 6; i++) {
    output_tensor_data_t[i] = NULL;
    ORT_ABORT_ON_ERROR(g_ort->GetTensorMutableData(output_tensor[i], (void**)&output_tensor_data_t[i]));
  }

  prep_out_data(output_tensor_data_t, shape[1], shape[2], out, input_height, input_width, n_masks);

  for (int i = 0; i < 6; i++) {
    g_ort->ReleaseValue(output_tensor[i]);
  }

  g_ort->ReleaseMemoryInfo(memory_info);
  free(shape);
  g_ort->ReleaseTensorTypeAndShapeInfo(tensor_info);
  g_ort->ReleaseValue(input_tensor);
  free(model_input);
  
  return ret;
}