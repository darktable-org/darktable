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

#ifdef _DEBUG
static void _similarity_dump_histogram(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  fprintf(stderr, "histogram for %d:",imgid);
  for(int j=0;j<DT_SIMILARITY_HISTOGRAM_BUCKETS;j++)
    fprintf(stderr," [%f, %f, %f, %f]",histogram->rgbl[j][0],histogram->rgbl[j][1],histogram->rgbl[j][2],histogram->rgbl[j][3]);
  fprintf(stderr,"\n");
}
#endif

/* matches the rgb histogram and returns a score for the match  */
static float _similarity_match_histogram_rgb(dt_similarity_t *data, const dt_similarity_histogram_t *target, const dt_similarity_histogram_t *source)
{
  float score=0;
  
  for(int k=0;k<DT_SIMILARITY_HISTOGRAM_BUCKETS;k++)
    for(int j=0;j<3;j++)
      score += fabs(target->rgbl[k][j] - source->rgbl[k][j])/3.0;
    
  score/=DT_SIMILARITY_HISTOGRAM_BUCKETS;
  
  return 1.0 - score;
}

/* scoring match of lightmap */
static float _similarity_match_lightmap(dt_similarity_t *data, const dt_similarity_lightmap_t *target, const dt_similarity_lightmap_t *source)
{
  float score=0;
  int channel = 3;	
  
  /* sum up the score */
  for(int j=0;j<(DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);j++) 
    score += fabs((float)(target->pixels[4*j+channel] - source->pixels[4*j+channel]) / 0xff);
  
  /* scale down score */
  score /= (DT_SIMILARITY_LIGHTMAP_SIZE * DT_SIMILARITY_LIGHTMAP_SIZE);
  
   return 1.0 - score;
}

/* scoring match of colormap */
static float _similarity_match_colormap(dt_similarity_t *data, const dt_similarity_lightmap_t *target, const dt_similarity_lightmap_t *source)
{
  float redscore = 0;
  float greenscore = 0;
  float bluescore = 0;
  float score = 0;

  /* sum up the score */
  for (int j=0;j<(DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);j++) 
  {
    redscore    +=  fabs((float)(target->pixels[4*j+0] - source->pixels[4*j+0]) / 0xff);
    greenscore  += fabs((float)(target->pixels[4*j+1] - source->pixels[4*j+1]) / 0xff);
    bluescore   += fabs((float)(target->pixels[4*j+2] - source->pixels[4*j+2]) / 0xff);
  }
  
  /* scale down score */
  redscore    /= (DT_SIMILARITY_LIGHTMAP_SIZE * DT_SIMILARITY_LIGHTMAP_SIZE);
  greenscore  /= (DT_SIMILARITY_LIGHTMAP_SIZE * DT_SIMILARITY_LIGHTMAP_SIZE);
  bluescore   /= (DT_SIMILARITY_LIGHTMAP_SIZE * DT_SIMILARITY_LIGHTMAP_SIZE);
  
  //  fprintf(stderr,"weight: %f\n",data->redmap_weight+data->bluemap_weight+data->greenmap_weight);
  score = ((redscore*data->redmap_weight) + (greenscore * data->greenmap_weight) + (bluescore*data->bluemap_weight)) / 3.0;

  /* now we have each score for r,g and b channel, lets weight them and calculate 
      a main score for the match. */
  // fprintf(stderr,"Color score %f,%f,%f total weighted score = %f\n",redscore,greenscore,bluescore,score);  
  
  return 1.0 - score;
}

void dt_similarity_match_image(uint32_t imgid,dt_similarity_t *data)
{
  sqlite3_stmt *stmt;
  gboolean all_ok_for_match = TRUE;
  dt_similarity_histogram_t orginal_histogram,test_histogram;
  dt_similarity_lightmap_t orginal_lightmap,test_lightmap;
 
  /* create temporary mem table for matches */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "create temporary table if not exists similar_images (id integer,score real)", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from similar_images", NULL, NULL, NULL);
  
  /* 
   * get the histogram and lightmap data for image to match against 
   */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select histogram,lightmap from images where id = ?1", -1, &stmt, NULL);
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
  else
  {
    all_ok_for_match = FALSE;
    dt_control_log(_("this image has not been indexed yet."));
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  
  
  /*
   * if all ok lets begin matching
   */
  if (all_ok_for_match) 
  {
    char query[4096]={0};

    /* add target image with 100.0 in score into result to ensure it always shown in top */
    sprintf(query,"insert into similar_images(id,score) values(%d,%f)",imgid,100.0);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
    

    /* set an extended collection query for viewing the result of match */
    dt_collection_set_extended_where(darktable.collection, ", similar_images where images.id = similar_images.id order by similar_images.score desc");
    dt_collection_set_query_flags( darktable.collection, 
            dt_collection_get_query_flags(darktable.collection) | COLLECTION_QUERY_USE_ONLY_WHERE_EXT);
    dt_collection_update(darktable.collection);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);  

        
    /* loop thru images and generate score table */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id,histogram,lightmap from images", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      float score_histogram=0, score_lightmap=0, score_colormap=0;
      
      /* verify size of histogram and lightmap blob of test image */
      if ( 
                  (sqlite3_column_bytes(stmt,1) == sizeof(dt_similarity_histogram_t)) && 
                  (sqlite3_column_bytes(stmt,2) == sizeof(dt_similarity_lightmap_t))
      )
      {
        /*
         * Get the histogram blob and calculate the similarity score
         */
        memcpy(&test_histogram, sqlite3_column_blob(stmt, 1), sizeof(dt_similarity_histogram_t));
        score_histogram = _similarity_match_histogram_rgb(data, &orginal_histogram, &test_histogram);
         
        /*
         * Get the lightmap blob and calculate the similarity score
         *  1.08 is a tuned constant that works well with threshold
         */
        memcpy(&test_lightmap, sqlite3_column_blob(stmt, 2), sizeof(dt_similarity_lightmap_t));
        score_lightmap = _similarity_match_lightmap(data, &orginal_lightmap, &test_lightmap);
        
        /*
         * then calculate the colormap similarity score
         */
        score_colormap = _similarity_match_colormap(data, &orginal_lightmap, &test_lightmap);
       
        
        /* 
         * calculate the similarity score
         */
	float score =  (pow(score_histogram, data->histogram_weight) *
			     pow(score_lightmap, data->lightmap_weight) *
			     pow(score_colormap, data->redmap_weight));
       
        // fprintf(stderr,"score: %f, histo: %f, light: %f, color: %f\n",score,score_histogram,score_lightmap,score_colormap);

        
        /* 
         * If current images scored, lets add it to similar_images table 
         */
        if(score >= 0.92)
        {
          sprintf(query,"insert into similar_images(id,score) values(%d,%f)",sqlite3_column_int(stmt, 0),score);
          DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
                    
          /* lets redraw the view */
          dt_control_queue_redraw_center();
        }
      } else
        fprintf(stderr,"Image has inconsisten similarity matching data..\n");
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set histogram = NULL where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}

void dt_similarity_histogram_store(uint32_t imgid, const dt_similarity_histogram_t *histogram)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set histogram =?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, histogram,sizeof(dt_similarity_histogram_t),SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
#ifdef _DEBUG
  _similarity_dump_histogram(imgid,histogram);
#endif
}

void dt_similarity_lightmap_store(uint32_t imgid, const dt_similarity_lightmap_t *lightmap)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set lightmap =?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, lightmap,sizeof(dt_similarity_lightmap_t),SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}

void dt_similarity_lightmap_dirty(uint32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set lightmap = NULL where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);
}

// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
