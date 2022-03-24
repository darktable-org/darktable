#!/bin/python

import re
import sys
from enum import Enum

CLANG_OFF='// clang-format off\n'
NOTIFICATION_LINE='// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh\n'
VIM_MODELINE='// vim: shiftwidth=2 expandtab tabstop=2 cindent\n'
KATE_MODELINE='// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;\n'
CLANG_ON='// clang-format on\n'

class estate(Enum):
	none=1
	start=2
	inside=3
	end=4

class modelines_updater_t:
	def __init__(self, filename):
		self.filename = filename
		self.state = estate.none
		self.begin = -1
		self.end = -1
		self.lines = []
		
	def load_lines(self):
		with open(self.filename,'r') as file:
			self.lines = file.readlines()
		
	def process_line(self, line, lineno):
		if(self.state == estate.none):
			if(line.startswith(CLANG_OFF)):
				self.state=estate.start
			return
		elif(self.state == estate.start):
			if(line.startswith('// modelines:')):
				self.state=estate.inside
				self.begin = lineno-1
			else:
				self.state=estate.none
			return
		elif(self.state == estate.inside):
			if(line.startswith(CLANG_ON)):
				self.state=estate.end
				self.end = lineno
			return
		elif(self.state == estate.end):
			if(not line.strip()):
				self.end = lineno
			else:
				self.state=estate.none
			return
		
	def remove_lines(self):
		with open(sys.argv[1],'w') as file:
			for lineno,line in enumerate(self.lines):
				if((lineno >= self.begin) and (lineno <= self.end)):
					continue
				if(line.startswith('// modelines')):
					continue
				if(line.startswith('// vim')):
					continue
				if(line.startswith('// kate')):
					continue
				file.write(line)
				
	def write_file(self):
		with open(self.filename,'a') as file:
			file.write(CLANG_OFF)
			file.write(NOTIFICATION_LINE)
			file.write(VIM_MODELINE)
			file.write(KATE_MODELINE)
			file.write(CLANG_ON)
			file.write('\n')
			
	def update(self):
		print('parsing {}'.format(self.filename));
		self.load_lines()
		for lineno,line in enumerate(self.lines):
			self.process_line(line, lineno)
		if((self.begin == -1) != (self.end == -1)):
			raise RuntimeError("parsing error")
		print('removing old modelines')
		self.remove_lines()
		print('writing file')
		self.write_file()
			


if __name__ == "__main__":
	updater = modelines_updater_t(sys.argv[1])
	updater.update()

