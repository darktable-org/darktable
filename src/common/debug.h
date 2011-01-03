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

#ifndef __DEBUG_H__
#define __DEBUG_H__

#ifdef _DEBUG
	#include <assert.h>
	#define __DT_DEBUG_ASSERT__(x) assert(x == SQLITE_OK)
#else
// 	#define __DT_DEBUG_ASSERT__(x) x
	#define __DT_DEBUG_ASSERT__(x) \
		if(x != SQLITE_OK){ \
			fprintf(stderr, "sqlite3 error: %s\n", sqlite3_errmsg(darktable.db)); \
		} \

#endif

#define DT_DEBUG_SQLITE3_EXEC(a,b,c,d,e)       __DT_DEBUG_ASSERT__(sqlite3_exec(a,b,c,d,e))

#define DT_DEBUG_SQLITE3_PREPARE_V2(a,b,c,d,e) __DT_DEBUG_ASSERT__(sqlite3_prepare_v2(a,b,c,d,e))

#define DT_DEBUG_SQLITE3_BIND_INT(a,b,c)       __DT_DEBUG_ASSERT__(sqlite3_bind_int(a,b,c))
#define DT_DEBUG_SQLITE3_BIND_DOUBLE(a,b,c)    __DT_DEBUG_ASSERT__(sqlite3_bind_double(a,b,c))
#define DT_DEBUG_SQLITE3_BIND_TEXT(a,b,c,d,e)  __DT_DEBUG_ASSERT__(sqlite3_bind_text(a,b,c,d,e))
#define DT_DEBUG_SQLITE3_BIND_BLOB(a,b,c,d,e)  __DT_DEBUG_ASSERT__(sqlite3_bind_blob(a,b,c,d,e))

#endif
