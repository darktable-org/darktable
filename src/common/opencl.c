/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011--2017 Ulrich Pegelow.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_OPENCL

#include "common/opencl.h"
#include "common/bilateralcl.h"
#include "common/locallaplaciancl.h"
#include "common/darktable.h"
#include "common/dlopencl.h"
#include "common/gaussian.h"
#include "common/interpolation.h"
#include "common/dwt.h"
#include "common/heal.h"
#include "common/nvidia_gpus.h"
#include "common/opencl_drivers_blacklist.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/pixelpipe.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <zlib.h>

static const char *dt_opencl_get_vendor_by_id(unsigned int id);
static float dt_opencl_benchmark_gpu(const int devid, const size_t width, const size_t height, const int count, const float sigma);
static float dt_opencl_benchmark_cpu(const size_t width, const size_t height, const int count, const float sigma);
static char *_ascii_str_canonical(const char *in, char *out, int maxlen);
/** parse a single token of priority string and store priorities in priority_list */
static void dt_opencl_priority_parse(dt_opencl_t *cl, char *configstr, int *priority_list, int *mandatory);
/** parse a complete priority string */
static void dt_opencl_priorities_parse(dt_opencl_t *cl, const char *configstr);
/** set device priorities according to config string */
static void dt_opencl_update_priorities(const char *configstr);
/** read scheduling profile for config variables */
static dt_opencl_scheduling_profile_t dt_opencl_get_scheduling_profile(void);
/** adjust opencl subsystem according to scheduling profile */
static void dt_opencl_apply_scheduling_profile(dt_opencl_scheduling_profile_t profile);
/** set opencl specific synchronization timeout */
static void dt_opencl_set_synchronization_timeout(int value);


int dt_opencl_get_device_info(dt_opencl_t *cl, cl_device_id device, cl_device_info param_name, void **param_value,
                              size_t *param_value_size)
{
  cl_int err;

  *param_value_size = SIZE_MAX;

  // 1. figure out how much memory is needed
  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(device, param_name, 0, NULL, param_value_size);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] could not query the actual size in bytes of info %d: %d\n", param_name,
             err);
    goto error;
  }

  // 2. did we /actually/ get the size?
  if(*param_value_size == SIZE_MAX || *param_value_size == 0)
  {
    // both of these sizes make no sense. either i failed to parse spec, or opencl implementation bug?
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] ERROR: no size returned, or zero size returned for data %d: %zu\n",
             param_name, *param_value_size);
    err = CL_INVALID_VALUE; // FIXME: anything better?
    goto error;
  }

  // 3. make sure that *param_value points to big-enough memory block
  {
    void *ptr = realloc(*param_value, *param_value_size);
    if(!ptr)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dt_opencl_get_device_info] memory allocation failed! tried to allocate %zu bytes for data %d: %d",
               *param_value_size, param_name, err);
      err = CL_OUT_OF_HOST_MEMORY;
      goto error;
    }

    // allocation succeeed, update pointer.
    *param_value = ptr;
  }

  // 4. actually get the value
  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(device, param_name, *param_value_size, *param_value, NULL);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[dt_opencl_get_device_info] could not query info %d: %d\n", param_name, err);
    goto error;
  }

  return CL_SUCCESS;

error:
  free(*param_value);
  *param_value = NULL;
  param_value_size = 0;
  return err;
}

// returns 0 if all ok
// returns 1 if we failed hard, and need to skip opencl initialization
// returns -1 if we failed to init this device
static int dt_opencl_device_init(dt_opencl_t *cl, const int dev, cl_device_id *devices, const int k,
                                 const int opencl_memory_requirement)
{
  int res;
  cl_int err;

  memset(cl->dev[dev].program, 0x0, sizeof(cl_program) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].program_used, 0x0, sizeof(int) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].kernel, 0x0, sizeof(cl_kernel) * DT_OPENCL_MAX_KERNELS);
  memset(cl->dev[dev].kernel_used, 0x0, sizeof(int) * DT_OPENCL_MAX_KERNELS);
  cl->dev[dev].eventlist = NULL;
  cl->dev[dev].eventtags = NULL;
  cl->dev[dev].numevents = 0;
  cl->dev[dev].eventsconsolidated = 0;
  cl->dev[dev].maxevents = 0;
  cl->dev[dev].lostevents = 0;
  cl->dev[dev].totalevents = 0;
  cl->dev[dev].totalsuccess = 0;
  cl->dev[dev].totallost = 0;
  cl->dev[dev].summary = CL_COMPLETE;
  cl->dev[dev].used_global_mem = 0;
  cl->dev[dev].nvidia_sm_20 = 0;
  cl->dev[dev].vendor = NULL;
  cl->dev[dev].name = NULL;
  cl->dev[dev].cname = NULL;
  cl->dev[dev].options = NULL;
  cl->dev[dev].memory_in_use = 0;
  cl->dev[dev].peak_memory = 0;
  cl_device_id devid = cl->dev[dev].devid = devices[k];

  char *infostr = NULL;
  size_t infostr_size;

  char *cname = NULL;
  size_t cname_size;

  char *options = NULL;

  char *vendor = NULL;
  size_t vendor_size;

  char *driverversion = NULL;
  size_t driverversion_size;

  char *deviceversion = NULL;
  size_t deviceversion_size;

  size_t infoint;
  size_t *infointtab = NULL;
  cl_device_type type;
  cl_bool image_support = 0;
  cl_bool device_available = 0;
  cl_uint vendor_id = 0;
  cl_bool little_endian = 0;

  // test GPU availability, vendor, memory, image support etc:
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_AVAILABLE, sizeof(cl_bool), &device_available, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VENDOR, (void **)&vendor, &vendor_size);
  if(err != CL_SUCCESS)
  {
    res = 1;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_VENDOR_ID, sizeof(cl_uint), &vendor_id, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_NAME, (void **)&infostr, &infostr_size);
  if(err != CL_SUCCESS)
  {
    res = 1;
    goto end;
  }

  err = dt_opencl_get_device_info(cl, devid, CL_DRIVER_VERSION, (void **)&driverversion, &driverversion_size);
  if(err != CL_SUCCESS)
  {
    res = 1;
    goto end;
  }

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VERSION, (void **)&deviceversion, &deviceversion_size);
  if(err != CL_SUCCESS)
  {
    res = 1;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_TYPE, sizeof(cl_device_type), &type, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE_SUPPORT, sizeof(cl_bool), &image_support, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_HEIGHT, sizeof(size_t),
                                           &(cl->dev[dev].max_image_height), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_WIDTH, sizeof(size_t),
                                           &(cl->dev[dev].max_image_width), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong),
                                           &(cl->dev[dev].max_mem_alloc), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_ENDIAN_LITTLE, sizeof(cl_bool), &little_endian, NULL);


  cname_size = infostr_size;
  cname = malloc(cname_size);
  _ascii_str_canonical(infostr, cname, sizeof(cname_size));

  if(!strncasecmp(vendor, "NVIDIA", 6))
  {
    // very lame attempt to detect support for atomic float add in global memory.
    // we need compute model sm_20, but let's try for all nvidia devices :(
    cl->dev[dev].nvidia_sm_20 = dt_nvidia_gpu_supports_sm_20(infostr);
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] device %d `%s' %s sm_20 support.\n", k, infostr,
             cl->dev[dev].nvidia_sm_20 ? "has" : "doesn't have");
  }

  if(((type & CL_DEVICE_TYPE_CPU) == CL_DEVICE_TYPE_CPU) && !dt_conf_get_bool("opencl_use_cpu_devices"))
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding CPU device %d `%s'.\n", k, infostr);
    res = -1;
    goto end;
  }

  if(dt_opencl_check_driver_blacklist(deviceversion) && !dt_conf_get_bool("opencl_disable_drivers_blacklist"))
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' because the driver `%s' is blacklisted.\n",
             k, infostr, deviceversion);
    res = -1;
    goto end;
  }

  if(!device_available)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' as it is not available.\n", k, infostr);
    res = -1;
    goto end;
  }

  if(!image_support)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' due to missing image support.\n", k,
             infostr);
    res = -1;
    goto end;
  }

  if(!little_endian)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' as it is not little endian.\n", k, infostr);
    res = -1;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong),
                                           &(cl->dev[dev].max_global_mem), NULL);
  if(cl->dev[dev].max_global_mem < opencl_memory_requirement * 1024 * 1024)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] discarding device %d `%s' due to insufficient global memory (%" PRIu64 "MB).\n", k,
             infostr, cl->dev[dev].max_global_mem / 1024 / 1024);
    res = -1;
    goto end;
  }

  cl->dev[dev].vendor = strdup(dt_opencl_get_vendor_by_id(vendor_id));
  cl->dev[dev].name = strdup(infostr);
  cl->dev[dev].cname = strdup(cname);

  cl->crc = crc32(cl->crc, (const unsigned char *)infostr, strlen(infostr));

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] device %d `%s' supports image sizes of %zd x %zd\n", k, infostr,
           cl->dev[dev].max_image_width, cl->dev[dev].max_image_height);
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] device %d `%s' allows GPU memory allocations of up to %" PRIu64 "MB\n",
           k, infostr, cl->dev[dev].max_mem_alloc / 1024 / 1024);

  if(darktable.unmuted & DT_DEBUG_OPENCL)
  {
    printf("[opencl_init] device %d: %s \n", k, infostr);
    printf("     GLOBAL_MEM_SIZE:          %.0fMB\n", (double)cl->dev[dev].max_global_mem / 1024.0 / 1024.0);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(infoint), &infoint, NULL);
    printf("     MAX_WORK_GROUP_SIZE:      %zd\n", infoint);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, sizeof(infoint), &infoint,
                                             NULL);
    printf("     MAX_WORK_ITEM_DIMENSIONS: %zd\n", infoint);
    printf("     MAX_WORK_ITEM_SIZES:      [ ");

    size_t infointtab_size;
    err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, (void **)&infointtab,
                                    &infointtab_size);
    if(err == CL_SUCCESS)
    {
      for(size_t i = 0; i < infoint; i++) printf("%zd ", infointtab[i]);
      free(infointtab);
      infointtab = NULL;
    }
    else
    {
      res = 1;
      goto end;
    }

    printf("]\n");
    printf("     DRIVER_VERSION:           %s\n", driverversion);
    printf("     DEVICE_VERSION:           %s\n", deviceversion);
  }

  dt_pthread_mutex_init(&cl->dev[dev].lock, NULL);

  cl->dev[dev].context = (cl->dlocl->symbols->dt_clCreateContext)(0, 1, &devid, NULL, NULL, &err);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create context for device %d: %d\n", k, err);
    res = 1;
    goto end;
  }
  // create a command queue for first device the context reported
  cl->dev[dev].cmd_queue = (cl->dlocl->symbols->dt_clCreateCommandQueue)(
      cl->dev[dev].context, devid, (darktable.unmuted & DT_DEBUG_PERF) ? CL_QUEUE_PROFILING_ENABLE : 0, &err);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create command queue for device %d: %d\n", k, err);
    res = 1;
    goto end;
  }

  char dtcache[PATH_MAX] = { 0 };
  char cachedir[PATH_MAX] = { 0 };
  char devname[1024];
  double tstart, tend, tdiff;
  dt_loc_get_user_cache_dir(dtcache, sizeof(dtcache));

  int len = strlen(infostr);
  int j = 0;
  // remove non-alphanumeric chars from device name
  for(int i = 0; i < len; i++)
    if(isalnum(infostr[i])) devname[j++] = infostr[i];
  devname[j] = 0;
  snprintf(cachedir, sizeof(cachedir), "%s" G_DIR_SEPARATOR_S "cached_kernels_for_%s", dtcache, devname);
  if(g_mkdir_with_parents(cachedir, 0700) == -1)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] failed to create directory `%s'!\n", cachedir);
    res = 1;
    goto end;
  }

  char dtpath[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  char confentry[PATH_MAX] = { 0 };
  char binname[PATH_MAX] = { 0 };
  dt_loc_get_datadir(dtpath, sizeof(dtpath));
  snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "kernels" G_DIR_SEPARATOR_S "programs.conf", dtpath);
  char kerneldir[PATH_MAX] = { 0 };
  snprintf(kerneldir, sizeof(kerneldir), "%s" G_DIR_SEPARATOR_S "kernels", dtpath);
  char *escapedkerneldir = NULL;
