#include "develop/tensor_box.h"

float IoU(TensorBoxes a, TensorBoxes b)
{
    float x1 = MAX(a.x1, b.x1);
    float y1 = MAX(a.y1, b.y1);
    float x2 = MIN(a.x2, b.x2);
    float y2 = MIN(a.y2, b.y2);
    float intersection = MAX(0, x2 - x1 + 1) * MAX(0, y2 - y1 + 1);
    float areaA = (a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1);
    float areaB = (b.x2 - b.x1 + 1) * (b.y2 - b.y1 + 1);
    return intersection / (areaA + areaB - intersection);
}

int compare_scores(const void* a, const void* b)
{
    TensorBoxes* boxA = (TensorBoxes*)a;
    TensorBoxes* boxB = (TensorBoxes*)b;
    if (boxA->score == boxB->score){
      // Sort in descending order of area
      float A_area = (boxA->x2 - boxA->x1) * (boxA->y2 - boxA->y1);
      float B_area = (boxB->x2 - boxB->x1) * (boxB->y2 - boxB->y1);
      if (A_area < B_area) return 1;
      if (A_area > B_area) return -1;
      return 0;
    }
    // Sort in descending order of score
    if (boxA->score < boxB->score) return 1;
    if (boxA->score > boxB->score) return -1;
    return 0;
}

void sort_tensor_boxes_by_score(TensorBoxes* boxes, size_t count)
{
    qsort(boxes, count, sizeof(TensorBoxes), compare_scores);
}

size_t NMS(TensorBoxes* boxes, size_t count, TensorBoxes* output)
{
    sort_tensor_boxes_by_score(boxes, count);
    char* suppressed = (char*)calloc(count, sizeof(char)); // 0 = not suppressed, 1 = suppressed
    size_t output_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (suppressed[i]) continue; // Skip if the box is suppressed
        output[output_count++] = boxes[i]; // Add the current box to output
        for (size_t j = i + 1; j < count; j++) {
            if (suppressed[j]) continue; // Skip if already suppressed
            float iou = IoU(boxes[i], boxes[j]);
            if (iou > IOU_THRESHOLD) {
                suppressed[j] = 1; // Suppress the box
            }
        }
    }
    free(suppressed);
    return output_count; // Return the number of boxes kept
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
