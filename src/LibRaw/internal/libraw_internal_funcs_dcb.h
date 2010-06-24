
#ifndef _LIBRAW_INTERNAL_FUNCS_DCB_H
#define _LIBRAW_INTERNAL_FUNCS_DCB_H
    void        dcb_pp();
    void        copy_to_buffer(float (*image2)[3]);
    void        restore_from_buffer(float (*image2)[3]);
    void        hid();
    void        hid2();
    void        dcb_color();
    void        dcb_color_full();
    void        dcb_map();
    void        dcb_correction();
    void        dcb_correction2();
    void        dcb_refinement();
    void        rgb_to_lch(double (*image3)[3]);
    void        lch_to_rgb(double (*image3)[3]);
    void        fbdd_green2();
    void        fbdd_correction();
    void        fbdd_correction2(double (*image3)[3]);
    void        fbdd_green();
    void        fbdd_color();
#endif
