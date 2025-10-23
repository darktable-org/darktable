#include "control/conf.h"
#include "darktable.h"
//
// Created by mikesolar on 25-10-21.
//
char *_get_version_string(void);
struct configs_from_command configs_from_command={0};
int usage(const char *argv0);

#include "darktable_application.h"

void darktable_application_finalize (GObject *object)
{
  G_OBJECT_CLASS (darktable_application_parent_class)->finalize (object);
}


GApplication *darktable_application_new (const gchar       *application_id,
                      GApplicationFlags  flags)
{
  g_return_val_if_fail (g_application_id_is_valid (application_id), NULL);

  return g_object_new (darktable_application_get_type (),
                       "application-id", application_id,
                       "flags", flags,
                       NULL);
}



int handle_command(GApplication   *application,
                    gchar        ***arguments,
                    gint           *exit_status
)
{
  char **argv = *arguments;
  int index=0;
  while(argv)
  {
    index++;
    argv++;
  }
  int argc = index;
  gchar **myoptions = argc > 1 ? g_strdupv(argv) : NULL;

  for(int k = 1; k < argc; k++)
  {
#ifdef _WIN32
    if(!strcmp(argv[k], "/?"))
    {
      g_strfreev(myoptions);
      return usage(argv[0]);
    }
#endif
    if(argv[k][0] == '-')
    {
      if(!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h"))
      {
        g_strfreev(myoptions);
        return usage(argv[0]);
      }

      else if(!strcmp(argv[k], "--version") || !strcmp(argv[k], "-v"))
      {
        char *theversion = _get_version_string();
        printf("%s", theversion);
        g_free(theversion);
        g_strfreev(myoptions);
        return 1;
      }
      else if(!strcmp(argv[k], "--dump-pfm") && argc > k + 1)
      {
        darktable.dump_pfm_module = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--bench-module") && argc > k + 1)
      {
        darktable.bench_module = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--dump-pipe") && argc > k + 1)
      {
        darktable.dump_pfm_pipe = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--dump-diff-pipe") && argc > k + 1)
      {
        darktable.dump_diff_pipe = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--library") && argc > k + 1)
      {
        configs_from_command.dbfilename_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--datadir") && argc > k + 1)
      {
        configs_from_command.datadir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--moduledir") && argc > k + 1)
      {
        configs_from_command.moduledir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--tmpdir") && argc > k + 1)
      {
        configs_from_command.tmpdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--configdir") && argc > k + 1)
      {
        configs_from_command.configdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--cachedir") && argc > k + 1)
      {
        configs_from_command.cachedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--dumpdir") && argc > k + 1)
      {
        darktable.tmp_directory = g_strdup(argv[++k]);
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--localedir") && argc > k + 1)
      {
        configs_from_command.localedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 'd' && argc > k + 1)
      {
        char *darg = argv[k + 1];
        dt_debug_thread_t dadd =
          !strcmp(darg, "common") ? DT_DEBUG_COMMON : // enable common processing options
          !strcmp(darg, "all") ? DT_DEBUG_ALL : // enable all debug information except verbose
          !strcmp(darg, "cache") ? DT_DEBUG_CACHE : // enable debugging for lib/film/cache module
          !strcmp(darg, "control") ? DT_DEBUG_CONTROL : // enable debugging for scheduler module
          !strcmp(darg, "dev") ? DT_DEBUG_DEV : // develop module
          !strcmp(darg, "input") ? DT_DEBUG_INPUT : // input devices
          !strcmp(darg, "camctl") ? DT_DEBUG_CAMCTL : // camera control module
          !strcmp(darg, "perf") ? DT_DEBUG_PERF : // performance measurements
          !strcmp(darg, "pwstorage") ? DT_DEBUG_PWSTORAGE : // pwstorage module
          !strcmp(darg, "opencl") ? DT_DEBUG_OPENCL : // gpu accel via opencl
          !strcmp(darg, "sql") ? DT_DEBUG_SQL : // SQLite3 queries
          !strcmp(darg, "memory") ? DT_DEBUG_MEMORY : // some stats on mem usage now and then.
          !strcmp(darg, "lighttable") ? DT_DEBUG_LIGHTTABLE : // lighttable related stuff.
          !strcmp(darg, "nan") ? DT_DEBUG_NAN : // check for NANs when processing the pipe.
          !strcmp(darg, "masks") ? DT_DEBUG_MASKS : // masks related stuff.
          !strcmp(darg, "lua") ? DT_DEBUG_LUA : // lua errors are reported on console
          !strcmp(darg, "print") ? DT_DEBUG_PRINT : // print errors are reported on console
          !strcmp(darg, "camsupport") ? DT_DEBUG_CAMERA_SUPPORT : // camera support warnings are reported on console
          !strcmp(darg, "ioporder") ? DT_DEBUG_IOPORDER : // iop order information are reported on console
          !strcmp(darg, "imageio") ? DT_DEBUG_IMAGEIO : // image importing or exporting messages on console
          !strcmp(darg, "undo") ? DT_DEBUG_UNDO : // undo/redo
          !strcmp(darg, "signal") ? DT_DEBUG_SIGNAL : // signal information on console
          !strcmp(darg, "params") ? DT_DEBUG_PARAMS : // iop module params checks on console
          !strcmp(darg, "act_on") ? DT_DEBUG_ACT_ON :
          !strcmp(darg, "tiling") ? DT_DEBUG_TILING :
          !strcmp(darg, "verbose") ? DT_DEBUG_VERBOSE :
          !strcmp(darg, "pipe") ? DT_DEBUG_PIPE :
          !strcmp(darg, "expose") ? DT_DEBUG_EXPOSE :
          !strcmp(darg, "picker") ? DT_DEBUG_PICKER :
          0;
        if(dadd)
          darktable.unmuted |= dadd;
        else
        {
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal-act") && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted_signal_dbg_acts = 0xffffffff; // enable all signal debug information
        else if(!strcmp(argv[k + 1], "raise"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_RAISE; // enable debugging for signal raising
        else if(!strcmp(argv[k + 1], "connect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_CONNECT; // enable debugging for signal connection
        else if(!strcmp(argv[k + 1], "disconnect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_DISCONNECT; // enable debugging for signal disconnection
        else if(!strcmp(argv[k + 1], "print-trace"))
        {
#ifdef DT_HAVE_SIGNAL_TRACE
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_PRINT_TRACE; // enable printing of signal tracing
#else
          dt_print(DT_DEBUG_ALWAYS, "[signal] print-trace not available, skipping");
#endif
        }
        else
        {
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal") && argc > k + 1)
      {
        gchar *str = g_ascii_strup(argv[k+1], -1);

        #define CHKSIGDBG(sig) else if(!g_strcmp0(str, #sig)) do {darktable.unmuted_signal_dbg[sig] = TRUE;} while(0)
        if(!g_strcmp0(str, "ALL"))
        {
          for(int sig=0; sig<DT_SIGNAL_COUNT; sig++)
            darktable.unmuted_signal_dbg[sig] = TRUE;
        }
        CHKSIGDBG(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_ALL);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_CENTER);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE);
        CHKSIGDBG(DT_SIGNAL_COLLECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_SELECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_TAG_CHANGED);
        CHKSIGDBG(DT_SIGNAL_METADATA_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_INFO_CHANGED);
        CHKSIGDBG(DT_SIGNAL_STYLE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGES_ORDER_CHANGE);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_CHANGED);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_IMPORTED);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_REMOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_INITIALIZE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_REMOVE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_MOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_IMPORT);
        CHKSIGDBG(DT_SIGNAL_IMAGE_EXPORT_TMPFILE);
        CHKSIGDBG(DT_SIGNAL_IMAGEIO_STORAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_PREFERENCES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CAMERA_DETECTED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_LOG_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_TOAST_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PICKERDATA_READY);
        CHKSIGDBG(DT_SIGNAL_METADATA_UPDATE);
        CHKSIGDBG(DT_SIGNAL_PRESET_APPLIED);
        else
        {
          dt_print(DT_DEBUG_SIGNAL,
                   "[dt_init] unknown signal name: '%s'. use 'ALL'"
                   " to enable debug for all or use full signal name", str);
          g_strfreev(myoptions);
          return usage(argv[0]);
        }
        g_free(str);
        #undef CHKSIGDBG
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if((argv[k][1] == 't' && argc > k + 1)
              || (!strcmp(argv[k], "--threads")
                  && argc > k + 1))
      {
        const int possible = dt_get_num_procs();
        const int desired = atol(argv[k + 1]);
        darktable.num_openmp_threads = CLAMP(desired, 1, possible);
        if(desired > possible)
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_init --threads] requested %d ompthreads restricted to %d",
                   desired, possible);
#ifdef _OPENMP
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_init --threads] using %d threads of %d for openmp parallel sections %s",
                 darktable.num_openmp_threads, (int)dt_get_num_procs(),
                 omp_get_dynamic() ? "(dynamic)" : "(static)");
#endif
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--conf") && argc > k + 1)
      {
        gchar *keyval = g_strdup(argv[++k]), *c = keyval;
        argv[k-1] = NULL;
        argv[k] = NULL;
        gchar *end = keyval + strlen(keyval);
        while(*c != '=' && c < end) c++;
        if(*c == '=' && *(c + 1) != '\0')
        {
          *c++ = '\0';
          dt_conf_string_entry_t *entry = g_malloc(sizeof(dt_conf_string_entry_t));
          entry->key = g_strdup(keyval);
          entry->value = g_strdup(c);
          configs_from_command.config_override = g_slist_append(configs_from_command.config_override, entry);
        }
        g_free(keyval);
      }
      else if(!strcmp(argv[k], "--noiseprofiles") && argc > k + 1)
      {
        configs_from_command.noiseprofiles_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--luacmd") && argc > k + 1)
      {
#ifdef USE_LUA
        configs_from_command.lua_command = argv[++k];
#else
        ++k;
#endif
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--disable-opencl"))
      {
#ifdef HAVE_OPENCL
        configs_from_command.exclude_opencl = TRUE;
#endif
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--disable-pipecache"))
      {
        darktable.pipe_cache = FALSE;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--gimp"))
      {
        argv[k] = NULL;
        darktable.gimp.error = TRUE;

        if(argc > k + 1)
        {
          darktable.gimp.mode = argv[++k];
          argv[k-1] = NULL;
          argv[k] = NULL;

          if(dt_check_gimpmode("version"))
          {
            darktable.gimp.error = FALSE;
          }
          else if(dt_check_gimpmode("file") && (argc > k + 1))
          {
            darktable.gimp.path = argv[++k];
            argv[k-1] = NULL;
            argv[k] = NULL;

            if(g_file_test(darktable.gimp.path, G_FILE_TEST_IS_REGULAR))
              darktable.gimp.error = FALSE;
          }
          else if(dt_check_gimpmode("thumb") && (argc > k + 2))
          {
            darktable.gimp.path = argv[++k];
            argv[k-1] = NULL;
            argv[k] = NULL;

            if(g_file_test(darktable.gimp.path, G_FILE_TEST_IS_REGULAR))
            {
              darktable.gimp.size = atol(argv[k + 1]);
              k++;
              argv[k-1] = NULL;
              argv[k] = NULL;

              if(darktable.gimp.size > 0)
                darktable.gimp.error = FALSE;
            }
          }
        }
      }
      else if(!strcmp(argv[k], "--"))
      {
        // "--" confuses the argument parser of glib/gtk. remove it.
        argv[k] = NULL;
        break;
      }
#ifdef __APPLE__
      else if(!strncmp(argv[k], "-psn_", 5))
      {
        // "-psn_*" argument is added automatically by macOS and should be ignored
        argv[k] = NULL;
      }
#endif
      else
      {
        g_strfreev(myoptions);
        return usage(argv[0]); // fail on unrecognized options
      }
    }
  }

  // remove the NULLs to not confuse gtk_init() later.
  for(int i = 1; i < argc; i++)
  {
    int k;
    for(k = i; k < argc; k++)
      if(argv[k] != NULL) break;

    if(k > i)
    {
      k -= i;
      for(int j = i + k; j < argc; j++)
      {
        argv[j-k] = argv[j];
        argv[j] = NULL;
      }
      argc -= k;
    }
  }

  /* We now have all command line options ready and check for gimp API questions.
      Return right now if we
        - check for API version
        - check for "file" or "thumb" and the file is not physically available
        - have an undefined gimp mode
      and let the caller check again and write out protocol messages.
  */
  if(dt_check_gimpmode("version")
    || (dt_check_gimpmode("file") && !dt_check_gimpmode_ok("file"))
    || (dt_check_gimpmode("thumb") && !dt_check_gimpmode_ok("thumb"))
    || darktable.gimp.error)
    return 0;
  return 0;
}

