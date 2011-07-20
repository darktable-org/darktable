/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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

#include "dtgtk/button.h"
#include "dtgtk/label.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/pwstorage/pwstorage.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

DT_MODULE(1)

typedef struct dt_storage_picasa_gui_data_t
{

  GtkLabel *label1,*label2,*label3, *label4,*label5,*label6,*label7;                           // username, password, albums, status, albumtitle, albumsummary, albumrights
  GtkEntry *entry1,*entry2,*entry3,*entry4;                          // username, password, albumtitle,albumsummary
  GtkComboBox *comboBox1;                                 // album box
  GtkCheckButton *checkButton1,*checkButton2;                         // public album, export tags
  GtkDarktableButton *dtbutton1;                        // refresh albums
  GtkBox *hbox1;                                                    // Create album options...

  /** Current picasa context for the gui */
  struct _picasa_api_context_t *picasa_api;

} dt_storage_picasa_gui_data_t;


typedef struct dt_storage_picasa_params_t
{
  int64_t hash;
  struct _picasa_api_context_t *picasa_api;
  gboolean export_tags;
} dt_storage_picasa_params_t;

/** Authenticate against google picasa service*/
typedef struct _buffer_t
{
  char *data;
  size_t size;
  size_t offset;
} _buffer_t;

typedef struct _picasa_api_context_t
{
  /** Handle to initialized curl context. */
  CURL *curl_handle;
  /** Headers to pass on to HTTP requests. */
  struct curl_slist *curl_headers;
  gboolean needsReauthentication;
  gchar *authHeader;  // Google Auth HTTP header string

  /** A list with _picasa_album_t objects from parsed XML */
  GList *albums;

  /** Current album used when posting images... */
  struct _picasa_album_t  *current_album;

  char *album_title;
  char *album_summary;
  int album_public;

} _picasa_api_context_t;

/** Info representing an album */
typedef struct _picasa_album_t
{
  char *id;
  char *title;
  char *summary;
  char *rights;
  char *photoCount;
} _picasa_album_t;

/** Authenticates and retreives an initialized picasa api object */
static _picasa_api_context_t *_picasa_api_authenticate(const char *username,const char *password);

static int _picasa_api_get_feed(_picasa_api_context_t *ctx);
static int _picasa_api_create_album(_picasa_api_context_t *ctx);
static int _picasa_api_upload_photo( _picasa_api_context_t *ctx, char *mime , char *data, int size , char *caption, char *description,GList * tags );

/** Grow and fill _buffer_t with recieved data... */
static size_t _picasa_api_buffer_write_func(void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer=(_buffer_t *)stream;
  char *newdata=g_malloc(buffer->size+nmemb+1);
  memset(newdata,0, buffer->size+nmemb+1);
  if( buffer->data != NULL ) memcpy(newdata, buffer->data, buffer->size);
  memcpy(newdata+buffer->size, ptr, nmemb);
  g_free( buffer->data );
  buffer->data = newdata;
  buffer->size += nmemb;
  return nmemb;
}

static size_t _picasa_api_buffer_read_func( void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer=(_buffer_t *)stream;
  size_t dsize=0;
  if( (buffer->size - buffer->offset) > nmemb )
    dsize=nmemb;
  else
    dsize=(buffer->size - buffer->offset);

  memcpy(ptr,buffer->data+buffer->offset,dsize);
  buffer->offset+=dsize;
  return dsize;
}

static void _picasa_api_free( _picasa_api_context_t *ctx )
{

  g_free( ctx->album_title );
  g_free( ctx->album_summary );

  /// \todo free list of albums...
  g_free( ctx );
}


static _picasa_api_context_t *_picasa_api_authenticate(const char *username,const char *password)
{
  if(username[0] == '\0' || password[0] == '\0') return NULL;
  _picasa_api_context_t *ctx = (_picasa_api_context_t *)g_malloc(sizeof(_picasa_api_context_t));
  memset(ctx,0,sizeof(_picasa_api_context_t));
  ctx->curl_handle = curl_easy_init();

  // Setup the auth curl request
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));
  char data[4096]= {0};
  g_strlcat(data,"accountType=HOSTED_OR_GOOGLE&Email=",4096);
  g_strlcat(data,username,4096);
  g_strlcat(data,"&Passwd=",4096);
  g_strlcat(data,password,4096);
  g_strlcat(data,"&service=lh2&source="PACKAGE_NAME"-"PACKAGE_VERSION,4096);

