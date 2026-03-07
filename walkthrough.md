# darktable: A Complete Code Walkthrough

*2026-03-01T09:55:43Z by Showboat 0.6.1*
<!-- showboat-id: 92a37e58-c792-4558-bc73-e72e16496a1a -->

# 1. Entry Point

darktable starts in `src/main.c`. The main function is remarkably simple — it initializes the application with `dt_init()`, runs the GTK event loop, then cleans up on exit. All the complexity lives in the subsystems that `dt_init()` brings up.

The three key lines are: `dt_init()` to bring up all subsystems, `dt_gui_gtk_run()` to enter the GTK event loop, and `dt_cleanup()` on exit:

```bash
grep -n 'dt_init\|dt_gui_gtk_run\|dt_cleanup' src/main.c
```

```output
117:  if(dt_init(argc, argv, TRUE, TRUE, NULL))
158:    dt_gui_gtk_run(darktable.gui);
160:  dt_cleanup();
```

# 2. Initialization Sequence

`dt_init()` in `src/common/darktable.c` is a ~600-line function that boots every subsystem in dependency order. Here are the key initialization calls, in order:

```bash
grep -n 'dt_exif_init\|dt_conf_init\|dt_database_init\|dt_control_init\|dt_opencl_init\|dt_image_cache_init\|dt_mipmap_cache_init\|dt_gui_gtk_init\|dt_bauhaus_init\|dt_iop_load_modules_so\|dt_view_manager_init\|dt_signals_init\|dt_colorspaces_init\|dt_styles_init' src/common/darktable.c | head -20
```

```output
699:  dt_opencl_init(darktable.opencl, GPOINTER_TO_INT(dt_control_job_get_params(job)), TRUE);
1519:  dt_exif_init();
1541:  dt_conf_init(darktable.conf, darktablerc_common, TRUE, config_override);
1611:  dt_conf_init(darktable.conf, darktablerc, FALSE, config_override);
1636:  darktable.color_profiles = dt_colorspaces_init();
1643:  darktable.db = dt_database_init(dbfilename_from_command, load_data, init_gui);
1697:  dt_control_init(init_gui);
1840:    dt_opencl_init(darktable.opencl, exclude_opencl, print_statistics);
1852:  dt_image_cache_init();
1854:  dt_mipmap_cache_init();
1885:    if(dt_gui_gtk_init(darktable.gui))
1891:    dt_bauhaus_init();
1912:  dt_iop_load_modules_so();
1940:    dt_view_manager_init(darktable.view_manager);
```

Reading those line numbers top to bottom shows the boot order:
1. **EXIF library** (line 1519) — initialize image metadata handling
2. **Configuration** (line 1541, 1611) — load darktablerc settings
3. **Color profiles** (line 1636) — system color management
4. **Database** (line 1643) — open SQLite library/edit databases
5. **Control** (line 1697) — job scheduler, thread pool, signals
6. **OpenCL** (line 1840) — GPU device detection and setup
7. **Image cache** (line 1852) — lightweight metadata cache
8. **Mipmap cache** (line 1854) — pixel buffer cache for thumbnails/previews
9. **GTK GUI** (line 1885) — window creation, CSS theming
10. **Bauhaus** (line 1891) — custom slider/combobox widget system
11. **IOP modules** (line 1912) — load all processing module shared objects
12. **View manager** (line 1940) — register lighttable, darkroom, etc.

Note line 699: OpenCL also runs async as a background job for GPU kernel compilation. The line 1840 call is the synchronous init after CLI parsing.

# 3. The Global Singleton

Everything in darktable hangs off a single global variable: `darktable`, of type `darktable_t`. This struct is the hub connecting every subsystem. It's defined in `src/common/darktable.h`:

```bash
sed -n '366,433p' src/common/darktable.h
```

```output
typedef struct darktable_t
{
  dt_codepath_t codepath;
  int32_t num_openmp_threads;

  int32_t unmuted;
  GList *iop;
  GList *iop_order_list;
  GList *iop_order_rules;
  GList *capabilities;
  JsonParser *noiseprofile_parser;
  struct dt_conf_t *conf;
  struct dt_develop_t *develop;
  struct dt_lib_t *lib;
  struct dt_view_manager_t *view_manager;
  struct dt_control_t *control;
  struct dt_control_signal_t *signals;
  struct dt_gui_gtk_t *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t *image_cache;
  struct dt_bauhaus_t *bauhaus;
  const struct dt_database_t *db;
  const struct dt_pwstorage_t *pwstorage;
  struct dt_camctl_t *camctl;
  const struct dt_collection_t *collection;
  struct dt_selection_t *selection;
  struct dt_points_t *points;
  struct dt_imageio_t *imageio;
  struct dt_opencl_t *opencl;
  struct dt_dbus_t *dbus;
  struct dt_undo_t *undo;
  struct dt_colorspaces_t *color_profiles;
  struct dt_l10n_t *l10n;
  dt_pthread_mutex_t db_image[DT_IMAGE_DBLOCKS];
  dt_pthread_mutex_t dev_threadsafe;
  dt_pthread_mutex_t plugin_threadsafe;
  dt_pthread_mutex_t capabilities_threadsafe;
  dt_pthread_mutex_t exiv2_threadsafe;
  dt_pthread_mutex_t readFile_mutex;
  dt_pthread_mutex_t metadata_threadsafe;
  char *progname;
  char *datadir;
  char *sharedir;
  char *plugindir;
  char *localedir;
  char *tmpdir;
  char *configdir;
  char *cachedir;
  char *dump_pfm_module;
  char *dump_pfm_pipe;
  char *dump_diff_pipe;
  char *tmp_directory;
  char *bench_module;
  dt_lua_state_t lua_state;
  GList *guides;
  double start_wtime;
  GList *themes;
  int32_t unmuted_signal_dbg_acts;
  gboolean unmuted_signal_dbg[DT_SIGNAL_COUNT];
  gboolean pipe_cache;
  int gui_running;		// atomic, access with g_atomic_int_*()
  GTimeZone *utc_tz;
  GDateTime *origin_gdt;
  struct dt_sys_resources_t dtresources;
  struct dt_backthumb_t backthumbs;
  struct dt_gimp_t gimp;
  struct dt_splash_t splash;
} darktable_t;
```