#ifndef __APPLE__
  escapedkerneldir = g_strdup_printf("\"%s\"", kerneldir);
#else
  escapedkerneldir = dt_util_str_replace(kerneldir, " ", "\\ ");
#endif

  options = g_strdup_printf("-cl-fast-relaxed-math %s -D%s=1 -I%s",
                            (cl->dev[dev].nvidia_sm_20 ? " -DNVIDIA_SM_20=1" : ""),
                            dt_opencl_get_vendor_by_id(vendor_id), escapedkerneldir);
  cl->dev[dev].options = strdup(options);

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] options for OpenCL compiler: %s\n", options);

  g_free(options);
  options = NULL;
  g_free(escapedkerneldir);
  escapedkerneldir = NULL;

  const char *clincludes[DT_OPENCL_MAX_INCLUDES] = { "colorspace.cl", "common.h", NULL };
  char *includemd5[DT_OPENCL_MAX_INCLUDES] = { NULL };
  dt_opencl_md5sum(clincludes, includemd5);

  // now load all darktable cl kernels.
  // TODO: compile as a job?
  tstart = dt_get_wtime();
  FILE *f = g_fopen(filename, "rb");
  if(f)
  {

    while(!feof(f))
    {
      int prog = -1;
      gchar *confline_pattern = g_strdup_printf("%%%zu[^\n]\n", sizeof(confentry) - 1);
      int rd = fscanf(f, confline_pattern, confentry);
      g_free(confline_pattern);
      if(rd != 1) continue;
      // remove comments:
      size_t end = strlen(confentry);
      for(size_t pos = 0; pos < end; pos++)
        if(confentry[pos] == '#')
        {
          confentry[pos] = '\0';
          for(int l = pos - 1; l >= 0; l--)
          {
            if(confentry[l] == ' ')
              confentry[l] = '\0';
            else
              break;
          }
          break;
        }
      if(confentry[0] == '\0') continue;

      const char *programname = NULL, *programnumber = NULL;
      gchar **tokens = g_strsplit_set(confentry, " \t", 2);
      if(tokens)
      {
        programname = tokens[0];
        if(tokens[0])
          programnumber = tokens[1]; // if the 0st wasn't NULL then we have at least the terminating NULL in [1]
      }

      prog = programnumber ? strtol(programnumber, NULL, 10) : -1;

      if(!programname || programname[0] == '\0' || prog < 0)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] malformed entry in programs.conf `%s'; ignoring it!\n", confentry);
        continue;
      }

      snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "kernels" G_DIR_SEPARATOR_S "%s", dtpath, programname);
      snprintf(binname, sizeof(binname), "%s" G_DIR_SEPARATOR_S "%s.bin", cachedir, programname);
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] compiling program `%s' ..\n", programname);
      int loaded_cached;
      char md5sum[33];
      if(dt_opencl_load_program(dev, prog, filename, binname, cachedir, md5sum, includemd5, &loaded_cached)
         && dt_opencl_build_program(dev, prog, binname, cachedir, md5sum, loaded_cached) != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] failed to compile program `%s'!\n", programname);
        fclose(f);
        g_strfreev(tokens);
        res = 1;
        goto end;
      }

      g_strfreev(tokens);
    }

    fclose(f);
    tend = dt_get_wtime();
    tdiff = tend - tstart;
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] kernel loading time: %2.4lf \n", tdiff);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not open `%s'!\n", filename);
    res = 1;
    goto end;
  }
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++) g_free(includemd5[n]);

  res = 0;

end:

  free(infostr);
  free(cname);
  free(options);
  free(vendor);
  free(driverversion);
  free(deviceversion);

  return res;
}

void dt_opencl_init(dt_opencl_t *cl, const gboolean exclude_opencl, const gboolean print_statistics)
{
  char *str;
  dt_pthread_mutex_init(&cl->lock, NULL);
  cl->inited = 0;
  cl->enabled = 0;
  cl->stopped = 0;
  cl->error_count = 0;
  cl->print_statistics = print_statistics;

  // work-around to fix a bug in some AMD OpenCL compilers, which would fail parsing certain numerical
  // constants if locale is different from "C".
  // we save the current locale, set locale to "C", and restore the previous setting after OpenCL is
  // initialized
  char *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_ALL, "C");

  int handles = dt_conf_get_int("opencl_number_event_handles");
  handles = (handles < 0 ? 0x7fffffff : handles);
  cl->number_event_handles = handles;
  cl->use_events = (handles != 0);

  cl->avoid_atomics = dt_conf_get_bool("opencl_avoid_atomics");
  cl->async_pixelpipe = dt_conf_get_bool("opencl_async_pixelpipe");
  cl->synch_cache = dt_conf_get_bool("opencl_synch_cache");
  cl->micro_nap = dt_conf_get_int("opencl_micro_nap");
  cl->crc = 5781;
  cl->dlocl = NULL;
  cl->dev_priority_image = NULL;
  cl->dev_priority_preview = NULL;
  cl->dev_priority_export = NULL;
  cl->dev_priority_thumbnail = NULL;

  cl_platform_id *all_platforms = NULL;
  cl_uint *all_num_devices = NULL;

  // user selectable parameter defines minimum requirement on GPU memory
  // default is 768MB
  // values below 200 will be (re)set to 200
  const int opencl_memory_requirement = MAX(200, dt_conf_get_int("opencl_memory_requirement"));
  dt_conf_set_int("opencl_memory_requirement", opencl_memory_requirement);

  if(exclude_opencl)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] do not try to find and use an opencl runtime library due to "
                              "explicit user request\n");
    goto finally;
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl related configuration options:\n");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] \n");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl: %d\n", dt_conf_get_bool("opencl"));
  str = dt_conf_get_string("opencl_library");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_library: '%s'\n", str);
  g_free(str);
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_memory_requirement: %d\n",
           dt_conf_get_int("opencl_memory_requirement"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_memory_headroom: %d\n",
           dt_conf_get_int("opencl_memory_headroom"));
  str = dt_conf_get_string("opencl_device_priority");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_device_priority: '%s'\n", str);
  g_free(str);
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_mandatory_timeout: %d\n",
           dt_conf_get_int("opencl_mandatory_timeout"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_size_roundup: %d\n",
           dt_conf_get_int("opencl_size_roundup"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_async_pixelpipe: %d\n",
           dt_conf_get_bool("opencl_async_pixelpipe"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_synch_cache: %d\n", dt_conf_get_bool("opencl_synch_cache"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_number_event_handles: %d\n",
           dt_conf_get_int("opencl_number_event_handles"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_micro_nap: %d\n", dt_conf_get_int("opencl_micro_nap"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_use_pinned_memory: %d\n",
           dt_conf_get_bool("opencl_use_pinned_memory"));
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_use_cpu_devices: %d\n",
           dt_conf_get_bool("opencl_use_cpu_devices"));

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl_avoid_atomics: %d\n",
           dt_conf_get_bool("opencl_avoid_atomics"));


  dt_print(DT_DEBUG_OPENCL, "[opencl_init] \n");

  // look for explicit definition of opencl_runtime library in preferences
  char *library = dt_conf_get_string("opencl_library");

  // dynamically load opencl runtime
  if((cl->dlocl = dt_dlopencl_init(library)) == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_init] no working opencl library found. Continue with opencl disabled\n");
    g_free(library);
    goto finally;
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl library '%s' found on your system and loaded\n",
             cl->dlocl->library);
  }
  g_free(library);

  cl_int err;
  all_platforms = malloc(DT_OPENCL_MAX_PLATFORMS * sizeof(cl_platform_id));
  all_num_devices = malloc(DT_OPENCL_MAX_PLATFORMS * sizeof(cl_uint));
  cl_uint num_platforms = DT_OPENCL_MAX_PLATFORMS;
  err = (cl->dlocl->symbols->dt_clGetPlatformIDs)(DT_OPENCL_MAX_PLATFORMS, all_platforms, &num_platforms);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get platforms: %d\n", err);
    goto finally;
  }

  if(num_platforms == 0)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] no opencl platform available\n");
    goto finally;
  }
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] found %d platform%s\n", num_platforms,
           num_platforms > 1 ? "s" : "");

  for(int n = 0; n < num_platforms; n++)
  {
    cl_platform_id platform = all_platforms[n];
    // get the number of GPU devices available to the platforms
    // the other common option is CL_DEVICE_TYPE_GPU/CPU (but the latter doesn't work with the nvidia drivers)
    err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, 0, NULL,
                                                  &(all_num_devices[n]));
    if(err != CL_SUCCESS)
    {
      all_num_devices[n] = 0;
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get device id size: %d\n", err);
    }
  }

  cl_uint num_devices = 0;
  for(int n = 0; n < num_platforms; n++) num_devices += all_num_devices[n];

  // create the device list
  cl_device_id *devices = 0;
  if(num_devices)
  {
    cl->dev = (dt_opencl_device_t *)malloc(sizeof(dt_opencl_device_t) * num_devices);
    devices = (cl_device_id *)malloc(sizeof(cl_device_id) * num_devices);
    if(!cl->dev || !devices)
    {
      free(cl->dev);
      free(devices);
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not allocate memory\n");
      goto finally;
    }
  }

  cl_device_id *devs = devices;
  for(int n = 0; n < num_platforms; n++)
  {
    if(all_num_devices[n])
    {
      cl_platform_id platform = all_platforms[n];
      err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, all_num_devices[n], devs,
                                                    NULL);
      if(err != CL_SUCCESS)
      {
        num_devices -= all_num_devices[n];
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get devices list: %d\n", err);
      }
      devs += all_num_devices[n];
    }
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] found %d device%s\n", num_devices, num_devices > 1 ? "s" : "");
  if(num_devices == 0) goto finally;

  int dev = 0;
  for(int k = 0; k < num_devices; k++)
  {
    const int res = dt_opencl_device_init(cl, dev, devices, k, opencl_memory_requirement);

    if(res == 1)
      goto finally;
    else if(res == -1)
      continue;

    // that is, increase dev only if res == 0

    ++dev;
  }
  free(devices);
  if(dev > 0)
  {
    cl->num_devs = dev;
    cl->inited = 1;
    cl->enabled = dt_conf_get_bool("opencl");
    memset(cl->mandatory, 0, sizeof(cl->mandatory));
    cl->dev_priority_image = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_preview = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_export = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_thumbnail = (int *)malloc(sizeof(int) * (dev + 1));

    // only check successful malloc in debug mode; darktable will crash anyhow sooner or later if mallocs that
    // small would fail
    assert(cl->dev_priority_image != NULL && cl->dev_priority_preview != NULL
           && cl->dev_priority_export != NULL && cl->dev_priority_thumbnail != NULL);

    dt_print(DT_DEBUG_OPENCL, "[opencl_init] OpenCL successfully initialized.\n");
    dt_print(
        DT_DEBUG_OPENCL,
        "[opencl_init] here are the internal numbers and names of OpenCL devices available to darktable:\n");
    for(int i = 0; i < dev; i++) dt_print(DT_DEBUG_OPENCL, "[opencl_init]\t\t%d\t'%s'\n", i, cl->dev[i].name);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] no suitable devices found.\n");
  }

