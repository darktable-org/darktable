/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#define cpuid(func,ax,bx,cx,dx) __asm__ __volatile__ ("cpuid":"=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (func));
int main (int argc, char **argv)
{
	int eax,ebx,ecx,edx;
	cpuid(0x1,eax,ebx,ecx,edx);
	fprintf(stdout,"%s", (edx>>25)&1?"SSE ":"");
	fprintf(stdout,"%s", (edx>>26)&1?"SSE2 ":"");
	fprintf(stdout,"%s", (ecx)&1?"SSE3 ":"");
	fprintf(stdout,"%s", (ecx>>19)&1?"SSE4.1 ":"");
	fprintf(stdout,"%s", (ecx>>20)&1?"SSE4.2 ":"");
	fprintf(stdout,"\n");
} 
