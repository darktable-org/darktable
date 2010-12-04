/*
    This file is part of darktable,
    copyright (c) 2010 Tobias Ellinghaus.

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

#include "utility.h"

guint dt_util_str_occurence(const gchar *haystack,const gchar *needle){
	guint o=0;
	const gchar *p=haystack;
	if( (p=g_strstr_len(p,strlen(p),needle)) != NULL){
		do{
			o++;
		}while((p=g_strstr_len((p+1),strlen(p+1),needle)) != NULL);
	}
	return o;
}

gchar* dt_util_str_escape(const gchar* string, const gchar* pattern, const gchar* substitute){
	gint occurences = dt_util_str_occurence(string, pattern);
	gchar* nstring;
	if(occurences){
		nstring=g_malloc(strlen(string)+(occurences*strlen(substitute))+1);
		const gchar *pend=string+strlen(string);
		const gchar *s = string, *p = string;
		gchar *np = nstring;
		if((s=g_strstr_len(s,strlen(s),pattern)) != NULL){
			do{
				memcpy(np,p,s-p);
				np+=(s-p);
				memcpy(np,substitute,strlen(substitute));
				np+=strlen(substitute);
				p=s+strlen(pattern);
			}while((s=g_strstr_len((s+1),strlen(s+1),pattern)) != NULL);
		}
		memcpy(np,p,pend-p);
		np[pend-p]='\0';
	} else
		nstring = g_strdup(string); // otherwise it's a hell to decide whether to free this string later.
	return nstring;
}