finally:
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] FINALLY: opencl is %sAVAILABLE on this system.\n",
           cl->inited ? "" : "NOT ");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] initial status of opencl enabled flag is %s.\n",
           cl->enabled ? "ON" : "OFF");
  if(cl->inited)
  {
    dt_capabilities_add("opencl");
    cl->blendop = dt_develop_blend_init_cl_global();
    cl->bilateral = dt_bilateral_init_cl_global();
    cl->gaussian = dt_gaussian_init_cl_global();
    cl->interpolation = dt_interpolation_init_cl_global();
    cl->local_laplacian = dt_local_laplacian_init_cl_global();
    cl->dwt = dt_dwt_init_cl_global();
    cl->heal = dt_heal_init_cl_global();

    char checksum[64];
    snprintf(checksum, sizeof(checksum), "%u", cl->crc);
    char *oldchecksum = dt_conf_get_string("opencl_checksum");

    // check if the configuration (OpenCL device setup) has changed, indicated by checksum != oldchecksum
    if(strcmp(oldchecksum, checksum) != 0)
    {
      // store new checksum value in config
      dt_conf_set_string("opencl_checksum", checksum);
      // do CPU bencharking
      float tcpu = dt_opencl_benchmark_cpu(1024, 1024, 5, 100.0f);
      // get best benchmarking value of all detected OpenCL devices
      float tgpumin = INFINITY;
      for(int n = 0; n < cl->num_devs; n++)
      {
        float tgpu = cl->dev[n].benchmark = dt_opencl_benchmark_gpu(n, 1024, 1024, 5, 100.0f);
        tgpumin = fmin(tgpu, tgpumin);
      }
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] benchmarking results: %f seconds for fastest GPU versus %f seconds for CPU.\n",
           tgpumin, tcpu);

      if(tcpu <= 1.5f * tgpumin)
      {
        // de-activate opencl for darktable in case of too slow GPU(s). user can always manually overrule this later.
        cl->enabled = FALSE;
        dt_conf_set_bool("opencl", FALSE);
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] due to a slow GPU the opencl flag has been set to OFF.\n");
        dt_control_log(_("due to a slow GPU hardware acceleration via opencl has been de-activated."));
      }
      else if(cl->num_devs >= 2)
      {
        // set scheduling profile to "multiple GPUs" if more than one device has been found
        dt_conf_set_string("opencl_scheduling_profile", "multiple GPUs");
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] set scheduling profile for multiple GPUs.\n");
        dt_control_log(_("multiple GPUs detected - opencl scheduling profile has been set accordingly."));
      }
      else if(tcpu >= 6.0f * tgpumin)
      {
        // set scheduling profile to "very fast GPU" if CPU is way too slow
        dt_conf_set_string("opencl_scheduling_profile", "very fast GPU");
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] set scheduling profile for very fast GPU.\n");
        dt_control_log(_("very fast GPU detected - opencl scheduling profile has been set accordingly."));
      }
      else
      {
        // set scheduling profile to "default"
        dt_conf_set_string("opencl_scheduling_profile", "default");
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] set scheduling profile to default.\n");
        dt_control_log(_("opencl scheduling profile set to default."));
      }
    }
    g_free(oldchecksum);

    // apply config settings for scheduling profile: sets device priorities and pixelpipe synchronization timeout
    dt_opencl_scheduling_profile_t profile = dt_opencl_get_scheduling_profile();
    dt_opencl_apply_scheduling_profile(profile);
  }
  else // initialization failed
  {
    for(int i = 0; cl->dev && i < cl->num_devs; i++)
    {
      dt_pthread_mutex_destroy(&cl->dev[i].lock);
      for(int k = 0; k < DT_OPENCL_MAX_KERNELS; k++)
        if(cl->dev[i].kernel_used[k]) (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[i].kernel[k]);
      for(int k = 0; k < DT_OPENCL_MAX_PROGRAMS; k++)
        if(cl->dev[i].program_used[k]) (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
      (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
      (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);
      if(cl->use_events)
      {
        dt_opencl_events_reset(i);
        free(cl->dev[i].eventlist);
        free(cl->dev[i].eventtags);
      }
      free((void *)(cl->dev[i].vendor));
      free((void *)(cl->dev[i].name));
      free((void *)(cl->dev[i].cname));
      free((void *)(cl->dev[i].options));
    }
  }

  free(all_num_devices);
  free(all_platforms);

  if(locale)
  {
    setlocale(LC_ALL, locale);
    free(locale);
  }
  return;
}

void dt_opencl_cleanup(dt_opencl_t *cl)
{
  if(cl->inited)
  {
    dt_develop_blend_free_cl_global(cl->blendop);
    dt_bilateral_free_cl_global(cl->bilateral);
    dt_gaussian_free_cl_global(cl->gaussian);
    dt_interpolation_free_cl_global(cl->interpolation);
    dt_dwt_free_cl_global(cl->dwt);
    dt_heal_free_cl_global(cl->heal);

    for(int i = 0; i < cl->num_devs; i++)
    {
      dt_pthread_mutex_destroy(&cl->dev[i].lock);
      for(int k = 0; k < DT_OPENCL_MAX_KERNELS; k++)
        if(cl->dev[i].kernel_used[k]) (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[i].kernel[k]);
      for(int k = 0; k < DT_OPENCL_MAX_PROGRAMS; k++)
        if(cl->dev[i].program_used[k]) (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
      (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
      (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);

      if(cl->print_statistics && (darktable.unmuted & DT_DEBUG_MEMORY))
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_summary_statistics] device '%s' (%d): peak memory usage %zu bytes (%.1f MB)\n",
                   cl->dev[i].name, i, cl->dev[i].peak_memory, (float)cl->dev[i].peak_memory/(1024*1024));
      }

      if(cl->print_statistics && cl->use_events)
      {
        if(cl->dev[i].totalevents)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_summary_statistics] device '%s' (%d): %d out of %d events were "
                                    "successful and %d events lost\n",
                   cl->dev[i].name, i, cl->dev[i].totalsuccess, cl->dev[i].totalevents, cl->dev[i].totallost);
        }
        else
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_summary_statistics] device '%s' (%d): NOT utilized\n",
                   cl->dev[i].name, i);
        }
      }

      if(cl->use_events)
      {
        dt_opencl_events_reset(i);

        free(cl->dev[i].eventlist);
        free(cl->dev[i].eventtags);
      }

      free((void *)(cl->dev[i].vendor));
      free((void *)(cl->dev[i].name));
      free((void *)(cl->dev[i].cname));
      free((void *)(cl->dev[i].options));
    }
    free(cl->dev_priority_image);
    free(cl->dev_priority_preview);
    free(cl->dev_priority_export);
    free(cl->dev_priority_thumbnail);
  }

  if(cl->dlocl)
  {
    free(cl->dlocl->symbols);
    g_free(cl->dlocl->library);
    free(cl->dlocl);
  }

  free(cl->dev);
  dt_pthread_mutex_destroy(&cl->lock);
}

static const char *dt_opencl_get_vendor_by_id(unsigned int id)
{
  const char *vendor;

  switch(id)
  {
    case 4098:
      vendor = "AMD";
      break;
    case 4318:
      vendor = "NVIDIA";
      break;
    case 0x8086u:
      vendor = "INTEL";
      break;
    default:
      vendor = "UNKNOWN";
  }

  return vendor;
}

#define TEA_ROUNDS 8
static void encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = { 0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e };
  unsigned int v0 = arg[0], v1 = arg[1];
  unsigned int sum = 0;
  unsigned int delta = 0x9e3779b9;
  for(int i = 0; i < TEA_ROUNDS; i++)
  {
    sum += delta;
    v0 += ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    v1 += ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
  }
  arg[0] = v0;
  arg[1] = v1;
}

