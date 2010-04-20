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
    guint sl=(strlen(search)-strlen(replace));
    gchar *pend=string+strlen(string);
    gchar *nstring=g_malloc(strlen(string)+(sl*occurences));
    gchar *np=nstring;
    gchar *s=string,*p=string;
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
    fprintf(stderr,"%s\n",nstring);
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
   
  }
  return NULL;
}

gchar *_variable_get_value(dt_string_params_t *params, gchar *variable,gchar *value)
{
  gboolean got_value=FALSE;
  struct tm *tim=localtime(&params->time);
 
  if( g_strcmp0(variable,"$(YEAR)") == 0 && (got_value=TRUE) )  sprintf(value,"%.4d",tim->tm_year+1900);
  else if( g_strcmp0(variable,"$(MONTH)") == 0&& (got_value=TRUE)  )   sprintf(value,"%.2d",tim->tm_mon+1);
  else if( g_strcmp0(variable,"$(DAY)") == 0 && (got_value=TRUE) )   sprintf(value,"%.2d",tim->tm_mday);
  else if( g_strcmp0(variable,"$(HOUR)") == 0 && (got_value=TRUE) )  sprintf(value,"%.2d",tim->tm_hour);
  else if( g_strcmp0(variable,"$(MINUTE)") == 0 && (got_value=TRUE) )   sprintf(value,"%.2d",tim->tm_min);
  else if( g_strcmp0(variable,"$(JOBCODE)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",params->jobcode);
  else if( g_strcmp0(variable,"$(USERNAME)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",g_get_user_name());
  else if( g_strcmp0(variable,"$(HOME_FOLDER)") == 0 && (got_value=TRUE)  )    sprintf(value,"%s",g_get_home_dir());
  else if( g_strcmp0(variable,"$(PICTURES_FOLDER)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
  else if( g_strcmp0(variable,"$(DESKTOP_FOLDER)") == 0 && (got_value=TRUE) )   sprintf(value,"%s",g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));
  
  if(got_value==TRUE) return value;
  return NULL;
}

gboolean dt_variables_expand(dt_string_params_t *params)
{
  gchar *variable=g_malloc(128);
  gchar *value=g_malloc(1024);
  gchar *token;
  if(params->time==0)
    params->time=time(NULL);
  
  params->result=params->source;
  if( (token=_string_get_first_variable(params->source,variable)) != NULL)
  {
    do {
      if( (value=_variable_get_value(params,variable,value)) != NULL )
        params->result = _string_substitute(params->result,variable,value);
    } while( (token=_string_get_next_variable(token,variable)) !=NULL );
  }
  return TRUE;
}