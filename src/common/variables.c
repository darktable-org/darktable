/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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
#include <stdio.h>
#include <string.h>
#include "common/variables.h"

typedef struct dt_variables_data_t 
{	
  gchar *source;
 
  /** expanded result string */
  gchar *result;
  time_t time;
  guint sequence;
}
dt_variables_data_t;

/** Find occurence of string*/
guint _string_occurence(const gchar *haystack,const gchar *needle);
/** search and replace, returns new allocated string */
gchar *_string_substitute(gchar *string,const gchar *search,const gchar *replace);

guint _string_occurence(const gchar *haystack,const gchar *needle) 
{
  guint o=0;
  const gchar *p=haystack;
  if( (p=g_strstr_len(p,strlen(p),needle)) != NULL) 
  {
    do
    {
      o++;
    } while((p=g_strstr_len((p+1),strlen(p+1),needle)) != NULL);
  }
  return o;
}

gchar *_string_substitute(gchar *string,const gchar *search,const gchar *replace) 
{
  guint occurences = _string_occurence(string,search);
  if( occurences )
  {
    gint sl=-(strlen(search)-strlen(replace));
    gchar *pend=string+strlen(string);
    gchar *nstring=g_malloc(strlen(string)+(sl*occurences)+1);
    gchar *np=nstring;
    gchar *s=string,*p=string;
    //fprintf(stderr,"replace %s with %s strdiff %d, occurences %d, oldstring %d, newstring %d\n",search,replace,sl,occurences,strlen(string),strlen(string)+(sl*occurences)+1);
    if( (s=g_strstr_len(s,strlen(s),search)) != NULL) 
    {
      do
      {
        memcpy(np,p,s-p);
        np+=(s-p);
        memcpy(np,replace,strlen(replace));
        np+=strlen(replace);
        p=s+strlen(search);
      } while((s=g_strstr_len((s+1),strlen(s+1),search)) != NULL);
    }
    memcpy(np,p,pend-p);
    np[pend-p]='\0';
    string=nstring;
  } 
  return string;
}

gchar *_string_get_first_variable(gchar *string,gchar *variable)
{
   gchar *pend=string+strlen(string);
   gchar *p,*e;
   p=e=string;
  while( p < pend && e < pend) 
  {
    while( *p!='$' && *(p+1)!='(' && p<pend) p++;
    if( *p=='$' && *(p+1)=='(' )
    {
      e=p;
      while( *e!=')' && e < pend) e++;
      if(e < pend && *e==')')
      {
        strncpy(variable,p,e-p+1);
        variable[e-p+1]='\0';
        return p+1;
      }
      else
        return NULL;
    }
    p++;
  }
  return p+1;
}

gchar *_string_get_next_variable(gchar *string,gchar *variable)
{
   gchar *pend=string+strlen(string);
   gchar *p,*e;
   p=e=string;
  while( p < pend && e < pend) 
  {
    while( !(*p=='$' && *(p+1)=='(') && p<=pend) p++;
    if( *p=='$' && *(p+1)=='(' )
    {
      e=p;
      while( *e!=')' && e < pend) e++;
      if(e < pend && *e==')')
      {
        strncpy(variable,p,e-p+1);
        variable[e-p+1]='\0';
        return p+1;
      }
      else
        return NULL;
    }
   
  }
  return NULL;
}