static float tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / 0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrtf(2.0f * frandom) - 1.0f) : (1.0f - sqrtf(2.0f * (1.0f - frandom))));
}

static float dt_opencl_benchmark_gpu(const int devid, const size_t width, const size_t height, const int count, const float sigma)
{
  const int bpp = 4 * sizeof(float);
  cl_int err = 666;
  cl_mem dev_mem = NULL;
  float *buf = NULL;
  dt_gaussian_cl_t *g = NULL;

  const float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
  const float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  unsigned int *const tea_states = calloc(2 * dt_get_num_threads(), sizeof(unsigned int));

  buf = dt_alloc_align(16, width * height * bpp);
  if(buf == NULL) goto error;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buf)
#endif
  for(size_t j = 0; j < height; j++)
  {
    unsigned int *tea_state = tea_states + 2 * dt_get_thread_num();
    tea_state[0] = j + dt_get_thread_num();
    size_t index = j * 4 * width;
    for(int i = 0; i < 4 * width; i++)
    {
      encrypt_tea(tea_state);
      buf[index + i] = 100.0f * tpdf(tea_state[0]);
    }
  }

  // start timer
  double start = dt_get_wtime();

  // allocate dev_mem buffer
  dev_mem = dt_opencl_alloc_device_use_host_pointer(devid, width, height, bpp, width*bpp, buf);
  if(dev_mem == NULL) goto error;

  // prepare gaussian filter
  g = dt_gaussian_init_cl(devid, width, height, 4, Labmax, Labmin, sigma, 0);
  if(!g) goto error;

  // gaussian blur
  for(int n = 0; n < count; n++)
  {
    err = dt_gaussian_blur_cl(g, dev_mem, dev_mem);
    if(err != CL_SUCCESS) goto error;
  }

  // cleanup gaussian filter
  dt_gaussian_free_cl(g);
  g = NULL;

  // copy dev_mem -> buf
  err = dt_opencl_copy_device_to_host(devid, buf, dev_mem, width, height, bpp);
  if(err != CL_SUCCESS) goto error;

  // free dev_mem
  dt_opencl_release_mem_object(dev_mem);

  // end timer
  double end = dt_get_wtime();

  dt_free_align(buf);
  free(tea_states);
  return (end - start);

error:
  dt_gaussian_free_cl(g);
  dt_free_align(buf);
  free(tea_states);
  dt_opencl_release_mem_object(dev_mem);
  return INFINITY;
}

static float dt_opencl_benchmark_cpu(const size_t width, const size_t height, const int count, const float sigma)
{
  const int bpp = 4 * sizeof(float);
  float *buf = NULL;
  dt_gaussian_t *g = NULL;

  const float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
  const float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  unsigned int *const tea_states = calloc(2 * dt_get_num_threads(), sizeof(unsigned int));

  buf = dt_alloc_align(16, width * height * bpp);
  if(buf == NULL) goto error;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buf)
#endif
  for(size_t j = 0; j < height; j++)
  {
    unsigned int *tea_state = tea_states + 2 * dt_get_thread_num();
    tea_state[0] = j + dt_get_thread_num();
    size_t index = j * 4 * width;
    for(int i = 0; i < 4 * width; i++)
    {
      encrypt_tea(tea_state);
      buf[index + i] = 100.0f * tpdf(tea_state[0]);
    }
  }

  // start timer
  double start = dt_get_wtime();

  // prepare gaussian filter
  g = dt_gaussian_init(width, height, 4, Labmax, Labmin, sigma, 0);
  if(!g) goto error;

  // gaussian blur
  for(int n = 0; n < count; n++)
  {
    dt_gaussian_blur(g, buf, buf);
  }

  // cleanup gaussian filter
  dt_gaussian_free(g);
  g = NULL;

  // end timer
  double end = dt_get_wtime();

  dt_free_align(buf);
  free(tea_states);
  return (end - start);

error:
  dt_gaussian_free(g);
  dt_free_align(buf);
  free(tea_states);
  return INFINITY;
}


int dt_opencl_finish(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return -1;

  cl_int err = (cl->dlocl->symbols->dt_clFinish)(cl->dev[devid].cmd_queue);

  // take the opportunity to release some event handles, but without printing
  // summary statistics
  cl_int success = dt_opencl_events_flush(devid, 0);

  return (err == CL_SUCCESS && success == CL_COMPLETE);
}

int dt_opencl_enqueue_barrier(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return -1;
  return (cl->dlocl->symbols->dt_clEnqueueBarrier)(cl->dev[devid].cmd_queue);
}

static int _take_from_list(int *list, int value)
{
  int result = -1;

  while(*list != -1 && *list != value) list++;
  result = *list;

  while(*list != -1)
  {
    *list = *(list + 1);
    list++;
  }

  return result;
}


static int _device_by_cname(const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  int devs = cl->num_devs;
  char tmp[2048] = { 0 };
  int result = -1;

  _ascii_str_canonical(name, tmp, sizeof(tmp));

  for(int i = 0; i < devs; i++)
  {
    if(!strcmp(tmp, cl->dev[i].cname))
    {
      result = i;
      break;
    }
  }

  return result;
}


static char *_ascii_str_canonical(const char *in, char *out, int maxlen)
{
  if(out == NULL)
  {
    maxlen = strlen(in) + 1;
    out = malloc(maxlen);
    if(out == NULL) return NULL;
  }

  int len = 0;

  while(*in != '\0' && len < maxlen - 1)
  {
    int n = strcspn(in, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    in += n;
    if(n != 0) continue;
    out[len] = tolower(*in);
    len++;
    in++;
  }
  out[len] = '\0';

  return out;
}


static char *_strsep(char **stringp, const char *delim)
{
  char *begin, *end;

  begin = *stringp;
  if(begin == NULL) return NULL;

  if(delim[0] == '\0' || delim[1] == '\0')
  {
    char ch = delim[0];

    if(ch == '\0')
      end = NULL;
    else
    {
      if(*begin == ch)
        end = begin;
      else if(*begin == '\0')
        end = NULL;
      else
        end = strchr(begin + 1, ch);
    }
  }
  else
    end = strpbrk(begin, delim);

  if(end)
  {
    *end++ = '\0';
    *stringp = end;
  }
  else
    *stringp = NULL;

  return begin;
}


// parse a single token of priority string and store priorities in priority_list
static void dt_opencl_priority_parse(dt_opencl_t *cl, char *configstr, int *priority_list, int *mandatory)
{
  int devs = cl->num_devs;
  int count = 0;
  int *full = malloc((size_t)(devs + 1) * sizeof(int));
  int mnd = 0;

  // NULL or empty configstring?
  if(configstr == NULL || *configstr == '\0')
  {
    priority_list[0] = -1;
    *mandatory = 0;
    free(full);
    return;
  }

  // check if user wants us to force-use opencl device(s)
  if(configstr[0] == '+')
  {
    mnd = 1;
    configstr++;
  }

  // first start with a full list of devices to take from
  for(int i = 0; i < devs; i++) full[i] = i;
  full[devs] = -1;

  gchar **tokens = g_strsplit(configstr, ",", 0);
  gchar **tokens_ptr = tokens;

  while(tokens != NULL && *tokens_ptr != NULL && count < devs + 1 && full[0] != -1)
  {
    gchar *str = *tokens_ptr;
    int not = 0;
    int all = 0;

    switch(*str)
    {
      case '*':
        all = 1;
        break;
      case '!':
        not = 1;
        while(*str == '!') str++;
        break;
    }

    if(all)
    {
      // copy all remaining device numbers from full to priority list
      for(int i = 0; i < devs && full[i] != -1; i++)
      {
        priority_list[count] = full[i];
        count++;
      }
      full[0] = -1; // mark full list as empty
    }
    else if(*str != '\0')
    {
      char *endptr = NULL;

      // first check if str corresponds to an existing canonical device name
      long number = _device_by_cname(str);

      // if not try to convert string into decimal device number
      if(number < 0) number = strtol(str, &endptr, 10);

      // still not found or negative number given? set number to -1
      if(number < 0 || (number == 0 && endptr == str)) number = -1;

      // try to take number out of remaining device list
      int dev_number = _take_from_list(full, number);

      if(!not&&dev_number != -1)
      {
        priority_list[count] = dev_number;
        count++;
      }
    }

    tokens_ptr++;
  }

  g_strfreev(tokens);

  // terminate priority list with -1
  while(count < devs + 1) priority_list[count++] = -1;

  // opencl use can only be mandatory if at least one opencl device is given
  *mandatory = (priority_list[0] != -1) ? mnd : 0;

  free(full);
}

// parse a complete priority string
static void dt_opencl_priorities_parse(dt_opencl_t *cl, const char *configstr)
{
  char tmp[2048];
  int len = 0;

  // first get rid of all invalid characters
  while(*configstr != '\0' && len < sizeof(tmp) - 1)
  {
    int n = strcspn(configstr, "/!,*+0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    configstr += n;
    if(n != 0) continue;
    tmp[len] = *configstr;
    len++;
    configstr++;
  }
  tmp[len] = '\0';

  char *str = tmp;

  // now split config string into tokens, separated by '/' and parse them one after the other
  char *prio = _strsep(&str, "/");
  dt_opencl_priority_parse(cl, prio, cl->dev_priority_image, &cl->mandatory[0]);

  prio = _strsep(&str, "/");
  dt_opencl_priority_parse(cl, prio, cl->dev_priority_preview, &cl->mandatory[1]);

  prio = _strsep(&str, "/");
  dt_opencl_priority_parse(cl, prio, cl->dev_priority_export, &cl->mandatory[2]);

  prio = _strsep(&str, "/");
  dt_opencl_priority_parse(cl, prio, cl->dev_priority_thumbnail, &cl->mandatory[3]);
}

// set device priorities according to config string
static void dt_opencl_update_priorities(const char *configstr)
{
  dt_opencl_t *cl = darktable.opencl;
  dt_opencl_priorities_parse(cl, configstr);

  dt_print(DT_DEBUG_OPENCL, "[opencl_priorities] these are your device priorities:\n");
  dt_print(DT_DEBUG_OPENCL, "[opencl_priorities] \t\timage\tpreview\texport\tthumbnail\n");
  for(int i = 0; i < cl->num_devs; i++)
    dt_print(DT_DEBUG_OPENCL, "[opencl_priorities]\t\t%d\t%d\t%d\t%d\n", cl->dev_priority_image[i],
             cl->dev_priority_preview[i], cl->dev_priority_export[i], cl->dev_priority_thumbnail[i]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_priorities] show if opencl use is mandatory for a given pixelpipe:\n");
  dt_print(DT_DEBUG_OPENCL, "[opencl_priorities] \t\timage\tpreview\texport\tthumbnail\n");
  dt_print(DT_DEBUG_OPENCL, "[opencl_priorities]\t\t%d\t%d\t%d\t%d\n", cl->mandatory[0],
             cl->mandatory[1], cl->mandatory[2], cl->mandatory[3]);
}

int dt_opencl_lock_device(const int pipetype)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;


  dt_pthread_mutex_lock(&cl->lock);

  size_t prio_size = sizeof(int) * (cl->num_devs + 1);
  int *priority = (int *)malloc(prio_size);
  int mandatory;

  switch(pipetype)
  {
    case DT_DEV_PIXELPIPE_FULL:
      memcpy(priority, cl->dev_priority_image, prio_size);
      mandatory = cl->mandatory[0];
      break;
    case DT_DEV_PIXELPIPE_PREVIEW:
      memcpy(priority, cl->dev_priority_preview, prio_size);
      mandatory = cl->mandatory[1];
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      memcpy(priority, cl->dev_priority_export, prio_size);
      mandatory = cl->mandatory[2];
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      memcpy(priority, cl->dev_priority_thumbnail, prio_size);
      mandatory = cl->mandatory[3];
      break;
    default:
      free(priority);
      priority = NULL;
      mandatory = 0;
  }

  dt_pthread_mutex_unlock(&cl->lock);

  if(priority)
  {
    const int usec = 5000;
    const int nloop = MAX(0, dt_conf_get_int("opencl_mandatory_timeout"));

    // check for free opencl device repeatedly if mandatory is TRUE, else give up after first try
    for(int n = 0; n < nloop; n++)
    {
      const int *prio = priority;

      while(*prio != -1)
      {
        if(!dt_pthread_mutex_BAD_trylock(&cl->dev[*prio].lock))
        {
          int devid = *prio;
          free(priority);
          return devid;
        }
        prio++;
      }

      if(!mandatory)
      {
        free(priority);
        return -1;
      }

      dt_iop_nap(usec);
    }
  }
  else
  {
    // only a fallback if a new pipe type would be added and we forget to take care of it in opencl.c
    for(int try_dev = 0; try_dev < cl->num_devs; try_dev++)
    {
      // get first currently unused processor
      if(!dt_pthread_mutex_BAD_trylock(&cl->dev[try_dev].lock)) return try_dev;
    }
  }

  free(priority);

  // no free GPU :(
  // use CPU processing, if no free device:
  return -1;
}

void dt_opencl_unlock_device(const int dev)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(dev < 0 || dev >= cl->num_devs) return;
  dt_pthread_mutex_BAD_unlock(&cl->dev[dev].lock);
}