Key pointers in this struct:
- **`develop`** — the active darkroom editing session (`dt_develop_t`)
- **`view_manager`** — manages switching between lighttable/darkroom/map/etc.
- **`control`** — job scheduler, thread pool, keyboard accelerators
- **`signals`** — publish/subscribe event system
- **`gui`** — GTK window state, CSS theme
- **`db`** — SQLite database connection (library.db + data.db)
- **`opencl`** — GPU devices and OpenCL kernels
- **`iop`** — list of all loaded processing module shared objects
- **`iop_order_list`** — default ordering of modules in the pipeline

# 4. View System

darktable is organized into "views" — distinct screens the user can switch between. The view types are defined as bit flags in `src/views/view.h`:

```bash
sed -n '52,65p' src/views/view.h
```

```output
typedef enum dt_view_type_flags_t
{
  DT_VIEW_NONE       = 0,
  DT_VIEW_LIGHTTABLE = 1 << 0,
  DT_VIEW_DARKROOM   = 1 << 1,
  DT_VIEW_TETHERING  = 1 << 2,
  DT_VIEW_MAP        = 1 << 3,
  DT_VIEW_SLIDESHOW  = 1 << 4,
  DT_VIEW_PRINT      = 1 << 5,
  DT_VIEW_MULTI      = 1 << 28,
  DT_VIEW_FALLBACK   = 1 << 29,
  DT_VIEW_OTHER      = 1 << 30, // for your own unpublished user view
  DT_VIEW_ALL        = ~DT_VIEW_FALLBACK,
} dt_view_type_flags_t;
```

Each view is implemented as a separate C file in `src/views/`:

```bash
ls src/views/*.c
```

```output
src/views/darkroom.c
src/views/lighttable.c
src/views/map.c
src/views/print.c
src/views/slideshow.c
src/views/tethering.c
src/views/view.c
```

Each view provides an `enter()` and `leave()` callback, plus `expose()` for drawing. The view manager handles transitions via `dt_view_manager_switch()`. Views also declare which lib modules (UI panels) they want visible — this is how the same sidebar can show different panels in lighttable vs darkroom.

# 5. The Darkroom & Develop Structure

When you double-click an image in lighttable, darktable switches to the darkroom view. The central data structure for editing is `dt_develop_t`, defined in `src/develop/develop.h`. This holds everything about the current editing session:

```bash
sed -n '165,230p' src/develop/develop.h
```

```output
typedef struct dt_develop_t
{
  gboolean gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be
                         // gui_init'ed.
  gboolean gui_leaving;  // set if everything is scheduled to shut down.
  gboolean gui_synch;    // set to TRUE by the render threads if gui_update should be called in the modules.

  gpointer gui_previous_target; // widget that was changed last time. If same again, don't save undo.
  double   gui_previous_time;   // last time that widget was changed. If too recent, don't save undo.
  double   gui_previous_pipe_time; // time pipe finished after last widget was changed.

  gboolean focus_hash;   // determines whether to start a new history item or to merge down.
  gboolean history_updating, image_force_reload, first_load;
  gboolean autosaving;
  double autosave_time;
  int32_t image_invalid_cnt;
  uint32_t timestamp;
  uint32_t preview_average_delay;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *preview_pipe;

  // image under consideration, which
  // is copied each time an image is changed. this means we have some information
  // always cached (might be out of sync, so stars are not reliable), but for the iops
  // it's quite a convenience to access trivial stuff which is constant anyways without
  // calling into the cache explicitly. this should never be accessed directly, but
  // by the iop through the copy their respective pixelpipe holds, for thread-safety.
  dt_image_t image_storage;
  dt_imgid_t requested_id;
  int32_t snapshot_id; /* for the darkroom snapshots */

  // history stack
  dt_pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;
  // some modules don't want to add new history items while active
  gboolean history_postpone_invalidate;
  // avoid checking for latest added module into history via list traversal
  struct dt_iop_module_t *history_last_module;

  // operations pipeline
  int32_t iop_instance;
  GList *iop;
  // iop's to be deleted
  GList *alliop;

  // iop order
  int iop_order_version;
  GList *iop_order_list;

  // profiles info
  GList *allprofile_info;

  // histogram for display.
  uint32_t *histogram_pre_tonecurve, *histogram_pre_levels;
  uint32_t histogram_pre_tonecurve_max, histogram_pre_levels_max;

  // list of forms iop can use for masks or whatever
  GList *forms;
  struct dt_masks_form_t *form_visible;
  struct dt_masks_form_gui_t *form_gui;
  // all forms to be linked here for cleanup:
  GList *allforms;

```

```bash
sed -n '230,270p' src/develop/develop.h
```

```output

  //full preview stuff
  gboolean full_preview;
  dt_dev_zoom_t full_preview_last_zoom;
  int full_preview_last_closeup;
  float full_preview_last_zoom_x, full_preview_last_zoom_y;
  struct dt_iop_module_t *full_preview_last_module;
  int full_preview_masks_state;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
    // list of exposure iop instances, with plugin hooks, used by
    // histogram dragging functions each element is
    // dt_dev_proxy_exposure_t
    dt_dev_proxy_exposure_t exposure;

    // this module receives right-drag events if not already claimed
    struct dt_iop_module_t *rotate;

    // modulegroups plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* switch module group */
      void (*set)(struct dt_lib_module_t *self,
                  const uint32_t group);
      /* get current module group */
      uint32_t (*get)(struct dt_lib_module_t *self);
      /* get activated module group */
      uint32_t (*get_activated)(struct dt_lib_module_t *self);
      /* test if iop group flags matches modulegroup */
      gboolean (*test)(struct dt_lib_module_t *self,
                       const uint32_t group,
                       struct dt_iop_module_t *module);
      /* switch to modulegroup */
      void (*switch_group)(struct dt_lib_module_t *self,
                           struct dt_iop_module_t *module);
      /* update modulegroup visibility */
      void (*update_visibility)(struct dt_lib_module_t *self);
      /* test if module is preset in one of the current groups */
```

