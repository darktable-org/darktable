#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif
typedef enum squish_flags_t
{
	//! Use DXT1 compression.
	squish_dxt1 = ( 1 << 0 ), 
	
	//! Use DXT3 compression.
	squish_dxt3 = ( 1 << 1 ), 
	
	//! Use DXT5 compression.
	squish_dxt5 = ( 1 << 2 ), 
	
	//! Use a very slow but very high quality colour compressor.
	squish_colour_iterative_cluster_fit = ( 1 << 8 ),	
	
	//! Use a slow but high quality colour compressor (the default).
	squish_colour_cluster_fit = ( 1 << 3 ),	
	
	//! Use a fast but low quality colour compressor.
	squish_colour_range_fit = ( 1 << 4 ),
	
	//! Use a perceptual metric for colour error (the default).
	squish_colour_metric_perceptual = ( 1 << 5 ),

	//! Use a uniform metric for colour error.
	squish_colour_metric_uniform = ( 1 << 6 ),
	
	//! Weight the colour by alpha during cluster fit (disabled by default).
	squish_weight_colour_by_alpha = ( 1 << 7 )
}
squish_flags_t;

void squish_compress_image  (uint8_t *const rgba, const int width, const int height, void * blocks, int flags);
void squish_decompress_image(uint8_t *rgba, const int width, const int height, void const* blocks, int flags);
#ifdef __cplusplus
}
#endif