#ifdef _DEBUG
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
#else
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0);
#endif
  curl_easy_setopt(ctx->curl_handle, CURLOPT_FOLLOWLOCATION, 1);

  curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, "https://www.google.com/accounts/ClientLogin");
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);
  curl_easy_perform( ctx->curl_handle );

  long result=1;
  curl_easy_getinfo(ctx->curl_handle,CURLINFO_RESPONSE_CODE,&result );
  if( result == 200 )   // All good lets get auth key from response buffer...
  {
    char *pa=strstr(buffer.data,"Auth=");
    pa+=5;

    // Add auth to curl context header
    gchar *end=g_strrstr(pa,"\n");
    end[0]='\0';
    char auth[4096]= {0};
    g_strlcat(auth,"Authorization: GoogleLogin auth=",4096);
    g_strlcat(auth,pa,4096);
    ctx->authHeader=g_strdup(auth);
    ctx->curl_headers = curl_slist_append(ctx->curl_headers,auth);
    curl_easy_setopt(ctx->curl_handle,CURLOPT_HTTPHEADER, ctx->curl_headers);
    g_free(buffer.data);
    return ctx;
  }
  g_free(buffer.data);
  g_free(ctx);
  return NULL;
}


static int _picasa_api_upload_photo( _picasa_api_context_t *ctx, char *mime , char *data, int size , char *caption, char *description,GList * tags )
{
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));
  char uri[4096]= {0};

  gchar *entry = g_markup_printf_escaped (
                   "<entry xmlns='http://www.w3.org/2005/Atom'>"
                   "<title>%s</title>"
                   "<summary>%s</summary>"
                   "<category scheme=\"http://schemas.google.com/g/2005#kind\""
                   " term=\"http://schemas.google.com/photos/2007#photo\"/>"
                   "</entry>",
                   caption,description);


  // Hack for nonform multipart post...
  gchar mpart1[4096]= {0};
  gchar *mpart_format="Media multipart posting\n--END_OF_PART\nContent-Type: application/atom+xml\n\n%s\n--END_OF_PART\nContent-Type: %s\n\n";
  sprintf(mpart1,mpart_format,entry,mime);

  int mpart1size=strlen(mpart1);
  int postdata_length=mpart1size+size+strlen("\n--END_OF_PART--");
  gchar *postdata=g_malloc(postdata_length);
  memcpy( postdata, mpart1, mpart1size);
  memcpy( postdata+mpart1size, data, size);
  memcpy( postdata+mpart1size+size, "\n--END_OF_PART--",strlen("\n--END_OF_PART--") );

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers,ctx->authHeader);
  headers = curl_slist_append(headers,"Content-Type: multipart/related; boundary=\"END_OF_PART\"");
  headers = curl_slist_append(headers,"MIME-version: 1.0");
  headers = curl_slist_append(headers,"Expect:");
  headers = curl_slist_append(headers,"GData-Version: 2");

  sprintf(uri,"http://picasaweb.google.com/data/feed/api/user/default/albumid/%s", ctx->current_album->id);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, uri);
