/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2010--2011 henrik andersson.

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
#include "common/collection.h"
#include "common/selection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"

#include <glib.h>
#include <glib/gstdio.h>

DT_MODULE(1)

const char*
name ()
{
  return _("gps data");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_gps_t
{
  GtkWidget
  *attach_gps_data_button, *status_gps_data_label;
} dt_lib_gps_t;

typedef enum
{
  GPX_TAG_STATE_START,
  GPX_TAG_STATE_END,
} GPXParserState;

typedef struct
{
  GMarkupParseContext *context;
  GPXParserState state;
  GList *list;
} GPXData;

typedef struct
{
  gdouble lon;
  gdouble lat;
  GDateTime *time;
} GPXRecord;

static void
gpx_start_element (GMarkupParseContext  *context,
		   const gchar          *element_name,
		   const gchar         **attribute_names,
		   const gchar         **attribute_values,
		   gpointer              user_data,
		   GError              **error)
{
  GPXData *data = user_data;
  GPXRecord *item;
  
  if (strcmp(element_name,"trkpt") == 0) {

    item = (GPXRecord *)malloc(sizeof(GPXRecord));
    
    for (int i = 0; attribute_names[i] != NULL; i++)
      {
	// Longitude and lattitude
	switch(i){
	case 0:
	  item->lat = g_ascii_strtod(attribute_values[i],NULL);
	  break;
	case 1:
	  item->lon = g_ascii_strtod(attribute_values[i],NULL);
	  break;
	default:
	  continue;
	}
      }
    data->list = g_list_append(data->list, item);
  }
  if(strcmp(element_name,"time") == 0) {
    data->state = GPX_TAG_STATE_START;
  }
}

static void
gpx_end_element (GMarkupParseContext *context,
		 const gchar         *element_name,
		 gpointer             user_data,
		 GError             **error)
{
  GPXData *data = user_data;
  
  if(strcmp(element_name,"time") == 0) {
    data->state = GPX_TAG_STATE_END;
  }
}

static void
gpx_text (GMarkupParseContext *context,
                          const gchar         *text,
                          gsize                text_len,
                          gpointer             user_data,
                          GError             **error)
{
  GPXData *data = user_data;
  GPXRecord *item;
  gint year;
  gint month;
  gint day;
  gint hour;
  gint minute;
  gdouble seconds;
  
  if(data->state == GPX_TAG_STATE_START) {
    GList *last = g_list_last(data->list);
    item = last->data;
    
    // Parse GPX time string
    sscanf(text,"%i-%i-%iT%i:%i:%iZ", 
	   (int*)&year,(int*)&month,(int*)&day,(int*)&hour,(int*)&minute,(int*)&seconds);
    // Put in GDateTime structure
    item->time = g_date_time_new_utc(year,month,day,hour,minute,seconds);
  }
}

static void
gpx_error (GMarkupParseContext *context,
	   GError              *error,
	   gpointer             user_data)
{
  GPXData *data = user_data;

  g_slice_free (GPXData, data);
}

static GMarkupParser gpx_parser =
{
  gpx_start_element,
  gpx_end_element,
  gpx_text,
  NULL,
  gpx_error
};

void db_fill_gps(gchar *time, gdouble lon, gdouble lat) {
  sqlite3_stmt *stmt;
  
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set longitude = ?1, latitude = ?2 where datetime_taken = ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, lat);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, time, strlen(time), SQLITE_STATIC);

  sqlite3_step (stmt);
  
  sqlite3_finalize (stmt);
}

void dt_gpx_attach_to_images(int32_t imgid, GList *list)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select datetime_taken from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  gint year;
  gint month;
  gint day;
  gint hour;
  gint minute;
  gdouble seconds;
  
  while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      gchar *datetime = (gchar*)sqlite3_column_text(stmt, 0);
      
      sscanf(datetime,"%i:%i:%i %i:%i:%i", 
	     (int*)&year,(int*)&month,(int*)&day,(int*)&hour,(int*)&minute,(int*)&seconds);
      
      GDateTime *taken = g_date_time_new_utc(year,month,day,hour,minute,seconds);
      GDateTime *gpxTime = NULL;
      GPXRecord *item = NULL;

      for(GList *elem = list; elem; elem = elem->next) 
      {
	item = elem->data;
	gpxTime = item->time;
	GTimeSpan span = g_date_time_difference(gpxTime,taken);
	// Find nearest GPS data point (GPS data are ordered)
	if(span > 0) { // Span is possitive if gpxTime is lager
	  break;
	}
      }
      
      // Add  gps location to database
      db_fill_gps(datetime, item->lon, item->lat);
    }
  sqlite3_finalize (stmt);
}

void on_selected_images(GList *list) {
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int imgid = sqlite3_column_int(stmt, 0);
      dt_gpx_attach_to_images(imgid, list); 
    }
  sqlite3_finalize(stmt);
}


static void
button_clicked(GtkWidget *widget, gpointer user_data)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new ("open gpx file",
							GTK_WINDOW (win),
							GTK_FILE_CHOOSER_ACTION_OPEN,
							GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
							GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
							(char *)NULL);
  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.gpx");
  gtk_file_filter_set_name(filter, _("GPS Data Exchange Format"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      GError *error;
      gchar *gpx_filename;
      gpx_filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
      
      // Map file into memory
      GMappedFile *file_mapped = g_mapped_file_new(gpx_filename, FALSE, &error);
      gchar *file_content =  g_mapped_file_get_contents(file_mapped);
      gint  file_length   =  g_mapped_file_get_length(file_mapped);

      // Read GPS data
      GPXData *gpx_data;
      gpx_data = g_slice_new0(GPXData);
      gpx_data->state = GPX_TAG_STATE_END;
      gpx_data->context = g_markup_parse_context_new (&gpx_parser, 0, gpx_data, NULL);
      g_markup_parse_context_parse(gpx_data->context, file_content,file_length,&error);

      // Pair
      on_selected_images(gpx_data->list);
      
      // Cleanup
      g_markup_parse_context_free (gpx_data->context);     // Free GMarkup structure
      g_list_foreach(gpx_data->list, (GFunc)g_free, NULL); // Free list nodes data
      g_list_free(gpx_data->list);                         // Free list nodes
      g_slice_free (GPXData, gpx_data);                    // Free GPXData structure
      g_free (gpx_filename);                               // Free file path
    }
  
  gtk_widget_destroy (filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

int
position ()
{
  return 800;
}


void
gui_init (dt_lib_module_t *self)
{
  dt_lib_gps_t *d = (dt_lib_gps_t*)malloc(sizeof(dt_lib_gps_t));
  self->data = d;
  self->widget = gtk_vbox_new(TRUE,1);
  
  GtkBox *hbox;
  GtkWidget *button;
  GtkWidget *label;
  hbox = GTK_BOX(gtk_hbox_new(TRUE,1));
  
  button = gtk_button_new_with_label("Use GPX file");
  label  = gtk_label_new("Attaches GPS tags");
  
  d->attach_gps_data_button = button;
  d->status_gps_data_label = label;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)1);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, label, TRUE,TRUE, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  g_free(self->data);
}

void init_key_accels(dt_lib_module_t *self)
{
  
}

void connect_key_accels(dt_lib_module_t *self)
{
  
}
