#ifndef DT_DEVELOP_FREQSEP_H
#define DT_DEVELOP_FREQSEP_H

#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "develop/pixelpipe.h"
#include "common/opencl.h"

/** apply frequency separation */
void dt_develop_freqsep_preprocess(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
void dt_develop_freqsep_postprocess(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);

int dt_develop_freqsep_preprocess_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
int dt_develop_freqsep_postprocess_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);

#ifdef HAVE_OPENCL

/** apply frequency separation for opencl modules*/
int dt_develop_freqsep_preprocess_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
int dt_develop_freqsep_postprocess_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                              void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);


int dt_develop_freqsep_preprocess_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out);
int dt_develop_freqsep_postprocess_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out);
#endif

#endif