static FILE *fopen_stat(const char *filename, struct stat *st)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_fopen_stat] could not open file `%s'!\n", filename);
    return NULL;
  }
  int fd = fileno(f);
  if(fstat(fd, st) < 0)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_fopen_stat] could not stat file `%s'!\n", filename);
    return NULL;
  }
  return f;
}


void dt_opencl_md5sum(const char **files, char **md5sums)
{
  char dtpath[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_datadir(dtpath, sizeof(dtpath));

  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++, files++, md5sums++)
  {
    if(!*files)
    {
      *md5sums = NULL;
      continue;
    }

    snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "kernels" G_DIR_SEPARATOR_S "%s", dtpath, *files);

    struct stat filestat;
    FILE *f = fopen_stat(filename, &filestat);

    if(!f)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not open file `%s'!\n", filename);
      *md5sums = NULL;
      continue;
    }

    size_t filesize = filestat.st_size;
    char *file = (char *)malloc(filesize);

    if(!file)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not allocate buffer for file `%s'!\n", filename);
      *md5sums = NULL;
      fclose(f);
      continue;
    }

    size_t rd = fread(file, sizeof(char), filesize, f);
    fclose(f);

    if(rd != filesize)
    {
      free(file);
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not read all of file `%s'!\n", filename);
      *md5sums = NULL;
      continue;
    }

    *md5sums = g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, filesize);

    free(file);
  }
}


int dt_opencl_load_program(const int dev, const int prog, const char *filename, const char *binname,
                           const char *cachedir, char *md5sum, char **includemd5, int *loaded_cached)
{
  cl_int err;
  dt_opencl_t *cl = darktable.opencl;

  struct stat filestat, cachedstat;
  *loaded_cached = 0;

  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] invalid program number `%d' of file `%s'!\n", prog,
             filename);
    return 0;
  }

  if(cl->dev[dev].program_used[prog])
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_source] program number `%d' already in use when loading file `%s'!\n", prog,
             filename);
    return 0;
  }

  FILE *f = fopen_stat(filename, &filestat);
  if(!f) return 0;

  size_t filesize = filestat.st_size;
  char *file = (char *)malloc(filesize + 2048);
  size_t rd = fread(file, sizeof(char), filesize, f);
  fclose(f);
  if(rd != filesize)
  {
    free(file);
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] could not read all of file `%s'!\n", filename);
    return 0;
  }

  char *start = file + filesize;
  char *end = start + 2048;
  size_t len;

  cl_device_id devid = cl->dev[dev].devid;
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DRIVER_VERSION, end - start, start, &len);
  start += len;

  cl_platform_id platform;
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform, NULL);

  (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform, CL_PLATFORM_VERSION, end - start, start, &len);
  start += len;

  len = snprintf(start, end - start, "%s", cl->dev[dev].options);
  start += len;

  /* make sure that the md5sums of all the includes are applied as well */
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++)
  {
    if(!includemd5[n]) continue;
    len = snprintf(start, end - start, "%s", includemd5[n]);
    start += len;
  }

  char *source_md5 = g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, start - file);
  strncpy(md5sum, source_md5, 33);
  g_free(source_md5);

  file[filesize] = '\0';

  char linkedfile[PATH_MAX] = { 0 };
  ssize_t linkedfile_len = 0;

#if defined(_WIN32)
  // No symlinks on Windows
  // Have to figure out the name using the filename + md5sum
  char dup[PATH_MAX] = { 0 };
  snprintf(dup, sizeof(dup), "%s.%s", binname, md5sum);
  FILE *cached = fopen_stat(dup, &cachedstat);
  g_strlcpy(linkedfile, md5sum, sizeof(linkedfile));
  linkedfile_len = strlen(md5sum);
#else
  FILE *cached = fopen_stat(binname, &cachedstat);
#endif

  if(cached)
  {
#if !defined(_WIN32)
    linkedfile_len = readlink(binname, linkedfile, sizeof(linkedfile) - 1);
#endif // !defined(_WIN32)
    if(linkedfile_len > 0)
    {
      linkedfile[linkedfile_len] = '\0';

      if(strncmp(linkedfile, md5sum, 33) == 0)
      {
        // md5sum matches, load cached binary
        size_t cached_filesize = cachedstat.st_size;

        unsigned char *cached_content = (unsigned char *)malloc(cached_filesize + 1);
        rd = fread(cached_content, sizeof(char), cached_filesize, cached);
        if(rd != cached_filesize)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not read all of file `%s'!\n", binname);
        }
        else
        {
          cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithBinary)(
              cl->dev[dev].context, 1, &(cl->dev[dev].devid), &cached_filesize,
              (const unsigned char **)&cached_content, NULL, &err);
          if(err != CL_SUCCESS)
          {
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_load_program] could not load cached binary program from file `%s'! (%d)\n",
                     binname, err);
          }
          else
          {
            cl->dev[dev].program_used[prog] = 1;
            *loaded_cached = 1;
          }
        }
        free(cached_content);
      }
    }
    fclose(cached);
  }


  if(*loaded_cached == 0)
  {
    // if loading cached was unsuccessful for whatever reason,
    // try to remove cached binary & link
    if(linkedfile_len > 0)
    {
      char link_dest[PATH_MAX] = { 0 };
      snprintf(link_dest, sizeof(link_dest), "%s" G_DIR_SEPARATOR_S "%s", cachedir, linkedfile);
      g_unlink(link_dest);
    }
    g_unlink(binname);

    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_program] could not load cached binary program, trying to compile source\n");

    cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithSource)(
        cl->dev[dev].context, 1, (const char **)&file, &filesize, &err);
    free(file);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] could not create program from file `%s'! (%d)\n",
               filename, err);
      return 0;
    }
    else
    {
      cl->dev[dev].program_used[prog] = 1;
    }
  }
  else
  {
    free(file);
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] loaded cached binary program from file `%s'\n", binname);
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] successfully loaded program from `%s'\n", filename);

  return 1;
}

