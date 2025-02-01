#ifndef TENSOR_BOXES_H
#define TENSOR_BOXES_H

#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif
#ifndef MIN
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#endif

#define IOU_THRESHOLD 0.7
#define CONF 0.3

#include <stdlib.h>
#include <stddef.h>

typedef struct
{
  float x1;
  float y1;
  float x2;
  float y2;
  float score;
  float* mask;
} TensorBoxes;


/**
 * Calculate the intersection over union between two bounding boxes.
 *
 * @param a first bounding box
 * @param b second bounding box
 * @return IoU value between 0 and 1
 */
float IoU(TensorBoxes a, TensorBoxes b);

/**
 * Compares two TensorBoxes in terms of their scores and areas.
 * 
 * This function is used as a comparison function for sorting an array of TensorBoxes.
 * The boxes are sorted in descending order of their scores, and then in descending order of their areas.
 * 
 * @param a A pointer to the first TensorBoxes to compare.
 * @param b A pointer to the second TensorBoxes to compare.
 * @return An integer less than, equal to, or greater than zero if a is considered to be
 *  respectively less than, equal to, or greater than b.
 */
int compare_scores(const void* a, const void* b);

/**
 * Sort the given array of TensorBoxes in descending order of their score.
 *
 * This function does not modify the original array but instead sorts it in-place.
 * The sorting is done by the qsort function from the standard library
 *
 * This function uses the compare_scores function as the comparison function to
 * compare two TensorBoxes.
 *
 * @param boxes The array of TensorBoxes to be sorted.
 * @param count The number of TensorBoxes in the array.
 * @see compare_scores
 */
void sort_tensor_boxes_by_score(TensorBoxes* boxes, size_t count);

/**
 * Perform non-maximum suppression on the given boxes.
 *
 * @param boxes An array of boxes to perform NMS on.
 * @param count The number of boxes in the given array.
 * @param output An array to store the output boxes.
 *
 * @return The number of boxes kept after NMS.
 */
size_t NMS(TensorBoxes* boxes, size_t count, TensorBoxes* output);

#endif