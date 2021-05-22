;  This file is part of darktable,
;    copyright (c) 2009--2010 henrik andersson.
;
;   darktable is free software: you can redistribute it and/or modify
;    it under the terms of the GNU General Public License as published by
;    the Free Software Foundation, either version 3 of the License, or
;    (at your option) any later version.
;
;    darktable is distributed in the hope that it will be useful,
;    but WITHOUT ANY WARRANTY; without even the implied warranty of
;    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;    GNU General Public License for more details.
;
;    You should have received a copy of the GNU General Public License
;    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
		
			extern printf
			section .data
sse_text:		db "SSE",0
sse2_text:		db "SSE2",0
sse3_text:		db "SSE3",0
ssse3_text:	db "SSSE3",0
sse4_1_text:	db "SSE4.1",0
sse4_2_text:	db "SSE4.2",0
		
			section .text
			global main
main:
			mov eax, 1			; Call cpuid with function number 1
			cpuid
			push ebp				; setup stackframe for _printf call
			mov ebp, esp
			
			bt edx,20				; test sse4_2
			push sse4_2_text
			je print
			bt edx,19				; test sse4_1
			push sse4_1_text
			je print
			bt ecx,9				; test ssse3
			push ssse3_text
			je print
			bt ecx,0				; test sse3
			push sse3_text
			je print
			bt edx,26				; test sse2
			push sse2_text
			je print
			bt edx,25				; test sse
			push sse_text
			je print
noflags:
			mov esp, ebp			; cleanout stackframe
			pop ebp
			mov eax, 1			; return 1
			jmp quit
print:
			call printf
			add esp, 4			; pop stack 4 bytes
			mov esp, ebp			; cleanout stackframe
			pop ebp
			mov eax, 0			; return 0
quit:
			ret
		