Key fields in `dt_develop_t`:
- **`history`** / **`history_end`** — the non-destructive edit stack (GList of history items)
- **`iop`** — list of active module instances for this image
- **`preview_pipe`** — the pixel processing pipeline (see next section)
- **`image_storage`** — cached metadata for the current image
- **`forms`** — drawn masks (circles, paths, gradients) for parametric editing

The `dt_dev_chroma_t` struct facilitates communication between white balance and color modules:

```bash
sed -n '154,163p' src/develop/develop.h
```

```output
typedef struct dt_dev_chroma_t
{
  struct dt_iop_module_t *temperature;  // always available for GUI reports
  struct dt_iop_module_t *adaptation;   // set if one module is processing this without blending

  dt_aligned_pixel_t wb_coeffs;         // coeffs actually set by temperature
  double D65coeffs[4];                  // both read from exif data or "best guess"
  double as_shot[4];
  gboolean late_correction;
} dt_dev_chroma_t;
```

This struct is how `temperature.c` (white balance) communicates WB coefficients to `channelmixerrgb.c` (chromatic adaptation) and `colorin.c` — a key example of inter-module state sharing through the develop structure.

When the darkroom view is entered, the `enter()` callback sets up the editing session:

```bash
sed -n '3037,3075p' src/views/darkroom.c
```

```output
void enter(dt_view_t *self)
{
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  /* connect to ui pipe finished signal for redraw */
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                           _darkroom_ui_pipe_finish_signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED,
                           _darkroom_ui_preview2_pipe_finish_signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_TROUBLE_MESSAGE,
                           _display_module_trouble_message_callback);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode", dt_get_wtime());
  dt_develop_t *dev = self->data;
  
  // Reset shutdown flags on all pipes - they may still be set from previous session
  if(dev->full.pipe)
    dt_atomic_set_int(&dev->full.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  if(dev->preview_pipe)
    dt_atomic_set_int(&dev->preview_pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  if(dev->preview2.pipe)
    dt_atomic_set_int(&dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);
  dev->form_gui->pipe_hash = DT_INVALID_HASH;
  dev->form_gui->formid = NO_MASKID;
  dev->gui_leaving = FALSE;
  dev->gui_module = NULL;

  // change active image
```

# 6. The Pixel Pipeline

The pixel pipeline is darktable's core engine. It processes raw sensor data through a chain of image operation modules to produce the final image. The pipeline is defined in `src/develop/pixelpipe_hb.h`.

## The Pipeline Struct

`dt_dev_pixelpipe_t` represents one pipeline instance. Multiple pipes run simultaneously:

```bash
sed -n '107,207p' src/develop/pixelpipe_hb.h
```

```output
typedef struct dt_dev_pixelpipe_t
{
  // store history/zoom caches
  dt_dev_pixelpipe_cache_t cache;
  // set to TRUE in order to obsolete old cache entries on next pixelpipe run
  gboolean cache_obsolete;
  uint64_t runs; // used only for pixelpipe cache statistics
  // input buffer
  float *input;
  // width and height of input buffer
  int iwidth, iheight;
  // input actually just downscaled buffer? iscale*iwidth = actual width
  float iscale;
  // dimensions of processed buffer
  int processed_width, processed_height;

  // this one actually contains the expected output format,
  // and should be modified by process*(), if necessary.
  dt_iop_buffer_dsc_t dsc;

  /** work profile info of the image */
  struct dt_iop_order_iccprofile_info_t *work_profile_info;
  /** input profile info **/
  struct dt_iop_order_iccprofile_info_t *input_profile_info;
  /** output profile info **/
  struct dt_iop_order_iccprofile_info_t *output_profile_info;

  // instances of pixelpipe, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
  // event flag
  dt_dev_pixelpipe_change_t changed;
  // pipe status
  dt_dev_pixelpipe_status_t status;
  gboolean loading;
  gboolean input_changed;
  // backbuffer (output)
  uint8_t *backbuf;
  size_t backbuf_size;
  int backbuf_width, backbuf_height;
  float backbuf_scale;
  dt_dev_zoom_pos_t backbuf_zoom_pos;
  dt_hash_t backbuf_hash;
  dt_pthread_mutex_t mutex, backbuf_mutex, busy_mutex;
  int final_width, final_height;

  // the data for the luminance mask are kept in a buffer written by demosaic or rawprepare
  // as we have to scale the mask later we keep size at that stage
  gboolean want_detail_mask;
  struct dt_dev_detail_mask_t scharr;

  // avoid cached data for processed module
  gboolean nocache;

  dt_imgid_t output_imgid;
  // working?
  gboolean processing;
  /* shutting down?
     can be used in various ways defined in dt_dev_pixelpipe_stopper_t, in all cases the
       running pipe is stopped asap
     If we don't use one of the enum values this is interpreted as the iop_order of the module
     that has set this in case of an error condition or other reasons that request a re-run of the pipe.
     In those cases we assume cachelines after this module and the input of the stopper module
     are not valid cachelines any more so the pixelpipe takes care of this.
  */
  dt_atomic_int shutdown;
  // opencl enabled for this pixelpipe?
  gboolean opencl_enabled;
  // opencl error detected?
  gboolean opencl_error;
  // running in a tiling context?
  gboolean tiling;
  // should this pixelpipe display a mask in the end?
  dt_dev_pixelpipe_display_mask_t mask_display;
  // should this pixelpipe completely suppressed the blendif module?
  gboolean bypass_blendif;
  // input data based on this timestamp:
  int input_timestamp;
  uint32_t average_delay;
  dt_dev_pixelpipe_type_t type;
  // the final output pixel format this pixelpipe will be converted to
  dt_imageio_levels_t levels;
  // opencl device that has been locked for this pipe.
  int devid;
  // image struct as it was when the pixelpipe was initialized. copied to avoid race conditions.
  dt_image_t image;
  // the user might choose to overwrite the output color space and rendering intent.
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
  // snapshot of modules
  GList *iop;
  // snapshot of modules iop_order
  GList *iop_order_list;
  // snapshot of mask list
  GList *forms;
  // the masks generated in the pipe for later reusal are inside dt_dev_pixelpipe_iop_t
  gboolean store_all_raster_masks;
  // module blending cache
  float *bcache_data;
  dt_hash_t bcache_hash;
} dt_dev_pixelpipe_t;
```

