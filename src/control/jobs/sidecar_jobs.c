/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "control/jobs/sidecar_jobs.h"

static GSList *pending_images = NULL;
static gboolean background_running = FALSE;

#ifndef _OPENMP
#include "common/dtpthread.h"
static gboolean lock_initialized = FALSE;
static dt_pthread_mutex_t pending_mutex;

static void _lock_pending_queue()
{
  if(!lock_initialized)
  {
    dt_pthread_mutex_init(&pending_mutex, NULL);
    lock_initialized = TRUE;
  }
  dt_pthread_mutex_lock(&pending_mutex);
}

static void _unlock_pending_queue()
{
  if(lock_initialized)
  {
    dt_pthread_mutex_unlock(&pending_mutex);
  }
}
#endif /* !_OPENMP */

static int32_t _control_write_sidecars_job_run(dt_job_t *job)
{
  GSList *imgs = NULL;
  GHashTable *enqueued = g_hash_table_new(g_direct_hash, g_direct_equal);

  double prev_fetch = 0;
  // keep going until explicitly cancelled or darktable shuts down AND all writes have finished
  while(imgs || (dt_control_running() && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED))
  {
    GSList *new_imgs = NULL;
    double curr_fetch = dt_get_wtime();
    // grab any pending images and add them to the list of images to be synchronized
    if(curr_fetch > prev_fetch + 0.25)
    {
      prev_fetch = curr_fetch;
#ifdef _OPENMP
#pragma omp atomic capture
      { new_imgs = pending_images; pending_images = NULL ; }
#else
      // don't have atomics, so use locks instead
      _lock_pending_queue();
      new_imgs = pending_images;
      pending_images = NULL;
      _unlock_pending_queue();
#endif
      if(new_imgs)
      {
        // add the new images to the queue being processed if they are not already on the queue
        GSList *to_add = NULL;
        for(GSList *imglist = new_imgs; imglist; imglist = g_slist_next(imglist))
        {
          if(!g_hash_table_contains(enqueued,GINT_TO_POINTER(imglist->data)))
          {
            to_add = g_slist_prepend(to_add, imglist->data);
            g_hash_table_insert(enqueued, GINT_TO_POINTER(imglist->data), GINT_TO_POINTER(imglist->data));
          }
        }
        imgs = g_slist_concat(imgs, to_add);
        g_slist_free(new_imgs);
      }
    }
    // synchronize the first few images on the queue
    for(int i = 0; imgs && i < 3; i++)
    {
      dt_imgid_t imgid = GPOINTER_TO_INT(imgs->data);
      dt_image_write_sidecar_file(imgid);
      // remove the head of the image queue
      g_hash_table_remove(enqueued, GINT_TO_POINTER(imgid));
      imgs = g_slist_delete_link(imgs, imgs);
    }
    if(imgs) // do we have more images already queued?
    {
      // give others a chance to run by sleeping 10ms; avoids apparent
      // hangs when trying to switch views
      g_usleep(10000);
    }
    else
    {
      // we currently have nothing to do, so wait 1 second before checking for more work
      g_usleep(1000000);
    }
  }
  g_hash_table_destroy(enqueued);
  return 0;
}

void dt_sidecar_synch_enqueue(dt_imgid_t imgid)
{
  if(background_running)
  {
    GSList *img = g_slist_prepend(NULL,GINT_TO_POINTER(imgid));
#ifdef _OPENMP
#pragma omp atomic capture
    { img->next = pending_images; pending_images = img; }
#else
    // don't have atomics, so use locks instead
    _lock_pending_queue();
    img->next = pending_images;
    pending_images = img;
    _unlock_pending_queue();
#endif
  }
  else
  {
    // synchronize the sidecar immediately instead of queueing it for background write
    dt_image_write_sidecar_file(imgid);
  }
}

void dt_sidecar_synch_enqueue_list(const GList *imgs)
{
  if(!imgs)
    return;
  if(!background_running)
  {
    // synchronize the sidecars immediately instead of queueing them for background write
    for(const GList *ilist = imgs; ilist; ilist = g_list_next(ilist))
    {
      dt_image_write_sidecar_file(GPOINTER_TO_INT(ilist->data));
    }
    return;
  }
  GSList *new_imgs = NULL;
  for(const GList *ilist = imgs; ilist; ilist = g_list_next(ilist))
  {
    new_imgs = g_slist_prepend(new_imgs,ilist->data);
  }
  GSList *last = g_slist_last(new_imgs);
#ifdef _OPENMP
#pragma omp atomic capture
  { last->next = pending_images; pending_images = new_imgs; }
#else
  // don't have atomics, so use locks instead
  _lock_pending_queue();
  last->next = pending_images;
  pending_images = new_imgs;
  _unlock_pending_queue();
#endif
}

void dt_control_sidecar_synch_start()
{
  dt_job_t *job = dt_control_job_create(_control_write_sidecars_job_run, "%s", N_("synchronize sidecars"));
  if(!job)
  {
    return;
  }
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, job);
  background_running = TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
