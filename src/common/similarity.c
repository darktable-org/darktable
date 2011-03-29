/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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

#include "control/control.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/darktable.h"
#include "common/similarity.h"

#if 0
static void dump_histogram(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  fprintf(stderr, "histogram for %d:",imgid);
  for(int j=0;j<DT_SIMILARITY_HISTOGRAM_BUCKETS;j++)
    fprintf(stderr," [%f, %f, %f, %f]",histogram->rgbl[j][0],histogram->rgbl[j][1],histogram->rgbl[j][2],histogram->rgbl[j][3]);
  fprintf(stderr,"\n");
}
#endif

/* matches the rgb histogram and returns a score for the match */
float _similarity_match_histogram_rgb( const dt_similarity_histogram_t *target, const dt_similarity_histogram_t *source)
{
  float score=0;
  for(int k=0;k<DT_SIMILARITY_HISTOGRAM_BUCKETS;k++)
    for(int j=0;j<3;j++)
      score += ( 1 - fabs(target->rgbl[k][j] - source->rgbl[k][j]) )/3.0;
  
  score/=DT_SIMILARITY_HISTOGRAM_BUCKETS;
  
  return score;
}

void dt_similarity_match_image(uint32_t imgid)
{
  gboolean all_ok_for_match = TRUE;
  dt_similarity_histogram_t orginal,test;
  sqlite3_stmt *stmt;
  
  /* create temporary mem table */
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "create temporary table if not exists similar_images (id integer,score real)", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from similar_images", NULL, NULL, NULL);
  
  /* get the histogram data for image to match against */
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select histogram from images id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* verify size of histogram blob */
    uint32_t size = sqlite3_column_bytes(stmt,0);
    if (size!=sizeof(dt_similarity_histogram_t)) {
      all_ok_for_match = FALSE;
      dt_control_log(_("histogram is corrupted, please open target image in darkroom and retry match."));
    } else 
      memcpy(&orginal, sqlite3_column_blob(stmt, 0), sizeof(dt_similarity_histogram_t));
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  
  if (all_ok_for_match) {
    char query[4096]={0};
    
    /* set an extended collection query for viewing the result of match */
    dt_collection_set_extended_where(darktable.collection,"id in (select id from similar_images order by score)");
    
    /* add target image with 1.0 in score into result */
    sprintf(query,"insert into similar_images(id,score) values(%d,%f)",imgid,1.0);
    DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
    
    /* loop thru images and generate score table */
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select id,histogram from images", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      /* verify size of histogram blob */
      uint32_t size = sqlite3_column_bytes(stmt,1);
      if (size == sizeof(dt_similarity_histogram_t)) 
      {
        float score = 0;
        memcpy(&test, sqlite3_column_blob(stmt, 1), sizeof(dt_similarity_histogram_t));
        
        /* get score for histogram similarity */
        score = _similarity_match_histogram_rgb(&orginal,&test);
        
        /* if above score threshold \todo user configurable ??? */
        if(score > 0.5)
        {
          sprintf(query,"insert into similar_images(id,score) values(%d,%f)",sqlite3_column_int(stmt, 0),score);
          DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
          
          /* new image added, lets redraw the view */
          dt_control_queue_draw_all();
        }
        
      }
    }
  }
  
  sqlite3_finalize (stmt);  
}

void dt_similarity_store_histogram(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set histogram =?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, histogram,sizeof(dt_similarity_histogram_t),SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
  //dump_histogram(imgid,histogram);
}