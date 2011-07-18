#!/usr/bin/env python

# import graphviz (python-pygraph python-pygraphviz)
import sys
sys.path.append('..')
sys.path.append('/usr/lib/graphviz/python/')
sys.path.append('/usr/lib64/graphviz/python/')

# libgv-python
import gv

# import pygraph
from pygraph.classes.digraph import digraph
from pygraph.algorithms.sorting import topological_sorting
from pygraph.algorithms.cycles import find_cycle
from pygraph.readwrite.dot import write
from pygraph.readwrite.dot import read

import fileinput
import sys
import os.path
import re

def replace_all(file,searchExp,replaceExp):
  for line in fileinput.input(file, inplace=1):
    # if searchExp in line:
    line = re.sub(searchExp, replaceExp, line)
    sys.stdout.write(line)

# in this function goes all our collected knowledge about the pipe and sort orders
# therein. please never reorder the pipe manually, but put your constraints in here:
def add_edges(gr):
  # basic frame of color flow:
  # output color profile:
  gr.add_edge(('gamma', 'colorout'))
  # std Lab:
  gr.add_edge(('colorout', 'colorin'))
  # camera input color profile:
  gr.add_edge(('colorin', 'demosaic'))
  
  # these work on mosaic data:
  gr.add_edge(('demosaic', 'rawspeed'))
  gr.add_edge(('demosaic', 'temperature'))
  gr.add_edge(('demosaic', 'stuckpixels'))
  gr.add_edge(('demosaic', 'hotpixels'))
  gr.add_edge(('demosaic', 'rawdenoise'))
  gr.add_edge(('demosaic', 'cacorrect'))
  
  # cacorrect works better on denoised data:
  gr.add_edge(('cacorrect', 'stuckpixels'))
  gr.add_edge(('cacorrect', 'hotpixels'))
  gr.add_edge(('cacorrect', 'rawdenoise'))
  
  # all these need white balanced input:
  gr.add_edge(('rawdenoise', 'temperature'))
  gr.add_edge(('stuckpixels', 'temperature'))
  gr.add_edge(('hotpixels', 'temperature'))
  gr.add_edge(('cacorrect', 'temperature'))
  
  # and of course rawspeed needs to give us the pixels first:
  gr.add_edge(('temperature', 'rawspeed'))
  
  # these need to be in camera color space (input rgb):
  gr.add_edge(('colorin', 'exposure'))
  gr.add_edge(('colorin', 'highlights'))
  gr.add_edge(('colorin', 'graduatednd'))
  gr.add_edge(('colorin', 'basecurve'))
  gr.add_edge(('colorin', 'lens'))
  gr.add_edge(('colorin', 'profile_gamma'))
  
  # handle highlights correctly:
  # we want highlights as early as possible, to avoid
  # pink highlights in plugins (happens only before highlight clipping)
  gr.add_edge(('highlights', 'demosaic'))
  gr.add_edge(('exposure', 'highlights'))
  gr.add_edge(('graduatednd', 'highlights'))
  gr.add_edge(('basecurve', 'highlights'))
  gr.add_edge(('lens', 'highlights'))
  gr.add_edge(('tonemap', 'highlights'))
  
  # this evil hack for nikon crap profiles needs to come
  # as late as possible before the input profile:
  gr.add_edge(('profile_gamma', 'exposure'))
  gr.add_edge(('profile_gamma', 'highlights'))
  gr.add_edge(('profile_gamma', 'graduatednd'))
  gr.add_edge(('profile_gamma', 'basecurve'))
  gr.add_edge(('profile_gamma', 'lens'))
  
  # these need Lab (between color in/out):
  gr.add_edge(('colorout', 'bloom'))
  gr.add_edge(('colorout', 'nlmeans'))
  gr.add_edge(('colorout', 'colortransfer'))
  gr.add_edge(('colorout', 'atrous'))
  gr.add_edge(('colorout', 'colorzones'))
  gr.add_edge(('colorout', 'lowlight'))
  gr.add_edge(('colorout', 'monochrome'))
  gr.add_edge(('colorout', 'zonesystem'))
  gr.add_edge(('colorout', 'tonecurve'))
  gr.add_edge(('colorout', 'relight'))
  gr.add_edge(('colorout', 'colorcorrection'))
  gr.add_edge(('colorout', 'sharpen'))
  gr.add_edge(('colorout', 'grain'))
  gr.add_edge(('colorout', 'anlfyeni'))
  gr.add_edge(('colorout', 'colorcontrast'))
  gr.add_edge(('bloom', 'colorin'))
  gr.add_edge(('nlmeans', 'colorin'))
  gr.add_edge(('colortransfer', 'colorin'))
  gr.add_edge(('atrous', 'colorin'))
  gr.add_edge(('colorzones', 'colorin'))
  gr.add_edge(('lowlight', 'colorin'))
  gr.add_edge(('monochrome', 'colorin'))
  gr.add_edge(('zonesystem', 'colorin'))
  gr.add_edge(('tonecurve', 'colorin'))
  gr.add_edge(('relight', 'colorin'))
  gr.add_edge(('colorcorrection', 'colorin'))
  gr.add_edge(('sharpen', 'colorin'))
  gr.add_edge(('grain', 'colorin'))
  gr.add_edge(('anlfyeni', 'colorin'))
  gr.add_edge(('highpass', 'colorin'))
  gr.add_edge(('colorcontrast', 'colorin'))
  
  # spot removal works on demosaiced data
  # and needs to be before geometric distortions:
  gr.add_edge(('spots', 'demosaic'))
  gr.add_edge(('lens', 'spots'))
  gr.add_edge(('borders', 'spots'))
  gr.add_edge(('clipping', 'spots'))
  
  # want to do powerful color magic before monochroming it:
  gr.add_edge(('monochrome', 'colorzones'))
  # want to change contrast in monochrome images:
  gr.add_edge(('zonesystem', 'monochrome'))
  gr.add_edge(('tonecurve', 'monochrome'))
  gr.add_edge(('relight', 'monochrome'))
  
  # want to splittone evenly, even when changing contrast:
  gr.add_edge(('colorcorrection', 'zonesystem'))
  gr.add_edge(('colorcorrection', 'tonecurve'))
  gr.add_edge(('colorcorrection', 'relight'))
  # want to split-tone monochrome images:
  gr.add_edge(('colorcorrection', 'monochrome'))
  
  # want to enhance detail/local contrast/sharpen denoised images:
  gr.add_edge(('atrous', 'nlmeans'))
  gr.add_edge(('sharpen', 'nlmeans'))
  gr.add_edge(('anlfyeni', 'nlmeans'))
  gr.add_edge(('highpass', 'nlmeans'))
  gr.add_edge(('zonesystem', 'nlmeans'))
  gr.add_edge(('tonecurve', 'nlmeans'))
  gr.add_edge(('relight', 'nlmeans'))
  gr.add_edge(('colorzones', 'nlmeans'))
  
  # don't sharpen grain:
  gr.add_edge(('grain', 'sharpen'))
  gr.add_edge(('grain', 'anlfyeni'))
  gr.add_edge(('grain', 'atrous'))
  gr.add_edge(('grain', 'highpass'))
  
  # output profile (sRGB) between gamma and colorout
  gr.add_edge(('gamma', 'channelmixer'))
  gr.add_edge(('gamma', 'clahe'))
  gr.add_edge(('gamma', 'velvia'))
  gr.add_edge(('gamma', 'soften'))
  gr.add_edge(('gamma', 'vignette'))
  gr.add_edge(('gamma', 'splittoning'))
  gr.add_edge(('gamma', 'watermark'))
  gr.add_edge(('gamma', 'overexposed'))
  gr.add_edge(('gamma', 'borders'))
  gr.add_edge(('channelmixer', 'colorout'))
  gr.add_edge(('clahe', 'colorout'))
  gr.add_edge(('velvia', 'colorout'))
  gr.add_edge(('soften', 'colorout'))
  gr.add_edge(('vignette', 'colorout'))
  gr.add_edge(('splittoning', 'colorout'))
  gr.add_edge(('watermark', 'colorout'))
  gr.add_edge(('overexposed', 'colorout'))
  
  # borders should not change shape/color:
  gr.add_edge(('borders', 'colorout'))
  gr.add_edge(('borders', 'watermark'))
  gr.add_edge(('borders', 'vignette'))
  gr.add_edge(('borders', 'splittoning'))
  gr.add_edge(('borders', 'velvia'))
  gr.add_edge(('borders', 'soften'))
  gr.add_edge(('borders', 'clahe'))
  gr.add_edge(('borders', 'channelmixer'))
  
  # want to sharpen after geometric transformations:
  gr.add_edge(('sharpen', 'clipping'))
  gr.add_edge(('sharpen', 'lens'))
  
  # don't bloom away sharpness:
  gr.add_edge(('sharpen', 'bloom'))
  
  # lensfun wants an uncropped buffer:
  gr.add_edge(('clipping', 'lens'))
  
  # want to splittone vignette and b/w
  gr.add_edge(('splittoning', 'vignette'))
  gr.add_edge(('splittoning', 'channelmixer'))
  
  # want to change exposure/basecurve after tone mapping
  gr.add_edge(('exposure', 'tonemap'))
  gr.add_edge(('basecurve', 'tonemap'))
  # need demosaiced data, but not Lab:
  gr.add_edge(('tonemap', 'demosaic'))
  gr.add_edge(('colorin', 'tonemap'))
  
  # want to fine-tune stuff after injection of color transfer:
  gr.add_edge(('atrous', 'colortransfer'))
  gr.add_edge(('colorzones', 'colortransfer'))
  gr.add_edge(('tonecurve', 'colortransfer'))
  gr.add_edge(('monochrome', 'colortransfer'))
  gr.add_edge(('zonesystem', 'colortransfer'))
  gr.add_edge(('colorcorrection', 'colortransfer'))
  gr.add_edge(('relight', 'colortransfer'))
  gr.add_edge(('highpass', 'colortransfer'))
  gr.add_edge(('anlfyeni', 'colortransfer'))
  gr.add_edge(('lowlight', 'colortransfer'))
  gr.add_edge(('bloom', 'colortransfer'))

  # deprecated:
  gr.add_edge(('colorout', 'bilateral'))
  gr.add_edge(('bilateral', 'colorin'))
  gr.add_edge(('colorout', 'equalizer'))
  gr.add_edge(('equalizer', 'colorin'))