#ifdef _DEBUG
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
#else
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0);
#endif
  curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_UPLOAD,0);   // A post request !
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POST,1);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, postdata);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDSIZE, postdata_length);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);

  curl_easy_perform( ctx->curl_handle );

  curl_slist_free_all(headers);

  long result;
  curl_easy_getinfo(ctx->curl_handle,CURLINFO_RESPONSE_CODE,&result );

  // If we want to add tags let's do...
  if( result == 201 && g_list_length(tags) > 0 )
  {
    // Image was created , fine.. and result have the fully created photo xml entry..
    // Let's perform an update of the photos keywords with tags passed along to this function..
    // and use picasa photo update api to add keywords to the photo...

    // Build the keywords content string
    gchar keywords[4096]= {0};
    for( int i=0; i<g_list_length( tags ); i++)
    {
      g_strlcat(keywords,((dt_tag_t *)g_list_nth_data(tags,i))->tag,4096);
      if( i < g_list_length( tags )-1)
        g_strlcat(keywords,", ",4096);
    }

    xmlDocPtr doc;
    xmlNodePtr entryNode;
    // Parse xml document
    if( ( doc = xmlParseDoc( (xmlChar *)buffer.data ))==NULL) return 0;
    entryNode = xmlDocGetRootElement(doc);
    if(  !xmlStrcmp(entryNode->name, (const xmlChar *) "entry") )
    {
      // Let's get the gd:etag attribute of entry...
      // For now, we force update using "If-Match: *"
      /*
        if( !xmlHasProp(entryNode, (const xmlChar*)"gd:etag") ) return 0;
        xmlChar *etag = xmlGetProp(entryNode,(const xmlChar*)"gd:etag");
      */

      gchar *photo_id=NULL;
      gchar *updateUri=NULL;
      xmlNodePtr entryChilds = entryNode->xmlChildrenNode;
      if( entryChilds != NULL )
      {
        do
        {
          if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")) )
          {
            // Get the photo id used in uri for update
            xmlChar *id= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if( xmlStrncmp( id, (const xmlChar *)"http://",7) )
              photo_id = g_strdup((const char *)id);
            xmlFree(id);
          }
          else  if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"group")) )
          {
            // Got the media:group entry lets find the child media:keywords
            xmlNodePtr mediaChilds = entryChilds->xmlChildrenNode;
            if(mediaChilds != NULL)
            {
              do
              {
                // Got the keywords tag, let's set the tags
                if ((!xmlStrcmp(mediaChilds->name, (const xmlChar *)"keywords")) )
                  xmlNodeSetContent(mediaChilds, (xmlChar *)keywords);
              }
              while( (mediaChilds = mediaChilds->next)!=NULL );
            }
          }
          else if (( !xmlStrcmp(entryChilds->name,(const xmlChar*)"link")) )
          {
            xmlChar *rel = xmlGetProp(entryChilds,(const xmlChar*)"rel");
            if( !xmlStrcmp(rel,(const xmlChar *)"edit") )
            {
              updateUri=(char *)xmlGetProp(entryChilds,(const xmlChar*)"href");
            }
            xmlFree(rel);
          }
        }
        while( (entryChilds = entryChilds->next)!=NULL );
      }

      // Let's update the photo
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers,ctx->authHeader);
      headers = curl_slist_append(headers,"Content-Type: application/atom+xml");
      headers = curl_slist_append(headers,"If-Match: *");
      headers = curl_slist_append(headers,"Expect:");
      headers = curl_slist_append(headers,"GData-Version: 2");

      _buffer_t response;
      memset(&response,0,sizeof(_buffer_t));

      // Setup data to send..
      _buffer_t writebuffer;
      int datasize;
      xmlDocDumpMemory(doc,(xmlChar **)&writebuffer.data, &datasize);
      writebuffer.size = datasize;
      writebuffer.offset=0;

      curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, updateUri);
#ifdef _DEBUG
      curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
#else
      curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0);
#endif
      curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(ctx->curl_handle, CURLOPT_UPLOAD,1);   // A put request
      curl_easy_setopt(ctx->curl_handle, CURLOPT_READDATA,&writebuffer);
      curl_easy_setopt(ctx->curl_handle, CURLOPT_INFILESIZE,writebuffer.size);
      curl_easy_setopt(ctx->curl_handle, CURLOPT_READFUNCTION,_picasa_api_buffer_read_func);
      curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
      curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &response);
      curl_easy_perform( ctx->curl_handle );

      xmlFree( updateUri );
      xmlFree( writebuffer.data );
      if (response.data != NULL)
        g_free(response.data);
      if (photo_id != NULL)
        g_free(photo_id); // FIXME: never used!

      curl_slist_free_all( headers );
    }

  }
  return result;
}


static int _picasa_api_create_album(_picasa_api_context_t *ctx )
{
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));

  gchar *data = g_markup_printf_escaped ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                         "<entry xmlns='http://www.w3.org/2005/Atom'"
                                         " xmlns:media='http://search.yahoo.com/mrss/'"
                                         " xmlns:gphoto='http://schemas.google.com/photos/2007'>"
                                         "<title type='text'>%s</title>"
                                         "<summary type='text'>%s</summary>"
                                         "<gphoto:access>%s</gphoto:access>" // public
                                         "<gphoto:timestamp>%d000</gphoto:timestamp>"
                                         "<media:group>"
                                         "<media:keywords></media:keywords>"
                                         "</media:group>"
                                         "<category scheme='http://schemas.google.com/g/2005#kind'"
                                         "  term='http://schemas.google.com/photos/2007#album'></category>"
                                         "</entry>",ctx->album_title, (ctx->album_summary)?ctx->album_summary:"", (ctx->album_public)?"public":"private", ((int)time(NULL)) );

  ctx->curl_headers = curl_slist_append(ctx->curl_headers,"Content-Type: application/atom+xml");
  curl_easy_setopt(ctx->curl_handle,CURLOPT_HTTPHEADER, ctx->curl_headers);

