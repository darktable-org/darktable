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

#define CLIP(x) (fmax(0,fmin(1.0,x)))

static void _similarity_dump_histogram(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  fprintf(stderr, "histogram for %d:",imgid);
  for(int j=0;j<DT_SIMILARITY_HISTOGRAM_BUCKETS;j++)
    fprintf(stderr," [%f, %f, %f, %f]",histogram->rgbl[j][0],histogram->rgbl[j][1],histogram->rgbl[j][2],histogram->rgbl[j][3]);
  fprintf(stderr,"\n");
}

/* matches the rgb histogram and returns a score for the match */
static float _similarity_match_histogram_rgb(const dt_similarity_histogram_t *target, const dt_similarity_histogram_t *source)
{
  float score=0;
  
  for(int k=0;k<DT_SIMILARITY_HISTOGRAM_BUCKETS;k++)
    for(int j=0;j<3;j++)
      score += (1.0 - fabs(target->rgbl[k][j] - source->rgbl[k][j]))/3.0;
    
  score/=DT_SIMILARITY_HISTOGRAM_BUCKETS;
  
  return score;
}

/* scoring match of lightmap */
static float _similarity_match_lightmap(const dt_similarity_lightmap_t *target, const dt_similarity_lightmap_t *source, int channel)
{
  float score=0;
  
  for(int j=0;j<(DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);j++) 
    score += 1.0-fabs((float)(target->pixels[4*j+channel] - source->pixels[4*j+channel])/0xff);
  
  score /= (DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);
  
  /* scale score for sensitivity */
  score *=1.15;
  
  return CLIP(score);
}

void dt_similarity_match_image(uint32_t imgid,dt_similarity_t *data)
{
  sqlite3_stmt *stmt;
  gboolean all_ok_for_match = TRUE;
  dt_similarity_histogram_t orginal_histogram,test_histogram;
  dt_similarity_lightmap_t orginal_lightmap,test_lightmap;
 
  /* create temporary mem table for matches */
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "create temporary table if not exists similar_images (id integer,score real)", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from similar_images", NULL, NULL, NULL);
  
  /* 
   * get the histogram and lightmap data for image to match against 
   */
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select histogram,lightmap from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* get the histogram data */
    uint32_t size = sqlite3_column_bytes(stmt,0);
    if (size!=sizeof(dt_similarity_histogram_t)) 
    {
      all_ok_for_match = FALSE;
      dt_control_log(_("this image has not been indexed yet."));
    } 
    else 
      memcpy(&orginal_histogram, sqlite3_column_blob(stmt, 0), sizeof(dt_similarity_histogram_t));

    /* get the lightmap data */
    size = sqlite3_column_bytes(stmt,1);
    if (size!=sizeof(dt_similarity_lightmap_t)) 
    {
      all_ok_for_match = FALSE;
      dt_control_log(_("this image has not been indexed yet."));
    } 
    else 
      memcpy(&orginal_lightmap, sqlite3_column_blob(stmt, 1), sizeof(dt_similarity_lightmap_t));
    
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  
  
  /*
   * if all ok lets begin matching
   */
  if (all_ok_for_match) 
  {
    char query[4096]={0};
    
    /* set an extended collection query for viewing the result of match */
    dt_collection_set_extended_where(darktable.collection, "id in (select id from similar_images order by score)");
    dt_collection_set_query_flags( darktable.collection,
        dt_collection_get_query_flags(darktable.collection) | COLLECTION_QUERY_USE_ONLY_WHERE_EXT);
    dt_collection_update(darktable.collection);
    
    /* add target image with 1.0 in score into result */
    sprintf(query,"insert into similar_images(id,score) values(%d,%f)",imgid,1.0);
    DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
    dt_control_queue_draw_all();
    
    /* loop thru images and generate score table */
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select id,histogram,lightmap from images", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      float score_histogram=0, score_lightmap=0;
      
      /* verify size of histogram abnd lightmap blob of test image */
      if ( 
                  (sqlite3_column_bytes(stmt,1) == sizeof(dt_similarity_histogram_t)) && 
                  (sqlite3_column_bytes(stmt,2) == sizeof(dt_similarity_lightmap_t))
      )
      {
        /*
         * Get the histogram blob and calculate the similarity score
         */
        memcpy(&test_histogram, sqlite3_column_blob(stmt, 1), sizeof(dt_similarity_histogram_t));
        score_histogram = _similarity_match_histogram_rgb(&orginal_histogram,&test_histogram);
         
        /*
         * Get the lightmap blob and calculate the similarity score
         *  1.08 is a tuned constant that works well with threshold
         */
        memcpy(&test_lightmap, sqlite3_column_blob(stmt, 2), sizeof(dt_similarity_lightmap_t));
        score_lightmap = _similarity_match_lightmap(&orginal_lightmap,&test_lightmap,3);
       
      }
     

      /* 
       * calculate the similarity score
       */
      float total_weight = data->histogram_weight+data->lightmap_weight;
      float weighted_score = ((score_histogram*data->histogram_weight) + (score_lightmap*data->lightmap_weight)) / total_weight;
      float score = weighted_score;
      fprintf(stderr,"Image score %f\n",weighted_score);
      
      /* 
       * If current images scored, lets add it to similar_images table 
       */
      if(score >= 0.96)
      {
        sprintf(query,"insert into similar_images(id,score) values(%d,%f)",sqlite3_column_int(stmt, 0),score);
        DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
        
        /* new image added, lets redraw the view */
        dt_control_queue_draw_all();
      }
    }
  }
  sqlite3_finalize (stmt);  
}

void dt_similarity_image_dirty(uint32_t imgid)
{
  dt_similarity_histogram_dirty(imgid);
  dt_similarity_lightmap_dirty(imgid);
}

void dt_similarity_histogram_dirty(uint32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set histogram = NULL where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}

void dt_similarity_histogram_store(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set histogram =?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, histogram,sizeof(dt_similarity_histogram_t),SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
  _similarity_dump_histogram(imgid,histogram);
}

void dt_similarity_lightmap_store(uint32_t imgid, const dt_similarity_lightmap_t *lightmap)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set lightmap =?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, lightmap,sizeof(dt_similarity_lightmap_t),SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}

void dt_similarity_lightmap_dirty(uint32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set lightmap = NULL where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}