gr = digraph()
gr.add_nodes([
'anlfyeni',
'atrous',
'basecurve',
'bilateral', # deprecated
'bloom',
'borders',
'cacorrect',
'channelmixer',
'clahe',
'clipping',
'colorcorrection',
'colorin',
'colorout',
'colortransfer',
'colorzones',
'colorcontrast',
'demosaic',
'equalizer', # deprecated
'exposure',
'gamma',
'graduatednd',
'grain',
'highlights',
'highpass',
'hotpixels',
'lens',
'lowlight',
'monochrome',
'nlmeans',
'overexposed',
'profile_gamma',
'rawdenoise',
'relight',
'sharpen',
'soften',
'splittoning',
'spots',
'stuckpixels',
'temperature',
'tonecurve',
'tonemap',
'velvia',
'vignette',
'watermark',
'zonesystem',
'rawspeed' ])

add_edges(gr)

# make sure we don't have cycles:
cycle_list = find_cycle(gr)
if cycle_list:
  print "cycles:"
  print cycle_list
  exit(1)

# get us some sort order!
sorted_nodes = topological_sorting(gr)
length=len(sorted_nodes)
priority=1000
for n in sorted_nodes:
  # now that should be the priority in the c file:
  print "%d %s"%(priority, n)
  filename="../src/iop/%s.c"%n
  if not os.path.isfile(filename):
    filename="../src/iop/%s.cc"%n
  if not os.path.isfile(filename):
    print "could not find file `%s', maybe you're not running inside tools/?"%filename
    continue
  replace_all(filename, "( )*?(module->priority)( )*?(=).*?(;).*\n", "  module->priority = %d; // module order created by iop_dependencies.py, do not edit!\n"%priority)
  priority -= 1000.0/(length-1.0)

# beauty-print the sorted pipe as pdf:
gr2 = digraph()
gr2.add_nodes(sorted_nodes)
add_edges(gr2)

dot = write(gr2)
gvv = gv.readstring(dot)
gv.layout(gvv,'dot')
gv.render(gvv,'pdf','iop_deps.pdf')