int dt_opencl_build_program(const int dev, const int prog, const char *binname, const char *cachedir,
                            char *md5sum, int loaded_cached)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_opencl_t *cl = darktable.opencl;
  cl_program program = cl->dev[dev].program[prog];
  cl_int err;
  err = (cl->dlocl->symbols->dt_clBuildProgram)(program, 1, &(cl->dev[dev].devid), cl->dev[dev].options, 0, 0);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] could not build program: %d\n", err);
  else
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] successfully built program\n");

  cl_build_status build_status;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_STATUS,
                                                 sizeof(cl_build_status), &build_status, NULL);
  dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] BUILD STATUS: %d\n", build_status);

  char *build_log;
  size_t ret_val_size;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG, 0, NULL,
                                                 &ret_val_size);
  if(ret_val_size != SIZE_MAX)
  {
    build_log = (char *)malloc(sizeof(char) * (ret_val_size + 1));
    if(build_log)
    {
      (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG,
                                                     ret_val_size, build_log, NULL);

      build_log[ret_val_size] = '\0';

      dt_print(DT_DEBUG_OPENCL, "BUILD LOG:\n");
      dt_print(DT_DEBUG_OPENCL, "%s\n", build_log);

      free(build_log);
    }
  }

  if(err != CL_SUCCESS)
    return err;
  else
  {
    if(!loaded_cached)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] saving binary\n");

      cl_uint numdev = 0;
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_NUM_DEVICES, sizeof(cl_uint),
                                                      &numdev, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_NUM_DEVICES failed: %d\n", err);
        return CL_SUCCESS;
      }

      cl_device_id *devices = malloc(numdev * sizeof(cl_device_id));
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_DEVICES,
                                                      sizeof(cl_device_id) * numdev, devices, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_DEVICES failed: %d\n", err);
        free(devices);
        return CL_SUCCESS;
      }

      size_t *binary_sizes = malloc(numdev * sizeof(size_t));
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARY_SIZES,
                                                      sizeof(size_t) * numdev, binary_sizes, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_BINARY_SIZES failed: %d\n", err);
        free(binary_sizes);
        free(devices);
        return CL_SUCCESS;
      }

      unsigned char **binaries = malloc(numdev * sizeof(unsigned char *));
      for(int i = 0; i < numdev; i++) binaries[i] = (unsigned char *)malloc(binary_sizes[i]);
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARIES,
                                                      sizeof(unsigned char *) * numdev, binaries, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_BINARIES failed: %d\n", err);
        goto ret;
      }

      for(int i = 0; i < numdev; i++)
        if(cl->dev[dev].devid == devices[i])
        {
          // save opencl compiled binary as md5sum-named file
          char link_dest[PATH_MAX] = { 0 };
          snprintf(link_dest, sizeof(link_dest), "%s" G_DIR_SEPARATOR_S "%s", cachedir, md5sum);
          FILE *f = g_fopen(link_dest, "w");
          if(!f) goto ret;
          size_t bytes_written = fwrite(binaries[i], sizeof(char), binary_sizes[i], f);
          if(bytes_written != binary_sizes[i]) goto ret;
          fclose(f);

          // create link (e.g. basic.cl.bin -> f1430102c53867c162bb60af6c163328)
          char cwd[PATH_MAX] = { 0 };
          if(!getcwd(cwd, sizeof(cwd))) goto ret;
          if(chdir(cachedir) != 0) goto ret;
          char dup[PATH_MAX] = { 0 };
          g_strlcpy(dup, binname, sizeof(dup));
          char *bname = basename(dup);
#if defined(_WIN32)
          //CreateSymbolicLink in Windows requires admin privileges, which we don't want/need
          //store has using a simple filerename
          char finalfilename[PATH_MAX] = { 0 };
          snprintf(finalfilename, sizeof(finalfilename), "%s" G_DIR_SEPARATOR_S "%s.%s", cachedir, bname, md5sum);
          rename(link_dest, finalfilename);
#else
          if(symlink(md5sum, bname) != 0) goto ret;
#endif //!defined(_WIN32)
          if(chdir(cwd) != 0) goto ret;
        }

    ret:
      for(int i = 0; i < numdev; i++) free(binaries[i]);
      free(binaries);
      free(binary_sizes);
      free(devices);
    }
    return CL_SUCCESS;
  }
}

int dt_opencl_create_kernel(const int prog, const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_pthread_mutex_lock(&cl->lock);
  int k = 0;
  for(int dev = 0; dev < cl->num_devs; dev++)
  {
    cl_int err;
    for(; k < DT_OPENCL_MAX_KERNELS; k++)
      if(!cl->dev[dev].kernel_used[k])
      {
        cl->dev[dev].kernel_used[k] = 1;
        cl->dev[dev].kernel[k]
            = (cl->dlocl->symbols->dt_clCreateKernel)(cl->dev[dev].program[prog], name, &err);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] could not create kernel `%s'! (%d)\n", name, err);
          cl->dev[dev].kernel_used[k] = 0;
          goto error;
        }
        else
          break;
      }
    if(k < DT_OPENCL_MAX_KERNELS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] successfully loaded kernel `%s' (%d) for device %d\n",
               name, k, dev);
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] too many kernels! can't create kernel `%s'\n", name);
      goto error;
    }
  }
  dt_pthread_mutex_unlock(&cl->lock);
  return k;
error:
  dt_pthread_mutex_unlock(&cl->lock);
  return -1;
}

void dt_opencl_free_kernel(const int kernel)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return;
  dt_pthread_mutex_lock(&cl->lock);
  for(int dev = 0; dev < cl->num_devs; dev++)
  {
    cl->dev[dev].kernel_used[kernel] = 0;
    (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[dev].kernel[kernel]);
  }
  dt_pthread_mutex_unlock(&cl->lock);
}

int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  return (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                                                  sizeof(size_t) * 3, sizes, NULL);
}

int dt_opencl_get_work_group_limits(const int dev, size_t *sizes, size_t *workgroupsize,
                                    unsigned long *localmemsize)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  cl_ulong lmemsize;
  cl_int err;

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_LOCAL_MEM_SIZE,
                                                 sizeof(cl_ulong), &lmemsize, NULL);
  if(err != CL_SUCCESS) return err;

  *localmemsize = lmemsize;

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_GROUP_SIZE,
                                                 sizeof(size_t), workgroupsize, NULL);
  if(err != CL_SUCCESS) return err;

  return dt_opencl_get_max_work_item_sizes(dev, sizes);
}


int dt_opencl_get_kernel_work_group_size(const int dev, const int kernel, size_t *kernelworkgroupsize)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;

  return (cl->dlocl->symbols->dt_clGetKernelWorkGroupInfo)(cl->dev[dev].kernel[kernel], cl->dev[dev].devid,
                                                           CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t),
                                                           kernelworkgroupsize, NULL);
}


int dt_opencl_set_kernel_arg(const int dev, const int kernel, const int num, const size_t size,
                             const void *arg)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  return (cl->dlocl->symbols->dt_clSetKernelArg)(cl->dev[dev].kernel[kernel], num, size, arg);
}

int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes)
{
  return dt_opencl_enqueue_kernel_2d_with_local(dev, kernel, sizes, NULL);
}


int dt_opencl_enqueue_kernel_2d_with_local(const int dev, const int kernel, const size_t *sizes,
                                           const size_t *local)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  int err;
  char buf[256];
  buf[0] = '\0';
  (cl->dlocl->symbols->dt_clGetKernelInfo)(cl->dev[dev].kernel[kernel], CL_KERNEL_FUNCTION_NAME, 256, buf,
                                           NULL);
  cl_event *eventp = dt_opencl_events_get_slot(dev, buf);
  err = (cl->dlocl->symbols->dt_clEnqueueNDRangeKernel)(cl->dev[dev].cmd_queue, cl->dev[dev].kernel[kernel],
                                                        2, NULL, sizes, local, 0, NULL, eventp);
  // if (err == CL_SUCCESS) err = dt_opencl_finish(dev);
  return err;
}

int dt_opencl_copy_device_to_host(const int devid, void *host, void *device, const int width,
                                  const int height, const int bpp)
{
  return dt_opencl_read_host_from_device(devid, host, device, width, height, bpp);
}

int dt_opencl_read_host_from_device(const int devid, void *host, void *device, const int width,
                                    const int height, const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch(devid, host, device, width, height, bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch(const int devid, void *host, void *device, const int width,
                                             const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_read_host_from_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                 const int height, const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch_non_blocking(devid, host, device, width, height,
                                                               bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                          const int width, const int height,
                                                          const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin, region, rowpitch, CL_FALSE);
}


int dt_opencl_read_host_from_device_raw(const int devid, void *host, void *device, const size_t *origin,
                                        const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Read Image (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadImage)(darktable.opencl->dev[devid].cmd_queue,
                                                                   device, blocking, origin, region, rowpitch,
                                                                   0, host, 0, NULL, eventp);
}

int dt_opencl_write_host_to_device(const int devid, void *host, void *device, const int width,
                                   const int height, const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch(devid, host, device, width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch(const int devid, void *host, void *device, const int width,
                                            const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_write_host_to_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                const int height, const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch_non_blocking(devid, host, device, width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                         const int width, const int height,
                                                         const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin, region, rowpitch, CL_FALSE);
}

int dt_opencl_write_host_to_device_raw(const int devid, void *host, void *device, const size_t *origin,
                                       const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Write Image (from host to device)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteImage)(darktable.opencl->dev[devid].cmd_queue,
                                                                    device, blocking, origin, region,
                                                                    rowpitch, 0, host, 0, NULL, eventp);
}

int dt_opencl_enqueue_copy_image(const int devid, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst,
                                 size_t *region)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImage)(
      darktable.opencl->dev[devid].cmd_queue, src, dst, orig_src, orig_dst, region, 0, NULL, eventp);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_image] could not copy image: %d\n", err);
  return err;
}

int dt_opencl_enqueue_copy_image_to_buffer(const int devid, cl_mem src_image, cl_mem dst_buffer,
                                           size_t *origin, size_t *region, size_t offset)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image to Buffer (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImageToBuffer)(
      darktable.opencl->dev[devid].cmd_queue, src_image, dst_buffer, origin, region, offset, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_image_to_buffer] could not copy image: %d\n", err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_image(const int devid, cl_mem src_buffer, cl_mem dst_image,
                                           size_t offset, size_t *origin, size_t *region)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Buffer to Image (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBufferToImage)(
      darktable.opencl->dev[devid].cmd_queue, src_buffer, dst_image, offset, origin, region, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_buffer_to_image] could not copy buffer: %d\n", err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_buffer(const int devid, cl_mem src_buffer, cl_mem dst_buffer,
                                            size_t srcoffset, size_t dstoffset, size_t size)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Buffer to Buffer (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBuffer)(darktable.opencl->dev[devid].cmd_queue,
                                                                   src_buffer, dst_buffer, srcoffset,
                                                                   dstoffset, size, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_buffer_to_buffer] could not copy buffer: %d\n", err);
  return err;
}

int dt_opencl_read_buffer_from_device(const int devid, void *host, void *device, const size_t offset,
                                      const size_t size, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Read Buffer (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadBuffer)(
      darktable.opencl->dev[devid].cmd_queue, device, blocking, offset, size, host, 0, NULL, eventp);
}

int dt_opencl_write_buffer_to_device(const int devid, void *host, void *device, const size_t offset,
                                     const size_t size, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Write Buffer (from host to device)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteBuffer)(
      darktable.opencl->dev[devid].cmd_queue, device, blocking, offset, size, host, 0, NULL, eventp);
}


void *dt_opencl_copy_host_to_device_constant(const int devid, const size_t size, void *host)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)(
      darktable.opencl->dev[devid].context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, size, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device_constant] could not alloc buffer on device %d: %d\n", devid, err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}