#ifdef _DEBUG
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
#else
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0);
#endif
  curl_easy_setopt(ctx->curl_handle, CURLOPT_HEADER , 0);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, "http://picasaweb.google.com/data/feed/api/user/default");
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);
  curl_easy_perform( ctx->curl_handle );

  long result;
  curl_easy_getinfo(ctx->curl_handle,CURLINFO_RESPONSE_CODE,&result );
  if( result == 201 )
  {
    xmlDocPtr doc;
    xmlNodePtr entryNode;

    // Parse xml document
    if( ( doc = xmlParseDoc( (xmlChar *)buffer.data ))==NULL) return 0;
    // Let's parse album entry response and construct a _picasa_album_t and assign to current album...
    entryNode = xmlDocGetRootElement(doc);
    if(  xmlStrcmp(entryNode->name, (const xmlChar *) "entry")==0 )
    {
      xmlNodePtr entryChilds = entryNode->xmlChildrenNode;
      if( entryChilds != NULL )
      {
        // Allocate current_album...
        ctx->current_album =  g_malloc(sizeof(_picasa_album_t));
        memset( ctx->current_album,0,sizeof(_picasa_album_t));
        // Parse xml
        do
        {
          if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")) )
          {
            xmlChar *id= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if( xmlStrncmp( id, (const xmlChar *)"http://",7) )
              ctx->current_album->id = g_strdup((const char *)id);
            xmlFree(id);
          }
          else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"title")) )
          {
            xmlChar *title= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            ctx->current_album->title = g_strdup((const char *)title);
            xmlFree(title);
          }
          else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"summary")) )
          {
            xmlChar *summary= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if( summary )
              ctx->current_album->summary = g_strdup((const char *)summary);
            xmlFree(summary);
          }
          else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"rights")) )
          {
            xmlChar *rights= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            ctx->current_album->rights= g_strdup((const char *)rights);
            xmlFree(rights);
          }
          else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"numphotos")) )
          {
            xmlChar *photos= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            ctx->current_album->photoCount = g_strdup((const char *)photos);
            xmlFree(photos);
          }
        }
        while( (entryChilds = entryChilds->next)!=NULL );
      }
    }
    else
      return 0;

    return 201;
  }
  return 0;
}

static int _picasa_api_get_feed(_picasa_api_context_t *ctx)
{
  _buffer_t buffer;
  memset(&buffer,0,sizeof(_buffer_t));
#ifdef _DEBUG
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 1);
#else
  curl_easy_setopt(ctx->curl_handle, CURLOPT_VERBOSE, 0);