## The Module Piece

Each module instance within a pipe is wrapped in a `dt_dev_pixelpipe_iop_t` struct (called a "piece"). This holds the module's processing parameters and cached data for that specific pipe:

```bash
sed -n '34,66p' src/develop/pixelpipe_hb.h
```

```output
typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t *module;  // the module in the dev operation stack
  struct dt_dev_pixelpipe_t *pipe; // the pipe this piece belongs to
  void *data;                      // to be used by the module to store stuff per pipe piece
  void *blendop_data;              // to be used by the module to store blendop per pipe piece
  gboolean enabled; // used to disable parts of the pipe for export, independent on module itself.

  dt_dev_request_flags_t request_histogram;              // (bitwise) set if you want an histogram captured
  dt_dev_histogram_collection_params_t histogram_params; // set histogram generation params
  uint32_t *histogram; // pointer to histogram data; histogram_bins_count bins with 4 channels each
  dt_dev_histogram_stats_t histogram_stats; // stats of captured histogram
  uint32_t histogram_max[4];                // maximum levels in histogram, one per channel

  float iscale;                   // input actually just downscaled buffer? iscale*iwidth = actual width
  int iwidth, iheight;            // width and height of input buffer
  dt_hash_t hash;                 // hash of params and enabled.
  int bpc;                        // bits per channel, 32 means float
  int colors;                     // how many colors per pixel
  dt_iop_roi_t buf_in;            // theoretical full buffer regions of interest, as passed through modify_roi_out
  dt_iop_roi_t buf_out;
  dt_iop_roi_t processed_roi_in;  // the actual roi that was used for processing the piece
  dt_iop_roi_t processed_roi_out;
  gboolean process_cl_ready;      // set this to FALSE in commit_params to temporarily disable the use of process_cl
  gboolean process_tiling_ready;  // set this to FALSE in commit_params to temporarily disable tiling

  // the following are used internally for caching:
  dt_iop_buffer_dsc_t dsc_in;
  dt_iop_buffer_dsc_t dsc_out;
  uint8_t xtrans[6][6];
  uint32_t filters;
  GHashTable *raster_masks;
} dt_dev_pixelpipe_iop_t;
```

## Multiple Pipes

darktable runs several pipelines simultaneously:
- **full** — renders the zoomed/panned region for the main darkroom view
- **preview** — renders a small version for the navigation thumbnail
- **preview2** — used for second window or snapshot comparisons
- **export** — full-resolution output during file export
- **thumbnail** — lighttable thumbnail generation

## Two-Pass Processing

Pipeline processing happens in two passes:
1. **Backward ROI (Region of Interest) propagation**: Starting from the output size, each module's `modify_roi_in()` calculates what input region it needs. This walks backwards through the chain.
2. **Forward pixel processing**: Starting from the raw input, each module's `process()` transforms pixels. This walks forward through the chain.

Here's the entry point for pipeline processing:

```bash
sed -n '3111,3131p' src/develop/pixelpipe_hb.c
```

```output
gboolean dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe,
                                  dt_develop_t *dev,
                                  const int x,
                                  const int y,
                                  const int width,
                                  const int height,
                                  const float scale,
                                  const int devid)
{
  pipe->processing = TRUE;
  pipe->nocache = (pipe->type & DT_DEV_PIXELPIPE_IMAGE) != 0;
  pipe->runs++;
  pipe->opencl_enabled = dt_opencl_running();

  // if devid is a valid CL device we don't lock it as the caller has done so already
  const gboolean claimed = devid > DT_DEVICE_CPU;
  pipe->devid = pipe->opencl_enabled
    ? (claimed ? devid : dt_opencl_lock_device(pipe->type))
    : DT_DEVICE_CPU;

  if(!claimed)  // don't free cachelines as the caller is using them
```

# 7. IOP Module System

Image Operation (IOP) modules are the building blocks of the pixel pipeline. Each module is a `.c` file in `src/iop/` that gets compiled into a shared object and loaded at runtime. There are roughly 100 modules:

```bash
ls src/iop/*.c | wc -l && echo '---' && ls src/iop/*.c | head -20
```

```output
92
---
src/iop/agx.c
src/iop/ashift.c
src/iop/ashift_lsd.c
src/iop/ashift_nmsimplex.c
src/iop/atrous.c
src/iop/basecurve.c
src/iop/basicadj.c
src/iop/bilat.c
src/iop/bloom.c
src/iop/blurs.c
src/iop/borders.c
src/iop/cacorrect.c
src/iop/cacorrectrgb.c
src/iop/censorize.c
src/iop/channelmixer.c
src/iop/channelmixerrgb.c
src/iop/clahe.c
src/iop/clipping.c
src/iop/colisa.c
src/iop/colorbalance.c
```

## The Module Instance Struct

Each loaded module is represented by `dt_iop_module_t` (from `src/develop/imageop.h`). This is a large struct — here are the key fields:

```bash
sed -n '168,210p' src/develop/imageop.h
```