void *dt_opencl_copy_host_to_device(const int devid, void *host, const int width, const int height,
                                    const int bpp)
{
  return dt_opencl_copy_host_to_device_rowpitch(devid, host, width, height, bpp, 0);
}

void *dt_opencl_copy_host_to_device_rowpitch(const int devid, void *host, const int width, const int height,
                                             const int bpp, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  // TODO: if fmt = uint16_t, blow up to 4xuint16_t and copy manually!
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
      darktable.opencl->dev[devid].context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &fmt, width, height,
      rowpitch, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device] could not alloc/copy img buffer on device %d: %d\n", devid, err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void dt_opencl_release_mem_object(cl_mem mem)
{
  if(!darktable.opencl->inited) return;

  // the OpenCL specs are not absolutely clear if clReleaseMemObject(NULL) is a no-op. we take care of the
  // case in a centralized way at this place
  if(mem == NULL) return;

  dt_opencl_memory_statistics(-1, mem, OPENCL_MEMORY_SUB);

  (darktable.opencl->dlocl->symbols->dt_clReleaseMemObject)(mem);
}

void *dt_opencl_map_buffer(const int devid, cl_mem buffer, const int blocking, const int flags, size_t offset,
                           size_t size)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;
  void *ptr;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Map Buffer]");
  ptr = (darktable.opencl->dlocl->symbols->dt_clEnqueueMapBuffer)(
      darktable.opencl->dev[devid].cmd_queue, buffer, blocking, flags, offset, size, 0, NULL, eventp, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl map buffer] could not map buffer: %d\n", err);
  return ptr;
}

int dt_opencl_unmap_mem_object(const int devid, cl_mem mem_object, void *mapped_ptr)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Unmap Mem Object]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueUnmapMemObject)(
      darktable.opencl->dev[devid].cmd_queue, mem_object, mapped_ptr, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl unmap mem object] could not unmap mem object: %d\n", err);
  return err;
}

void *dt_opencl_alloc_device(const int devid, const int width, const int height, const int bpp)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
      darktable.opencl->dev[devid].context, CL_MEM_READ_WRITE, &fmt, width, height, 0, NULL, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device] could not alloc img buffer on device %d: %d\n", devid,
             err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void *dt_opencl_alloc_device_use_host_pointer(const int devid, const int width, const int height,
                                              const int bpp, const int rowpitch, void *host)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
      darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE | ((host == NULL) ? CL_MEM_ALLOC_HOST_PTR : CL_MEM_USE_HOST_PTR), &fmt, width, height,
      rowpitch, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device_use_host_pointer] could not alloc img buffer on device %d: %d\n", devid,
             err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void *dt_opencl_alloc_device_buffer(const int devid, const size_t size)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;

  cl_mem buf = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)(darktable.opencl->dev[devid].context,
                                                                     CL_MEM_READ_WRITE, size, NULL, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device_buffer] could not alloc buffer on device %d: %d\n", devid,
             err);

  dt_opencl_memory_statistics(devid, buf, OPENCL_MEMORY_ADD);

  return buf;
}

void *dt_opencl_alloc_device_buffer_with_flags(const int devid, const size_t size, const int flags)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;

  cl_mem buf = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)(darktable.opencl->dev[devid].context,
                                                                     flags, size, NULL, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device_buffer] could not alloc buffer on device %d: %d\n", devid,
             err);

  dt_opencl_memory_statistics(devid, buf, OPENCL_MEMORY_ADD);

  return buf;
}

size_t dt_opencl_get_mem_object_size(cl_mem mem)
{
  cl_int err;
  size_t size;
  if(mem == NULL) return 0;

  err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)(mem, CL_MEM_SIZE, sizeof(size), &size, NULL);

  return (err == CL_SUCCESS) ? size : 0;
}

int dt_opencl_get_mem_context_id(cl_mem mem)
{
  cl_int err;
  cl_context context;
  if(mem == NULL) return -1;

  err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)(mem, CL_MEM_CONTEXT, sizeof(context), &context, NULL);
  if(err != CL_SUCCESS)
    return -1;

  for(int devid = 0; devid < darktable.opencl->num_devs; devid++)
  {
    if(darktable.opencl->dev[devid].context == context)
      return devid;
  }

  return -1;
}

void dt_opencl_memory_statistics(int devid, cl_mem mem, dt_opencl_memory_t action)
{
  if(devid < 0)
    devid = dt_opencl_get_mem_context_id(mem);

  if(devid < 0)
    return;

  if(action == OPENCL_MEMORY_ADD)
    darktable.opencl->dev[devid].memory_in_use += dt_opencl_get_mem_object_size(mem);
  else
    darktable.opencl->dev[devid].memory_in_use -= dt_opencl_get_mem_object_size(mem);

  darktable.opencl->dev[devid].peak_memory = MAX(darktable.opencl->dev[devid].peak_memory,
                                                 darktable.opencl->dev[devid].memory_in_use);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
    dt_print(DT_DEBUG_OPENCL,
              "[opencl memory] device %d: %zu bytes (%.1f MB) in use\n", devid, darktable.opencl->dev[devid].memory_in_use,
                                      (float)darktable.opencl->dev[devid].memory_in_use/(1024*1024));
}

/** check if image size fit into limits given by OpenCL runtime */
int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const unsigned bpp,
                                const float factor, const size_t overhead)
{
  static float headroom = -1.0f;

  if(!darktable.opencl->inited || devid < 0) return FALSE;

  /* first time run */
  if(headroom < 0.0f)
  {
    headroom = dt_conf_get_float("opencl_memory_headroom") * 1024.0f * 1024.0f;

    /* don't let the user play games with us */
    headroom = fmin((float)darktable.opencl->dev[devid].max_global_mem, fmax(headroom, 0.0f));
    dt_conf_set_int("opencl_memory_headroom", headroom / 1024 / 1024);
  }

  float singlebuffer = (float)width * height * bpp;
  float total = factor * singlebuffer + overhead;

  if(darktable.opencl->dev[devid].max_image_width < width
     || darktable.opencl->dev[devid].max_image_height < height)
    return FALSE;

  if(darktable.opencl->dev[devid].max_mem_alloc < singlebuffer) return FALSE;

  if(darktable.opencl->dev[devid].max_global_mem < total + headroom) return FALSE;

  return TRUE;
}


/** round size to a multiple of the value given in config parameter opencl_size_roundup */
int dt_opencl_roundup(int size)
{
  static int roundup = -1;

  /* first time run */
  if(roundup < 0)
  {
    roundup = dt_conf_get_int("opencl_size_roundup");

    /* if not yet defined (or unsane), set a sane default */
    if(roundup <= 0)
    {
      roundup = 16;
      dt_conf_set_int("opencl_size_roundup", roundup);
    }
  }

  return (size % roundup == 0 ? size : (size / roundup + 1) * roundup);
}


/** check if opencl is inited */
int dt_opencl_is_inited(void)
{
  return darktable.opencl->inited;
}


/** check if opencl is enabled */
int dt_opencl_is_enabled(void)
{
  if(!darktable.opencl->inited) return FALSE;
  return darktable.opencl->enabled;
}


/** disable opencl */
void dt_opencl_disable(void)
{
  if(!darktable.opencl->inited) return;
  darktable.opencl->enabled = FALSE;
  dt_conf_set_bool("opencl", FALSE);
}


/** update enabled flag and profile with value from preferences, returns enabled flag */
int dt_opencl_update_settings(void)
{
  if(!darktable.opencl->inited) return FALSE;
  const int prefs = dt_conf_get_bool("opencl");

  if(darktable.opencl->enabled != prefs)
  {
    darktable.opencl->enabled = prefs;
    darktable.opencl->stopped = 0;
    darktable.opencl->error_count = 0;
    dt_print(DT_DEBUG_OPENCL, "[opencl_update_enabled] enabled flag set to %s\n", prefs ? "ON" : "OFF");
  }

  dt_opencl_scheduling_profile_t profile = dt_opencl_get_scheduling_profile();

  if(darktable.opencl->scheduling_profile != profile)
  {
    char *pstr = dt_conf_get_string("opencl_scheduling_profile");
    dt_print(DT_DEBUG_OPENCL, "[opencl_update_scheduling_profile] scheduling profile set to %s\n", pstr);
    g_free(pstr);
    dt_opencl_apply_scheduling_profile(profile);
  }

  return (darktable.opencl->enabled && !darktable.opencl->stopped);
}

/** read scheduling profile for config variables */
static dt_opencl_scheduling_profile_t dt_opencl_get_scheduling_profile(void)
{
  char *pstr = dt_conf_get_string("opencl_scheduling_profile");
  if(!pstr) return OPENCL_PROFILE_DEFAULT;

  dt_opencl_scheduling_profile_t profile = OPENCL_PROFILE_DEFAULT;

  if(!strcmp(pstr, "multiple GPUs"))
    profile = OPENCL_PROFILE_MULTIPLE_GPUS;
  else if(!strcmp(pstr, "very fast GPU"))
    profile = OPENCL_PROFILE_VERYFAST_GPU;

  g_free(pstr);

  return profile;
}

/** set opencl specific synchronization timeout */
static void dt_opencl_set_synchronization_timeout(int value)
{
  darktable.opencl->opencl_synchronization_timeout = value;
  dt_print(DT_DEBUG_OPENCL, "[opencl_synchronization_timeout] synchronization timeout set to %d\n", value);
}

/** adjust opencl subsystem according to scheduling profile */
static void dt_opencl_apply_scheduling_profile(dt_opencl_scheduling_profile_t profile)
{
  char *str;

  dt_pthread_mutex_lock(&darktable.opencl->lock);
  darktable.opencl->scheduling_profile = profile;

  switch(profile)
  {
    case OPENCL_PROFILE_MULTIPLE_GPUS:
      dt_opencl_update_priorities("*/*/*/*");
      dt_opencl_set_synchronization_timeout(20);
      break;
    case OPENCL_PROFILE_VERYFAST_GPU:
      dt_opencl_update_priorities("+*/+*/+*/+*");
      dt_opencl_set_synchronization_timeout(0);
      break;
    case OPENCL_PROFILE_DEFAULT:
    default:
      str = dt_conf_get_string("opencl_device_priority");
      dt_opencl_update_priorities(str);
      g_free(str);
      dt_opencl_set_synchronization_timeout(dt_conf_get_int("pixelpipe_synchronization_timeout"));
      break;
  }
  dt_pthread_mutex_unlock(&darktable.opencl->lock);
}