#endif
  curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, "http://picasaweb.google.com/data/feed/api/user/default");
  curl_easy_setopt(ctx->curl_handle, CURLOPT_POST, 0);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);
  curl_easy_perform( ctx->curl_handle );

  long result;
  curl_easy_getinfo(ctx->curl_handle,CURLINFO_RESPONSE_CODE,&result );
  if( result == 200 )
  {
    // setup xml parsing of result...
    xmlInitParser();
    xmlDocPtr doc;
    xmlNodePtr feedNode;

    // Parse xml document
    if( ( doc = xmlParseDoc( (xmlChar *)buffer.data ))==NULL)
    {
      return 0;
    }
    // Remove old album objects from list..
    if( ctx->albums != NULL )
    {
      while( g_list_length(ctx->albums) > 0 )
      {
        gpointer data=g_list_nth_data(ctx->albums, 0);
        ctx->albums = g_list_remove( ctx->albums,data);
        g_free( data );
      }
    }

    // Let's parse atom feed of albums...
    feedNode = xmlDocGetRootElement(doc);
    if(  xmlStrcmp(feedNode->name, (const xmlChar *) "feed")==0 )
    {
      xmlNodePtr children = feedNode->xmlChildrenNode;
      if( children != NULL)
      {
        do
        {
          // Let's take care of entry node
          if ((!xmlStrcmp(children->name, (const xmlChar *)"entry")) )
          {
            // we want <id/> <title/> <summary/> <rights/> nodes from entry
            xmlNodePtr entryChilds = children->xmlChildrenNode;
            if( entryChilds != NULL )
            {
              // Got an album entry let's create new _picasa_album_t object and parse node info into object
              // then add it to the list of albums available...
              _picasa_album_t *album = g_malloc(sizeof(_picasa_album_t));
              memset(album,0,sizeof(_picasa_album_t));
              do
              {

                if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")) )
                {
                  xmlChar *id= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
                  if( xmlStrncmp( id, (const xmlChar *)"http://",7) )
                  {
                    album->id = g_strdup((const char *)id);
                  }
                  xmlFree(id);

                }
                else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"title")) )
                {
                  xmlChar *title= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
                  album->title = g_strdup((const char *)title);
                  xmlFree(title);
                }
                else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"summary")) )
                {
                  xmlChar *summary= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
                  if( summary )
                    album->summary = g_strdup((const char *)summary);
                  xmlFree(summary);
                }
                else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"rights")) )
                {
                  xmlChar *rights= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
                  album->rights= g_strdup((const char *)rights);
                  xmlFree(rights);
                }
                else if ((!xmlStrcmp(entryChilds->name, (const xmlChar *)"numphotos")) )
                {
                  xmlChar *photos= xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
                  album->photoCount = g_strdup((const char *)photos);
                  xmlFree(photos);
                }
              }
              while( ( entryChilds = entryChilds->next ) != NULL);

              // Entry node parsed add album to list
              ctx->albums = g_list_append(ctx->albums,album);
            }
          }
        }
        while( (children = children->next)!=NULL );
      }
    }

    return result;
  }
  return 0;
}


const char*
name ()
{
  return _("picasa webalbum");
}


static void entry_changed(GtkEntry *entry, gpointer data)
{
  dt_storage_picasa_gui_data_t *ui=(dt_storage_picasa_gui_data_t *)data;

  if( ui->picasa_api != NULL)
    ui->picasa_api->needsReauthentication=TRUE;
}


/** Set status connection text */
static void set_status(dt_storage_picasa_gui_data_t *ui, gchar *message,gchar *color)
{
  if( !color ) color="#ffffff";
  gchar mup[512]= {0};
  sprintf( mup,"<span foreground=\"%s\" ><small>%s</small></span>",color,message);
  gtk_label_set_markup(ui->label4, mup);
}

/** Refresh albums */
static void refresh_albums(dt_storage_picasa_gui_data_t *ui)
{
  gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), FALSE);

  if( ui->picasa_api == NULL || ui->picasa_api->needsReauthentication == TRUE )
  {
    if( ui->picasa_api ) _picasa_api_free( ui->picasa_api);
    ui->picasa_api = _picasa_api_authenticate(gtk_entry_get_text(ui->entry1),gtk_entry_get_text(ui->entry2));
  }

  // First clear the model of data except first item (Create new album)
  GtkTreeModel *model=gtk_combo_box_get_model(ui->comboBox1);
  gtk_list_store_clear (GTK_LIST_STORE(model));

  if( ui->picasa_api )
  {
    set_status(ui,_("authenticated"), "#7fe07f");

    /* Add creds to pwstorage */
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);
    gchar* username = g_strdup(gtk_entry_get_text(ui->entry1));
    gchar* password = g_strdup(gtk_entry_get_text(ui->entry2));

    g_hash_table_insert(table, "username", username);
    g_hash_table_insert(table, "password", password);

    if( !dt_pwstorage_set("picasa", table) )
    {
      dt_print(DT_DEBUG_PWSTORAGE,"[picasa] cannot store username/password\n");
    }

    g_free(username);
    g_free(password);
    g_hash_table_destroy(table);

    if( _picasa_api_get_feed(ui->picasa_api) == 200)
    {

      // Add standard action
      gtk_combo_box_append_text( ui->comboBox1, _("create new album") );
      gtk_combo_box_append_text( ui->comboBox1, "" );// Separator

      // Then add albums from list...
      GList *album=g_list_first(ui->picasa_api->albums);
      if( album != NULL )
      {
        for(int i=0; i<g_list_length(ui->picasa_api->albums); i++)
        {
          char data[512]= {0};
          sprintf(data,"%s (%s)", ((_picasa_album_t*)g_list_nth_data(ui->picasa_api->albums,i))->title, ((_picasa_album_t*)g_list_nth_data(ui->picasa_api->albums,i))->photoCount );
          gtk_combo_box_append_text( ui->comboBox1, g_strdup(data));
        }
        gtk_combo_box_set_active( ui->comboBox1, 2);
        gtk_widget_hide( GTK_WIDGET(ui->hbox1) ); // Hide create album box...

      }
      else
        gtk_combo_box_set_active( ui->comboBox1, 0);
    }
    else
    {
      // Failed to parse feed of album...
      // Lets notify somehow...
    }
    gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), TRUE);
  }
  else
  {
    // Failed to authenticated, let's notify somehow...
    set_status(ui,_("authentication failed"),"#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET( ui->comboBox1 ) ,FALSE);
  }


}

