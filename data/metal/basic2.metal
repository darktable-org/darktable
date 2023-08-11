kernel void
add_arrays2(device const float* inA,
            device const float* inB,
            device float* result,
            uint index [[thread_position_in_grid]])
{
  // the for-loop is replaced with a collection of threads, each of which
  // calls this function.
  result[index] = inA[index] + inB[index];
}