```output
typedef struct dt_iop_module_t
{
  dt_action_type_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "iop/iop_api.h"

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order of the module on the pipe. the pipe will be sorted by iop_order. */
  int iop_order;
  /** position id of module in pipe */
  int position;
  /** module sets this if the enable checkbox should be hidden. */
  gboolean hide_enable_button;
  /** set to DT_REQUEST_COLORPICK_MODULE if you want an input color
   * picked during next eval. gui mode only. */
  dt_dev_request_colorpick_flags_t request_color_pick;
  /** (bitwise) set if you want an histogram generated during next eval */
  dt_dev_request_flags_t request_histogram;
  /** set to 1 if you want the mask to be transferred into alpha
   * channel during next eval. gui mode only. */
  dt_dev_pixelpipe_display_mask_t request_mask_display;
  /** set to 1 if you want the blendif mask to be suppressed in the
   * module in focus. gui mode only. */
  gboolean suppress_mask;
  /** place to store the picked color of module input. */
  dt_aligned_pixel_t picked_color, picked_color_min, picked_color_max;
  /** place to store the picked color of module output (before blending). */
  dt_aligned_pixel_t picked_output_color, picked_output_color_min, picked_output_color_max;
  /** pointer to pre-module histogram data; if available:
   * histogram_bins_count bins with 4 channels each */
  uint32_t *histogram;
  /** stats of captured histogram */
  dt_dev_histogram_stats_t histogram_stats;
  /** maximum levels in histogram, one per channel */
  uint32_t histogram_max[4];
  /** requested colorspace for the histogram, valid options are:
   * IOP_CS_NONE: module colorspace
```

Note line 173: the module's API functions are pulled in via `#include "iop/iop_api.h"` — this is a header that declares function pointers for every possible module callback (`process()`, `commit_params()`, `gui_init()`, etc.).

## The Useless Example Module

`src/iop/useless.c` is the documented example module. It shows the minimum required API. First, the introspection macro and parameter struct:

```bash
sed -n '56,98p' src/iop/useless.c
```

```output
DT_MODULE_INTROSPECTION(3, dt_iop_useless_params_t)

// TODO: some build system to support dt-less compilation and translation!

// Enums used in params_t can have $DESCRIPTIONs that will be used to
// automatically populate a combobox with dt_bauhaus_combobox_from_params.
// They are also used in the history changes tooltip.
// Combobox options will be presented in the same order as defined here.
// These numbers must not be changed when a new version is introduced.
typedef enum dt_iop_useless_type_t
{
  DT_USELESS_NONE = 0,     // $DESCRIPTION: "no"
  DT_USELESS_FIRST = 1,    // $DESCRIPTION: "first option"
  DT_USELESS_SECOND = 2,   // $DESCRIPTION: "second one"
} dt_iop_useless_type_t;

typedef struct dt_iop_useless_params_t
{
  // The parameters defined here fully record the state of the module
  // and are stored (as a serialized binary blob) into the db.  Make
  // sure everything is in here does not depend on temporary memory
  // (pointers etc).  This struct defines the layout of self->params
  // and self->default_params.  You should keep changes to this struct
  // to a minimum.  If you have to change this struct, it will break
  // user data bases, and you have to increment the version of
  // DT_MODULE_INTROSPECTION(VERSION) above and provide a
  // legacy_params upgrade path!
  //
  // Tags in the comments get picked up by the introspection framework
  // and are used in gui_init to set range and labels (for widgets and
  // history) and value checks before commit_params.  If no explicit
  // init() is specified, the default implementation uses $DEFAULT
  // tags to initialise self->default_params, which is then used in
  // gui_init to set widget defaults.
  //
  // These field names are just examples; chose meaningful ones! For
  // performance reasons, align to 4 byte boundaries (use gboolean,
  // not bool).
  int checker_scale; // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "size"
  float factor;      // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0
  gboolean check;    // $DESCRIPTION: "checkbox option"
  dt_iop_useless_type_t method; // $DEFAULT: DT_USELESS_SECOND $DESCRIPTION: "parameter choices"
} dt_iop_useless_params_t;
```

The `$MIN`, `$MAX`, `$DEFAULT`, and `$DESCRIPTION` comment tags are parsed by the introspection system. They drive automatic widget creation and parameter validation — no boilerplate needed.

The minimum required functions:

```bash
sed -n '119,125p' src/iop/useless.c
```

```output
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("silly example");
}

// a routine returning the description of the module. this is
```

```bash
sed -n '342,380p' src/iop/useless.c
```

```output
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with
  // its own pixelpipe piece.  get our data struct:
  dt_iop_useless_params_t *d = piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  // how many colors in our buffer?
  const size_t ch = piece->colors;

  // most modules only support a single type of input data, so we can
  // check whether that format has been supplied and simply pass along
  // the data if not (setting a trouble flag to inform the user)
  dt_iop_useless_gui_data_t *g = self->gui_data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  // we create a raster mask as an example
  float *mask = NULL;
  if(dt_iop_piece_is_raster_mask_used(piece, mask_id))
  {
    // Attempt to allocate all of the buffers we need.  For this
    // example, we need one buffer that is equal in dimensions to the
    // output buffer, has one color channel, and has been zero'd.
    // (See common/imagebuf.h for more details on all of the options.)
    if(!dt_iop_alloc_image_buffers
       (module, roi_in, roi_out,
        1/*ch per pixel*/ | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL | DT_IMGSZ_CLEARBUF, &mask,
        0 /* end of list of buffers to allocate */))
    {
      // Uh oh, we didn't have enough memory!  If multiple buffers
```

The `process()` function signature is the heart of every module:
- `self` — the module instance
- `piece` — the pipe-specific data (contains `piece->data` with committed params)
- `ivoid` / `ovoid` — input and output pixel buffers
- `roi_in` / `roi_out` — regions of interest (coordinates, scale)

## Module Loading and Ordering

At startup, `dt_iop_load_modules_so()` scans the plugin directory for shared objects:

```bash
sed -n '1775,1820p' src/develop/imageop.c
```

```output
void dt_iop_load_modules_so(void)
{
  darktable.iop = dt_module_load_modules
    ("/plugins", sizeof(dt_iop_module_so_t),
     dt_iop_load_module_so, _init_module_so, NULL);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE,
                            _iop_preferences_changed, darktable.iop);

  // set up memory.darktable_iop_names table
  _iop_set_darktable_iop_table();

  // after loading the iop table we want to refresh the collection if it uses
  // the module name as part of the query as we have just setup the iop name
  // table.

  if(dt_collection_has_property(DT_COLLECTION_PROP_MODULE))
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY,
                               DT_COLLECTION_PROP_UNDEF, NULL);
}

gboolean dt_iop_load_module(dt_iop_module_t *module,
                            dt_iop_module_so_t *module_so,
                            dt_develop_t *dev)
{
  memset(module, 0, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, dev))
  {
    free(module);
    return TRUE;
  }
  return FALSE;
}

GList *dt_iop_load_modules_ext(dt_develop_t *dev, const gboolean no_image)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = iop->data;
    module = calloc(1, sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
```