static void album_changed(GtkComboBox *cb,gpointer data)
{
  dt_storage_picasa_gui_data_t * ui=(dt_storage_picasa_gui_data_t *)data;
  gchar *value=gtk_combo_box_get_active_text(ui->comboBox1);
  if( value!=NULL && strcmp( value, _("create new album") ) == 0 )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox1), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox1));
  }
  else
    gtk_widget_hide(GTK_WIDGET(ui->hbox1));
}

static gboolean combobox_separator(GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
  GValue value = { 0, };
  gtk_tree_model_get_value(model,iter,0,&value);
  gchar *v=NULL;
  if (G_VALUE_HOLDS_STRING (&value))
  {
    if( (v=(gchar *)g_value_get_string (&value))!=NULL && strlen(v) == 0 ) return TRUE;
  }
  return FALSE;
}

// Refresh button pressed...
static void button1_clicked(GtkButton *button,gpointer data)
{
  dt_storage_picasa_gui_data_t * ui=(dt_storage_picasa_gui_data_t *)data;
  refresh_albums(ui);
}

/*
static gboolean
focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  dt_control_tab_shortcut_off(darktable.control);
  return FALSE;
}

static gboolean
focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  dt_control_tab_shortcut_on(darktable.control);
  return FALSE;
}
*/