gboolean _variable_get_value(dt_variables_params_t *params, gchar *variable,gchar *value)
{
  const gchar *file_ext=NULL;
  gboolean got_value=FALSE;
  struct tm *tim=localtime(&params->data->time);
  
  const gchar *homedir=g_getenv("HOME");
  if( !homedir ) 
    homedir=g_get_home_dir();
  
  gchar *pictures_folder=NULL;
  
  if(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) == NULL)
    pictures_folder=g_build_path(G_DIR_SEPARATOR_S,homedir,"Pictures",NULL);
  else 
    pictures_folder=g_strdup( g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) );
  
  if(params->filename) file_ext=(g_strrstr(params->filename,".")+1);
  
  if( g_strcmp0(variable,"$(YEAR)") == 0 && (got_value=TRUE) )  sprintf(value,"%.4d",tim->tm_year+1900);
  else if( g_strcmp0(variable,"$(MONTH)") == 0&& (got_value=TRUE)  )   sprintf(value,"%.2d",tim->tm_mon+1);
  else if( g_strcmp0(variable,"$(DAY)") == 0 && (got_value=TRUE) )   sprintf(value,"%.2d",tim->tm_mday);
  else if( g_strcmp0(variable,"$(HOUR)") == 0 && (got_value=TRUE) )  sprintf(value,"%.2d",tim->tm_hour);
  else if( g_strcmp0(variable,"$(MINUTE)") == 0 && (got_value=TRUE) )   sprintf(value,"%.2d",tim->tm_min);
  else if( g_strcmp0(variable,"$(SECOND)") == 0 && (got_value=TRUE) )   sprintf(value,"%.2d",tim->tm_sec);
  else if( g_strcmp0(variable,"$(JOBCODE)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",params->jobcode);
  else if( g_strcmp0(variable,"$(FILE_NAME)") == 0 && params->filename && (got_value=TRUE) )   sprintf(value,"%s",params->filename);
  else if( g_strcmp0(variable,"$(FILE_EXTENSION)") == 0 && params->filename && (got_value=TRUE) )   sprintf(value,"%s",file_ext);
  else if( g_strcmp0(variable,"$(SEQUENCE)") == 0 && (got_value=TRUE) )   sprintf(value,"%.4d",params->data->sequence);
  else if( g_strcmp0(variable,"$(USERNAME)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",g_get_user_name());
  else if( g_strcmp0(variable,"$(HOME_FOLDER)") == 0 && (got_value=TRUE)  )    sprintf(value,"%s",homedir);
  else if( g_strcmp0(variable,"$(PICTURES_FOLDER)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",pictures_folder);
  else if( g_strcmp0(variable,"$(DESKTOP_FOLDER)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));
  
  g_free(pictures_folder);
  
  return got_value;
}

void dt_variables_params_init(dt_variables_params_t **params)
{
  *params=g_malloc(sizeof(dt_variables_params_t));
  memset(*params ,0,sizeof(dt_variables_params_t));
  (*params)->data = g_malloc(sizeof(dt_variables_data_t));
  memset((*params)->data ,0,sizeof(dt_variables_data_t));
  (*params)->data->time=time(NULL);
}

void dt_variables_params_destroy(dt_variables_params_t *params)
{
  g_free(params->data);
  g_free(params);
}

const gchar *dt_variables_get_result(dt_variables_params_t *params) {
  return params->data->result;
}

gboolean dt_variables_expand(dt_variables_params_t *params, gchar *string, gboolean iterate)
{
  gchar *variable=g_malloc(128);
  gchar *value=g_malloc(1024);
  gchar *token=NULL;

  // Increase data..
  if( iterate ) 
    params->data->sequence++;
  
  // Lets expand string
  gchar *result=NULL;
  params->data->result=params->data->source=string;
  if( (token=_string_get_first_variable(params->data->source,variable)) != NULL)
  {
    do {
      //fprintf(stderr,"var: %s\n",variable);
      if( _variable_get_value(params,variable,value) )
      {
        //fprintf(stderr,"Substitute variable '%s' with value '%s'\n",variable,value);
        if( (result=_string_substitute(params->data->result,variable,value)) != params->data->result && result != params->data->source)
        { // we got a result 
          if( params->data->result != params->data->source)
            g_free(params->data->result);
          params->data->result=result;
        }
      }
    } while( (token=_string_get_next_variable(token,variable)) !=NULL );
  }
  return TRUE;
}