Modules are loaded from the `/plugins` directory as shared objects. Each image then gets its own set of module instances via `dt_iop_load_modules_ext()`, which creates `dt_iop_module_t` instances from the shared `dt_iop_module_so_t` templates.

Module ordering in the pipeline is controlled by the `iop_order` field — a floating point number that determines each module's position. The default order is defined in `src/develop/imageop_order.c` and can be customized per-image.

# 8. History Stack — Non-Destructive Editing

darktable never modifies the original image file. Instead, every edit is recorded as a history item. The history stack is the ordered list of all parameter changes the user has made.

## History Item

```bash
sed -n '34,49p' src/develop/develop.h
```

```output
typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  gboolean enabled;               // switched respective module on/off
  dt_iop_params_t *params;        // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
  char op_name[20];
  int iop_order;
  int multi_priority;
  char multi_name[128];
  gboolean multi_name_hand_edited;
  GList *forms;        // snapshot of dt_develop_t->forms
  int num;             // num of history on database
  gboolean focus_hash;  // used to determine whether or not to start a
                       // new item or to merge down
} dt_dev_history_item_t;
```

Each history item stores the module it belongs to, the full parameter blob, blend/mask parameters, and the module's pipeline position. The `num` field maps to the row in the SQLite database.

## Database Storage

History is persisted to SQLite (`library.db`) and also written to XMP sidecar files. Here's a representative database operation from `src/common/history.c`:

```bash
sed -n '62,120p' src/common/history.c
```

```output
void dt_history_delete_on_image_ext(const dt_imgid_t imgid,
                                    const gboolean undo,
                                    const gboolean init_history)
{
  if(!dt_is_valid_imgid(imgid))
    return;
  dt_undo_lt_history_t *hist = undo ? dt_history_snapshot_item_init() : NULL;

  if(undo)
  {
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  dt_lock_image(imgid);

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = 0, aspect_ratio = 0.0, thumb_timestamp = -1, thumb_maxmip = 0"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history_hash WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // remove all overlays for this image
  dt_overlays_remove(imgid);
```

This shows the canonical pattern: prepare SQL statement, bind parameters, step, finalize. When history is deleted, it cascades across multiple tables: history, module_order, masks_history, and history_hash.

## History Panel UI

The history panel in the darkroom sidebar is implemented as a lib module in `src/libs/history.c`:

```bash
sed -n '96,99p' src/libs/history.c && echo '---' && sed -n '127,157p' src/libs/history.c
```

```output
const char *name(dt_lib_module_t *self)
{
  return _("history");
}
---
void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_history_t *d = g_malloc0(sizeof(dt_lib_history_t));
  self->data = (void *)d;

  d->record_undo = TRUE;
  d->record_history_level = 0;

  d->history_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_has_tooltip(d->history_box, FALSE);

  d->compress_button = dt_action_button_new
    (self, N_("compress history stack"), _lib_history_compress_clicked_callback, self,
     _("create a minimal history stack which produces the same image\n"
       "ctrl+click to truncate history to the selected item"), 0, 0);
  g_signal_connect(G_OBJECT(d->compress_button), "button-press-event",
                   G_CALLBACK(_lib_history_compress_pressed_callback), self);

  /* add toolbar button for creating style */
  d->create_button = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_NONE, NULL);
  g_signal_connect(G_OBJECT(d->create_button), "clicked",
                   G_CALLBACK(_lib_history_create_style_button_clicked_callback), NULL);
  gtk_widget_set_name(d->create_button, "non-flat");
  gtk_widget_set_tooltip_text(d->create_button,
                              _("create a style from the current history stack"));
  dt_action_define(DT_ACTION(self), NULL,
                   N_("create style from history"),
                   d->create_button, &dt_action_def_button);

  self->widget = dt_gui_vbox
```

# 9. Signal System

darktable uses a publish/subscribe signal system for loose coupling between components. Signals are defined in `src/control/signal.h`:

```bash
sed -n '30,90p' src/control/signal.h
```

```output
typedef enum dt_signal_t
{
  /** \brief This signal is raised when mouse hovers over image thumbs
      both on lighttable and in the filmstrip.
      no param, no returned value
   */
  DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,

  /** \brief This signal is raised when image shown in the main view change
      no param, no returned value
   */
  DT_SIGNAL_ACTIVE_IMAGES_CHANGE,

  /** \brief This signal is raised when dt_control_queue_redraw() is called.
    no param, no returned value
  */
  DT_SIGNAL_CONTROL_REDRAW_ALL,

  /** \brief This signal is raised when dt_control_queue_redraw_center() is called.
    no param, no returned value
   */
  DT_SIGNAL_CONTROL_REDRAW_CENTER,

  /** \brief This signal is raised by viewmanager when a view has changed.
    1 : dt_view_t * the old view
    2 : dt_view_t * the new (current) view
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,

  /** \brief This signal is raised by viewmanager when a view has changed.
    1 : dt_view_t * the old view
    2 : dt_view_t * the new (current) view
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,

  /** \brief This signal is raised when a thumb is double-clicked in
    thumbtable (filemananger, filmstrip)
    1 : int the imageid of the thumbnail
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,

  /** \brief This signal is raised when collection changed. To avoid leaking the list,
    dt_collection_t is connected to this event and responsible of that.
    1 : dt_collection_change_t the reason why the collection has changed
    2 : dt_collection_properties_t the property that has changed
    3 : GList of imageids that have changed (can be null if it's a global change)
    4 : next untouched imgid in the list (-1 if no list)
    no returned value
    */
  /** image list not to be freed by the caller, automatically freed */
  DT_SIGNAL_COLLECTION_CHANGED,

  /** \brief This signal is raised when the selection is changed
  no param, no returned value
    */
  DT_SIGNAL_SELECTION_CHANGED,

  /** \brief This signal is raised when a tag is added/deleted/changed  */
```