void
gui_init (dt_imageio_module_storage_t *self)
{
  self->gui_data = (dt_storage_picasa_gui_data_t *)malloc(sizeof(dt_storage_picasa_gui_data_t));
  memset(self->gui_data,0,sizeof(dt_storage_picasa_gui_data_t));
  dt_storage_picasa_gui_data_t *ui= self->gui_data;
  self->widget = gtk_vbox_new(TRUE, 0);

  GtkWidget *hbox1=gtk_hbox_new(FALSE,5);
  GtkWidget *vbox1=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox2=gtk_vbox_new(FALSE,0);

  ui->label1 = GTK_LABEL(  gtk_label_new( _("user") ) );
  ui->label2 = GTK_LABEL(  gtk_label_new( _("password") ) );
  ui->label3 = GTK_LABEL(  gtk_label_new( _("albums") ) );
  ui->label4 = GTK_LABEL(  gtk_label_new( NULL ) );
  ui->label5 = GTK_LABEL(  gtk_label_new( _("title") ) );
  ui->label6 = GTK_LABEL(  gtk_label_new( _("summary") ) );
  ui->label7 = GTK_LABEL(  gtk_label_new( _("rights") ) );
  gtk_misc_set_alignment(GTK_MISC(ui->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label6), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label7), 0.0, 0.5);

  ui->entry1 = GTK_ENTRY( gtk_entry_new() );
  ui->entry2 = GTK_ENTRY( gtk_entry_new() );
  ui->entry3 = GTK_ENTRY( gtk_entry_new() );  // Album title
  ui->entry4 = GTK_ENTRY( gtk_entry_new() );  // Album summary

  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry1));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry2));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry3));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry4));

  /*
    gtk_widget_add_events(GTK_WIDGET(ui->entry1), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry1), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry1), "focus-out-event", G_CALLBACK(focus_out), NULL);

    gtk_widget_add_events(GTK_WIDGET(ui->entry2), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry2), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry2), "focus-out-event", G_CALLBACK(focus_out), NULL);
    gtk_widget_add_events(GTK_WIDGET(ui->entry3), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry3), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry3), "focus-out-event", G_CALLBACK(focus_out), NULL);
    gtk_widget_add_events(GTK_WIDGET(ui->entry4), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry4), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry4), "focus-out-event", G_CALLBACK(focus_out), NULL);
  */
  GHashTable* table = dt_pwstorage_get("picasa");
  gchar* _username = g_strdup( g_hash_table_lookup(table, "username"));
  gchar* _password = g_strdup( g_hash_table_lookup(table, "password"));
  g_hash_table_destroy(table);
  gtk_entry_set_text( ui->entry1,  _username == NULL?"":_username );
  gtk_entry_set_text( ui->entry2,  _password == NULL?"":_password );
  gtk_entry_set_text( ui->entry3, _("my new album") );
  gtk_entry_set_text( ui->entry4, _("exported from darktable") );

  gtk_entry_set_visibility(ui->entry2, FALSE);

  GtkWidget *albumlist=gtk_hbox_new(FALSE,0);
  ui->comboBox1=GTK_COMBO_BOX( gtk_combo_box_new_text()); // Available albums

  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(ui->comboBox1));
  GList *it = renderers;
  while(it)
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (char *)NULL);
    it = g_list_next(it);
  }
  g_list_free(renderers);

  ui->dtbutton1 = DTGTK_BUTTON( dtgtk_button_new(dtgtk_cairo_paint_refresh,0) );
  g_object_set(G_OBJECT(ui->dtbutton1), "tooltip-text", _("refresh album list"), (char *)NULL);
  gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox1,combobox_separator,ui->comboBox1,NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->dtbutton1), FALSE, FALSE, 0);

  ui->checkButton1 = GTK_CHECK_BUTTON( gtk_check_button_new_with_label(_("public album")) );
  ui->checkButton2 = GTK_CHECK_BUTTON( gtk_check_button_new_with_label(_("export tags")) );
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON( ui->checkButton2 ),TRUE);
  // Auth
  gtk_box_pack_start(GTK_BOX(hbox1), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), vbox2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label1 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label2 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( gtk_label_new("")), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( gtk_label_new("")), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label3 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry1 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry2 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->label4 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->checkButton2 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( albumlist ), TRUE, FALSE, 0);


  // Create Album
  ui->hbox1=GTK_BOX(gtk_hbox_new(FALSE,5));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox1), TRUE);
  vbox1=gtk_vbox_new(FALSE,0);
  vbox2=gtk_vbox_new(FALSE,0);

  gtk_box_pack_start(GTK_BOX(ui->hbox1), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox1), vbox2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->hbox1), TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label5 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label6 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label7 ), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry3 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry4 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->checkButton1 ), TRUE, FALSE, 0);


  // Setup signals
  // add signal on realize and hide gtk_widget_hide(GTK_WIDGET(ui->hbox1));

  g_signal_connect(G_OBJECT(ui->dtbutton1), "clicked", G_CALLBACK(button1_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->entry1), "changed", G_CALLBACK(entry_changed), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->entry2), "changed", G_CALLBACK(entry_changed), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox1), "changed", G_CALLBACK(album_changed), (gpointer)ui);

  // If username and password is stored, let's populate the combo
//   if( _username && _password )
//     refresh_albums(ui);
  if( _username )
    g_free( _username );
  if( _password )
    g_free( _password );

  gtk_combo_box_set_active( ui->comboBox1, 0);
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  int result=1;
  dt_storage_picasa_params_t *p=(dt_storage_picasa_params_t *)sdata;

  int fail = 0;
#ifdef _OPENMP // synch parallel store
  #pragma omp critical