/** get global memory of device */
cl_ulong dt_opencl_get_max_global_mem(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  return darktable.opencl->dev[devid].max_global_mem;
}


/** the following eventlist functions assume that affected structures are locked upstream */

/** get next free slot in eventlist (and manage size of eventlist) */
cl_event *dt_opencl_events_get_slot(const int devid, const char *tag)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return NULL;
  if(!cl->use_events) return NULL;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalevents = &(cl->dev[devid].totalevents);
  int *totallost = &(cl->dev[devid].totallost);

  // if first time called: allocate initial buffers
  if(*eventlist == NULL)
  {
    int newevents = DT_OPENCL_EVENTLISTSIZE;
    *eventlist = calloc(newevents, sizeof(cl_event));
    *eventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!*eventlist || !*eventtags)
    {
      free(*eventlist);
      free(*eventtags);
      *eventlist = NULL;
      *eventtags = NULL;
      return NULL;
    }
    *maxevents = newevents;
  }

  // check if currently highest event slot was actually consumed. If not use it again
  if(*numevents > 0 && !memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*lostevents)++;
    (*totallost)++;
    if(tag != NULL)
    {
      g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
    }
    else
    {
      (*eventtags)[*numevents - 1].tag[0] = '\0';
    }

    (*totalevents)++;
    return (*eventlist) + *numevents - 1;
  }

  // check if we would exceed the number of available event handles. In that case first flush existing handles
  if(*numevents - *eventsconsolidated + 1 > darktable.opencl->number_event_handles)
    (void)dt_opencl_events_flush(devid, FALSE);


  // if no more space left in eventlist: grow buffer
  if(*numevents == *maxevents)
  {
    int newevents = *maxevents + DT_OPENCL_EVENTLISTSIZE;
    cl_event *neweventlist = calloc(newevents, sizeof(cl_event));
    dt_opencl_eventtag_t *neweventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!neweventlist || !neweventtags)
    {
      free(neweventlist);
      free(neweventtags);
      return NULL;
    }
    memcpy(neweventlist, *eventlist, *maxevents * sizeof(cl_event));
    memcpy(neweventtags, *eventtags, *maxevents * sizeof(dt_opencl_eventtag_t));
    free(*eventlist);
    free(*eventtags);
    *eventlist = neweventlist;
    *eventtags = neweventtags;
    *maxevents = newevents;
  }

  // init next event slot and return it
  (*numevents)++;
  memcpy((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event));
  if(tag != NULL)
  {
    g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
  }
  else
  {
    (*eventtags)[*numevents - 1].tag[0] = '\0';
  }

  (*totalevents)++;
  return (*eventlist) + *numevents - 1;
}


/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // release all remaining events in eventlist, not to waste resources
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
  }

  memset(*eventtags, 0, *maxevents * sizeof(dt_opencl_eventtag_t));
  *numevents = 0;
  *eventsconsolidated = 0;
  *lostevents = 0;
  *summary = CL_COMPLETE;
  return;
}


/** Wait for events in eventlist to terminate -> this is a blocking synchronization point!
    Does not flush eventlist. Side effect: might adjust numevents. */
void dt_opencl_events_wait_for(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->use_events) return;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  int *numevents = &(cl->dev[devid].numevents);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totallost = &(cl->dev[devid].totallost);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // check if last event slot was actually used and correct numevents if needed
  if(!memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*numevents)--;
    (*lostevents)++;
    (*totallost)++;
  }

  if(*numevents == *eventsconsolidated) return; // nothing to do

  assert(*numevents > *eventsconsolidated);

  // now wait for all remaining events to terminate
  // Risk: might never return in case of OpenCL blocks or endless loops
  // TODO: run clWaitForEvents in separate thread and implement watchdog timer
  (cl->dlocl->symbols->dt_clWaitForEvents)(*numevents - *eventsconsolidated,
                                           (*eventlist) + *eventsconsolidated);

  return;
}


/** Wait for events in eventlist to terminate, check for return status and profiling
info of events.
If "reset" is TRUE report summary info (would be CL_COMPLETE or last error code) and
print profiling info if needed.
If "reset" is FALSE just store info (success value, profiling) from terminated events
and release events for re-use by OpenCL driver. */
cl_int dt_opencl_events_flush(const int devid, const int reset)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;
  if(!cl->use_events) return FALSE;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalsuccess = &(cl->dev[devid].totalsuccess);

  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0) return CL_COMPLETE; // nothing to do, no news is good news

  // Wait for command queue to terminate (side effect: might adjust *numevents)
  dt_opencl_events_wait_for(devid);

  // now check return status and profiling data of all newly terminated events
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    cl_int err;
    char *tag = (*eventtags)[k].tag;
    cl_int *retval = &((*eventtags)[k].retval);

    // get return value of event
    err = (cl->dlocl->symbols->dt_clGetEventInfo)((*eventlist)[k], CL_EVENT_COMMAND_EXECUTION_STATUS,
                                                  sizeof(cl_int), retval, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] could not get event info for '%s': %d\n",
               tag[0] == '\0' ? "<?>" : tag, err);
    }
    else if(*retval != CL_COMPLETE)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] execution of '%s' %s: %d\n",
               tag[0] == '\0' ? "<?>" : tag, *retval == CL_COMPLETE ? "was successful" : "failed", *retval);
      *summary = *retval;
    }
    else
      (*totalsuccess)++;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      // get profiling info of event (only if darktable was called with '-d perf')
      cl_ulong start;
      cl_ulong end;
      cl_int errs = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)(
          (*eventlist)[k], CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
      cl_int erre = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)((*eventlist)[k], CL_PROFILING_COMMAND_END,
                                                                   sizeof(cl_ulong), &end, NULL);
      if(errs == CL_SUCCESS && erre == CL_SUCCESS)
      {
        (*eventtags)[k].timelapsed = end - start;
      }
      else
      {
        (*eventtags)[k].timelapsed = 0;
        (*lostevents)++;
      }
    }
    else
      (*eventtags)[k].timelapsed = 0;

    // finally release event to be re-used by driver
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
    (*eventsconsolidated)++;
  }

  cl_int result = *summary;

  // do we want to get rid of all stored info?
  if(reset)
  {
    // output profiling info if wanted
    if(darktable.unmuted & DT_DEBUG_PERF) dt_opencl_events_profiling(devid, 1);

    // reset eventlist structures to empty state
    dt_opencl_events_reset(devid);
  }

  return result == CL_COMPLETE ? 0 : result;
}


/** display OpenCL profiling information. If "aggregated" is TRUE, try to generate summarized info for each
 * kernel */
void dt_opencl_events_profiling(const int devid, const int aggregated)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);

  if(*eventlist == NULL || *numevents == 0 || *eventtags == NULL || *eventsconsolidated == 0)
    return; // nothing to do

  char **tags = malloc((*eventsconsolidated + 1) * sizeof(char *));
  float *timings = malloc((*eventsconsolidated + 1) * sizeof(float));
  int items = 1;
  tags[0] = "";
  timings[0] = 0.0f;

  // get profiling info and arrange it
  for(int k = 0; k < *eventsconsolidated; k++)
  {
    // if aggregated is TRUE, try to sum up timings for multiple runs of each kernel
    if(aggregated)
    {
      // linear search: this is not efficient at all but acceptable given the limited
      // number of events (ca. 10 - 20)
      int tagfound = -1;
      for(int i = 0; i < items; i++)
      {
        if(!strncmp(tags[i], (*eventtags)[k].tag, DT_OPENCL_EVENTNAMELENGTH))
        {
          tagfound = i;
          break;
        }
      }

      if(tagfound >= 0) // tag was already detected before
      {
        // sum up timings
        timings[tagfound] += (*eventtags)[k].timelapsed * 1e-9;
      }
      else // tag is new
      {
        // make new entry
        items++;
        tags[items - 1] = (*eventtags)[k].tag;
        timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
      }
    }

    else // no aggregated info wanted -> arrange event by event
    {
      items++;
      tags[items - 1] = (*eventtags)[k].tag;
      timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
    }
  }

  // now display profiling info
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] profiling device %d ('%s'):\n", devid,
           cl->dev[devid].name);

  float total = 0.0f;
  for(int i = 1; i < items; i++)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds in %s\n", (double)timings[i],
             tags[i][0] == '\0' ? "<?>" : tags[i]);
    total += timings[i];
  }
  // aggregated timing info for items without tag (if any)
  if(timings[0] != 0.0f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds (unallocated)\n", (double)timings[0]);
    total += timings[0];
  }

  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] spent %7.4f seconds totally in command queue (with %d event%s missing)\n",
           (double)total, *lostevents, *lostevents == 1 ? "" : "s");

  free(timings);
  free(tags);

  return;
}

static int nextpow2(int n)
{
  int k = 1;
  while (k < n)
    k <<= 1;
  return k;
}

// utility function to calculate optimal work group dimensions for a given kernel
// taking device specific restrictions and local memory limitations into account
int dt_opencl_local_buffer_opt(const int devid, const int kernel, dt_opencl_local_buffer_t *factors)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  int *blocksizex = &factors->sizex;
  int *blocksizey = &factors->sizey;

  // initial values must be supplied in sizex and sizey.
  // we make sure that these are a power of 2 and lie within reasonable limits.
  *blocksizex = CLAMP(nextpow2(*blocksizex), 1, 1 << 16);
  *blocksizey = CLAMP(nextpow2(*blocksizey), 1, 1 << 16);

  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, kernel, &kernelworkgroupsize) == CL_SUCCESS)
  {
    while(maxsizes[0] < *blocksizex || maxsizes[1] < *blocksizey
       || localmemsize < ((factors->xfactor * (*blocksizex) + factors->xoffset) *
                          (factors->yfactor * (*blocksizey) + factors->yoffset)) * factors->cellsize + factors->overhead
       || workgroupsize < (*blocksizex) * (*blocksizey) || kernelworkgroupsize < (*blocksizex) * (*blocksizey))
    {
      if(*blocksizex == 1 && *blocksizey == 1) return FALSE;

      if(*blocksizex > *blocksizey)
        *blocksizex >>= 1;
      else
        *blocksizey >>= 1;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL,
         "[opencl_demosaic] can not identify resource limits for device %d\n", devid);
    return FALSE;
  }

  return TRUE;
}


#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