Modules connect to signals with `DT_CONTROL_SIGNAL_CONNECT()` and raise them with `DT_CONTROL_SIGNAL_RAISE()`. This decouples components — for example, when the collection changes, the filmstrip, lighttable, and various panels all get notified without direct function calls between them.

Key signal groups:
- **`DT_SIGNAL_DEVELOP_*`** — pipeline events (module added, history changed, pipe finished)
- **`DT_SIGNAL_COLLECTION_CHANGED`** — the image set has changed
- **`DT_SIGNAL_VIEWMANAGER_*`** — view transitions and thumbnail actions
- **`DT_SIGNAL_IMAGE_*`** — metadata or edit changes to images

# 10. GUI & Bauhaus Widgets

darktable uses GTK3 for its UI, with a custom widget library called "bauhaus" for the sliders and comboboxes you see in every module.

## Main GUI State

```bash
sed -n '111,149p' src/gui/gtk.h
```

```output
typedef struct dt_gui_gtk_t
{
  struct dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  dt_gui_scrollbars_t scrollbars;

  cairo_surface_t *surface;  // cached prior image when config var ui/loading_screen is FALSE
  gboolean drawing_snapshot;

  char *last_preset;

  int32_t reset;
  GdkRGBA colors[DT_GUI_COLOR_LAST];

  int32_t hide_tooltips;

  gboolean grouping;
  dt_imgid_t expanded_group_id;

  gboolean show_overlays;
  gboolean show_focus_peaking;
  double overlay_red, overlay_blue, overlay_green, overlay_contrast;
  GtkWidget *focus_peaking_button;

  double dpi, dpi_factor, ppd, ppd_thb;
  gboolean have_pen_pressure;

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  gint scroll_mask;
  guint sidebar_scroll_mask;

  cairo_filter_t filter_image;    // filtering used to scale images to screen
} dt_gui_gtk_t;
```

## Bauhaus Widgets

The bauhaus system (`src/bauhaus/`) provides custom sliders and comboboxes with a distinctive look and keyboard/scroll interaction. The key innovation is `dt_bauhaus_*_from_params()` — functions that auto-create widgets bound to module parameters via introspection:

```bash
sed -n '24,30p' src/develop/imageop_gui.h
```

```output

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param);

```

These functions look up the named field in the module's `params_t` struct via introspection, read the `$MIN/$MAX/$DEFAULT/$DESCRIPTION` tags, and create a fully-configured widget that automatically reads from and writes to the module's parameters. This eliminates massive amounts of boilerplate in module GUI code.

Here's how the useless example module uses it in `gui_init()`:

```bash
sed -n '610,652p' src/iop/useless.c
```

```output
void gui_init(dt_iop_module_t *self)
{
  // Allocates memory for the module's user interface in the darkroom
  // and sets up the widgets in it.
  //
  // self->widget needs to be set to the top level widget.  This can
  // be a (vertical) box, a grid or even a notebook. Modules that are
  // disabled for certain types of images (for example non-raw) may
  // use a stack where one of the pages contains just a label
  // explaining why it is disabled.
  //
  // Widgets that are directly linked to a field in params_t may be
  // set up using the dt_bauhaus_..._from_params family. They take a
  // string with the field name in the params_t struct definition. The
  // $MIN, $MAX and $DEFAULT tags will be used to set up the widget
  // (slider) ranges and default values and the $DESCRIPTION is used
  // as the widget label.
  //
  // The _from_params calls also set up an automatic callback that
  // updates the field in params whenever the widget is changed. In
  // addition, gui_changed is called, if it exists, so that any other
  // required changes, to dependent fields or to gui widgets, can be
  // made.
  //
  // Whenever self->params changes (switching images or history) the
  // widget values have to be updated in gui_update.
  //
  // Do not set the value of widgets or configure them depending on
  // field values here; this should be done in gui_update (or
  // gui_changed or individual widget callbacks)
  //
  // If any default values for(slider) widgets or options (in
  // comboboxes) depend on the type of image, then the widgets have to
  // be updated in reload_params.
  dt_iop_useless_gui_data_t *g = IOP_GUI_ALLOC(useless);

  // If the first widget is created using a _from_params call,
  // self->widget does not have to be explicitly initialised, as a new
  // vertical box will be created automatically.
  self->widget = dt_gui_vbox();

  // Linking a slider to an integer will make it take only whole
  // numbers (step=1).  The new slider is added to self->widget
```

```bash
sed -n '652,685p' src/iop/useless.c
```

```output
  // numbers (step=1).  The new slider is added to self->widget
  g->scale = dt_bauhaus_slider_from_params(self, "checker_scale");

  // If the field name should be used as label too, it does not need a
  // $DESCRIPTION; mark it for translation here using N_()
  //
  // A colorpicker can be attached to a slider, as here, or put
  // standalone in a box.  When a color is picked, color_picker_apply
  // is called with either the slider or the button that triggered it.
  g->factor = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
              dt_bauhaus_slider_from_params(self, N_("factor")));
  // The initial slider range can be reduced from the introspection $MIN - $MAX
  dt_bauhaus_slider_set_soft_range(g->factor, 0.5f, 1.5f);
  // The default step is range/100, but can be changed here
  dt_bauhaus_slider_set_step(g->factor, .1);
  dt_bauhaus_slider_set_digits(g->factor, 2);
  // Additional parameters determine how the value will be shown.
  dt_bauhaus_slider_set_format(g->factor, "%");
  // For a percentage, use factor 100.
  dt_bauhaus_slider_set_factor(g->factor, -100.0f);
  dt_bauhaus_slider_set_offset(g->factor, 100.0f);
  // Tooltips explain the otherwise compact interface
  gtk_widget_set_tooltip_text(g->factor, _("adjust factor"));

  // A combobox linked to struct field will be filled with the values
  // and $DESCRIPTIONs in the struct definition, in the same
  // order. The automatic callback will put the enum value, not the
  // position within the combobox list, in the field.
  g->method = dt_bauhaus_combobox_from_params(self, "method");

  g->check = dt_bauhaus_toggle_from_params(self, "check");

  // Any widgets that are _not_ directly linked to a field need to
  // have a custom callback function set up to respond to the
```