#endif
  if( p->picasa_api->current_album == NULL )
    if( _picasa_api_create_album( p->picasa_api ) != 201 )
    {
      dt_control_log("failed to create picasa album");
      fail = 1;
    }

  if(fail) return 1;

  const char *ext = format->extension(fdata);

  // Let's upload image...

  /* construct a temporary file name */
  char fname[4096]= {0};
  dt_get_user_local_dir (fname,4096);
  g_strlcat (fname,"/tmp",4096);
  g_mkdir_with_parents(fname,0700);
  g_strlcat (fname,"/darktable.XXXXXX.",4096);
  g_strlcat(fname,ext,4096);

  char *caption="a image";
  char *description="";
  char *mime="image/jpeg";
  GList *tags=NULL;

  // Fetch the attached tags of image id if exported..
  if( p->export_tags == TRUE )
    dt_tag_get_attached(imgid,&tags);

  // Ok, maybe a dt_imageio_export_to_buffer would suit here !?
  gint fd=g_mkstemp(fname);
  fprintf(stderr,"tempfile: %s\n",fname);
  if(fd==-1)
  {
    dt_control_log("failed to create temporary image for picasa export");
    return 1;
  }
  close(fd);
  dt_image_t *img = dt_image_cache_get(imgid, 'r');
  caption = g_path_get_basename( img->filename );

  (g_strrstr(caption,"."))[0]='\0'; // Shop extension...

  GList *desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
  if(desc != NULL)
  {
    description = desc->data;
  }

  dt_imageio_export(img, fname, format, fdata);
  dt_image_cache_release(img, 'r');

  // Open the temp file and read image to memory
  GMappedFile *imgfile = g_mapped_file_new(fname,FALSE,NULL);
  int size = g_mapped_file_get_length( imgfile );
  gchar *data =g_mapped_file_get_contents( imgfile );

#ifdef _OPENMP
  #pragma omp critical
#endif
  // Upload image to picasa
  if( _picasa_api_upload_photo( p->picasa_api, mime , data, size , caption, description, tags ) == 201 )
    result=0;

  // Unreference the memorymapped file...
  g_mapped_file_unref( imgfile );

  // And remove from filesystem..
  unlink( fname );
  g_free( caption );
  if(desc)
  {
    g_free(desc->data);
    g_list_free(desc);
  }

  dt_control_log(_("%d/%d exported to picasa webalbum"), num, total );
  return result;
}

void*
get_params(dt_imageio_module_storage_t *self, int *size)
{
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  // TODO: if a hash to encrypted data is stored here, return only this size and store it at the beginning of the struct!
  *size = sizeof(int64_t);
  dt_storage_picasa_gui_data_t *ui =(dt_storage_picasa_gui_data_t *)self->gui_data;
  dt_storage_picasa_params_t *d = (dt_storage_picasa_params_t *)malloc(sizeof(dt_storage_picasa_params_t));
  memset(d,0,sizeof(dt_storage_picasa_params_t));
  d->hash = 1;

  // fill d from controls in ui
  if( ui->picasa_api )
  {
    // We are authenticated and off to actually export images..
    d->picasa_api = ui->picasa_api;
    int index = gtk_combo_box_get_active(ui->comboBox1);
    if( index >= 0 )
    {
      if( index == 0 )   // Create new album
      {
        d->picasa_api->current_album = NULL;
        d->picasa_api->album_title = g_strdup( gtk_entry_get_text( ui->entry3 ) );
        d->picasa_api->album_summary = g_strdup( gtk_entry_get_text( ui->entry4) );
        d->picasa_api->album_public = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON( ui->checkButton1 ) );
      }
      else
      {
        // use existing album
        if( (d->picasa_api->current_album = g_list_nth_data(d->picasa_api->albums,(index-2)))==NULL )
        {
          // Something went wrong...
          fprintf(stderr,"Something went wrong.. album index %d = NULL\n",index-2 );
          return NULL;
        }
      }
    }
    else return NULL;

    d->export_tags = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->checkButton2));

    // Let UI forget about this api context and recreate a new one for further usage...
    ui->picasa_api = _picasa_api_authenticate(gtk_entry_get_text(ui->entry1), gtk_entry_get_text(ui->entry2));
  }
  else
    return NULL;

  return d;
}

int
set_params(dt_imageio_module_format_t *self, void *params, int size)
{
  if(size != sizeof(int64_t)) return 1;
  // gui stuff not updated, as sensitive user data is not stored in the preset.
  // TODO: store name/hash in kwallet/etc module and get encrypted stuff from there!
  return 0;
}

int supported(struct dt_imageio_module_storage_t *storage, struct dt_imageio_module_format_t *format)
{
  if( strcmp(format->mime(NULL) ,"image/jpeg") ==  0 ) return 1;
  else if( strcmp(format->mime(NULL) ,"image/png") ==  0 ) return 1;

  return 0;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  dt_storage_picasa_params_t *d = (dt_storage_picasa_params_t *)params;

  _picasa_api_free(  d->picasa_api );

  free(params);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