This is the magic of the bauhaus introspection system: `dt_bauhaus_slider_from_params(self, "checker_scale")` creates a fully-configured slider just by knowing the field name in `params_t`. The `$MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "size"` tags from the struct definition drive everything.

## Lib Modules

UI panels (sidebars, bottom panel, etc.) are implemented as "lib" modules in `src/libs/`. Unlike IOP modules, lib modules don't process pixels — they provide UI. Each lib module declares which views it's visible in:

```bash
ls src/libs/*.c | head -20 && echo '...' && ls src/libs/*.c | wc -l
```

```output
src/libs/backgroundjobs.c
src/libs/camera.c
src/libs/collect.c
src/libs/colorpicker.c
src/libs/copy_history.c
src/libs/duplicate.c
src/libs/export.c
src/libs/export_metadata.c
src/libs/filtering.c
src/libs/geotagging.c
src/libs/histogram.c
src/libs/history.c
src/libs/image.c
src/libs/import.c
src/libs/ioporder.c
src/libs/lib.c
src/libs/live_view.c
src/libs/location.c
src/libs/map_locations.c
src/libs/map_settings.c
...
32
```

# 11. OpenCL GPU Acceleration

darktable can offload pixel processing to the GPU via OpenCL. The OpenCL subsystem is managed by `dt_opencl_t` in `src/common/opencl.h`:

```bash
sed -n '227,279p' src/common/opencl.h
```

```output
typedef struct dt_opencl_t
{
  dt_pthread_mutex_t lock;
  gboolean inited;
  gboolean print_statistics;
  gboolean enabled;
  gboolean stopped;
  int num_devs;
  int error_count;
  int opencl_synchronization_timeout;
  gboolean api30;
  dt_opencl_scheduling_profile_t scheduling_profile;
  uint32_t crc;
  gboolean mandatory[5];
  int *dev_priority_image;
  int *dev_priority_preview;
  int *dev_priority_preview2;
  int *dev_priority_export;
  int *dev_priority_thumbnail;
  dt_opencl_device_t *dev;
  dt_dlopencl_t *dlocl;

  // global kernels for blending operations.
  struct dt_blendop_cl_global_t *blendop;

  // global kernels for bilateral filtering, to be reused by a few plugins.
  struct dt_bilateral_cl_global_t *bilateral;

  // global kernels for gaussian filtering, to be reused by a few plugins.
  struct dt_gaussian_cl_global_t *gaussian;

  // global kernels for interpolation resampling.
  struct dt_interpolation_cl_global_t *interpolation;

  // global kernels for local laplacian filter.
  struct dt_local_laplacian_cl_global_t *local_laplacian;

  // global kernels for dwt filter.
  struct dt_dwt_cl_global_t *dwt;

  // global kernels for heal filter.
  struct dt_heal_cl_global_t *heal;

  // global kernels for colorspaces filter.
  struct dt_colorspaces_cl_global_t *colorspaces;

  // global kernels for guided filter.
  struct dt_guided_filter_cl_global_t *guided_filter;

  // saved kernel info for deferred initialisation
  int program_saved[DT_OPENCL_MAX_KERNELS];
  const char *name_saved[DT_OPENCL_MAX_KERNELS];
} dt_opencl_t;
```

Key aspects of the OpenCL system:

- **Device scheduling**: Each pipeline type (image, preview, export) has its own device priority list, allowing different GPU assignment strategies.
- **Global kernels**: Common operations (blending, gaussian, bilateral filtering) are compiled once and shared across modules.
- **Dual code paths**: IOP modules that support GPU acceleration implement both `process()` (CPU) and `process_cl()` (GPU). The pipeline automatically picks the right one based on device availability.
- **Tiling**: When an image is too large for GPU memory, the pipeline automatically tiles the processing — splitting the image into chunks that fit in VRAM.

Here's how a module declares GPU support (from `src/iop/useless.c`):

```bash
sed -n '109,116p' src/iop/useless.c && echo '---' && sed -n '455,470p' src/iop/useless.c
```

```output
typedef struct dt_iop_useless_global_data_t
{
  // This is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.

  // We don't need it for this example (as for most dt plugins).
} dt_iop_useless_global_data_t;
---
void init_global(dt_iop_module_so_t *self)
{
  self->data = malloc(sizeof(dt_iop_useless_global_data_t));
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
  // Releases any memory allocated in init(module) Implement this
  // function explicitly if the module allocates additional memory
  // besides (default_)params.  this is rare.
}

void cleanup_global(dt_iop_module_so_t *self)
{
  free(self->data);
```

`global_data_t` persists for the lifetime of the application (not per-image). For OpenCL modules, this is where compiled GPU kernel handles are stored. `init_global()` compiles the kernels once at startup; `cleanup_global()` releases them.

# Summary

darktable's architecture follows a clear pattern:

1. **A global singleton** (`darktable_t`) connects all subsystems
2. **Views** organize the UI into distinct screens (lighttable, darkroom, etc.)
3. **The pixel pipeline** processes raw data through a chain of IOP modules
4. **IOP modules** are self-contained plugins with standardized interfaces loaded from shared objects
5. **The history stack** records every edit non-destructively in SQLite + XMP
6. **Signals** decouple components via publish/subscribe events
7. **Bauhaus widgets** with introspection eliminate GUI boilerplate
8. **OpenCL** provides optional GPU acceleration with automatic fallback to CPU

The codebase is large (~400K lines of C) but well-organized. The key directories to know:
- `src/iop/` — pixel processing modules (the creative engine)
- `src/develop/` — pipeline infrastructure and editing state  
- `src/views/` — application screens
- `src/libs/` — UI panels
- `src/common/` — shared utilities, database, configuration
- `src/gui/` — GTK framework and bauhaus widgets
- `src/control/` — job scheduler, signals, accelerators
