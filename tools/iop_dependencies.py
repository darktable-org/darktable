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

  # these work on float mosaic data:
  gr.add_edge(('demosaic', 'letsgofloat'))
  # handle highlights correctly:
  # we want highlights as early as possible, to avoid
  # pink highlights in plugins (happens only before highlight clipping)
  gr.add_edge(('demosaic', 'highlights'))
  gr.add_edge(('demosaic', 'hotpixels'))
  gr.add_edge(('demosaic', 'rawdenoise'))
  gr.add_edge(('demosaic', 'cacorrect'))

  # highlights come directly after whitebalance
  gr.add_edge(('highlights', 'temperature'))
  
  # cacorrect works better on undenoised data:
  gr.add_edge(('hotpixels', 'cacorrect'))
  gr.add_edge(('rawdenoise', 'cacorrect'))
  
  # all these need white balanced and clipped input:
  gr.add_edge(('rawdenoise', 'highlights'))
  gr.add_edge(('hotpixels', 'highlights'))
  gr.add_edge(('cacorrect', 'highlights'))

  # we want float pixels:
  gr.add_edge(('temperature', 'letsgofloat'))

  # and of course rawspeed needs to give us the pixels first:
  gr.add_edge(('letsgofloat', 'rawspeed'))

  # inversion should be really early in the pipe
  gr.add_edge(('temperature', 'invert'))

  # but after uint16 -> float conversio
  gr.add_edge(('invert', 'letsgofloat'))

  # these need to be in camera color space (linear input rgb):
  gr.add_edge(('colorin', 'exposure'))
  gr.add_edge(('colorin', 'highlights'))
  gr.add_edge(('colorin', 'graduatednd'))
  gr.add_edge(('colorin', 'basecurve'))
  gr.add_edge(('colorin', 'lens'))
  gr.add_edge(('colorin', 'profile_gamma'))

  # very linear:
  gr.add_edge(('basecurve', 'lens'))
  gr.add_edge(('basecurve', 'exposure'))
  
  # fix mad sensor designs: NIKON D1X have rectangular pixels
  gr.add_edge(('scalepixels', 'demosaic'))
  # fix mad sensor designs: some Fuji have their Bayer pattern rotated by -45deg
  gr.add_edge(('rotatepixels', 'demosaic'))

  # there is no cameras that have non-square pixels AND rotated Bayer pattern
  # at the same time, but IMO it makes more sense to scale after rotating.
  gr.add_edge(('scalepixels', 'rotatepixels'))

  # flip is a distortion plugin, and as such has to go after spot removal
  # and lens correction, which depend on original input buffers.
  # and after buffer has been downscaled/demosaiced
  gr.add_edge(('flip', 'demosaic'))
  gr.add_edge(('flip', 'scalepixels'))
  gr.add_edge(('flip', 'rotatepixels'))
  gr.add_edge(('flip', 'lens'))
  gr.add_edge(('flip', 'spots'))
  # plus, it confuses crop/rotate, vignetting and graduated density
  gr.add_edge(('clipping', 'flip'))
  gr.add_edge(('graduatednd', 'flip'))
  gr.add_edge(('vignette', 'flip'))
  # gives the ability to change the space of shadow recovery fusion.
  # maybe this has to go the other way round, let's see what experience shows!
  
  # this evil hack for nikon crap profiles needs to come
  # as late as possible before the input profile:
  gr.add_edge(('profile_gamma', 'exposure'))
  gr.add_edge(('profile_gamma', 'highlights'))
  gr.add_edge(('profile_gamma', 'graduatednd'))
  gr.add_edge(('profile_gamma', 'basecurve'))
  gr.add_edge(('profile_gamma', 'lens'))
  gr.add_edge(('profile_gamma', 'bilateral'))
  gr.add_edge(('profile_gamma', 'denoiseprofile'))
  
  # these need Lab (between color in/out): 
  gr.add_edge(('colorout', 'bloom'))
  gr.add_edge(('colorout', 'nlmeans'))
  gr.add_edge(('colorout', 'colortransfer'))
  gr.add_edge(('colorout', 'colormapping'))
  gr.add_edge(('colorout', 'atrous'))
  gr.add_edge(('colorout', 'bilat'))
  gr.add_edge(('colorout', 'colorzones'))
  gr.add_edge(('colorout', 'lowlight'))
  gr.add_edge(('colorout', 'monochrome'))
  gr.add_edge(('colorout', 'zonesystem'))
  gr.add_edge(('colorout', 'tonecurve'))
  gr.add_edge(('colorout', 'levels'))
  gr.add_edge(('colorout', 'relight'))
  gr.add_edge(('colorout', 'colorcorrection'))
  gr.add_edge(('colorout', 'sharpen'))
  gr.add_edge(('colorout', 'grain'))
  gr.add_edge(('colorout', 'lowpass'))
  gr.add_edge(('colorout', 'shadhi'))
  gr.add_edge(('colorout', 'highpass'))
  gr.add_edge(('colorout', 'colorcontrast'))
  gr.add_edge(('colorout', 'colorize'))
  gr.add_edge(('colorout', 'colisa'))
  gr.add_edge(('colorout', 'defringe'))
  gr.add_edge(('bloom', 'colorin'))
  gr.add_edge(('nlmeans', 'colorin'))
  gr.add_edge(('colortransfer', 'colorin'))
  gr.add_edge(('colormapping', 'colorin'))
  gr.add_edge(('atrous', 'colorin'))
  gr.add_edge(('bilat', 'colorin'))
  gr.add_edge(('colorzones', 'colorin'))
  gr.add_edge(('lowlight', 'colorin'))
  gr.add_edge(('monochrome', 'colorin'))
  gr.add_edge(('zonesystem', 'colorin'))
  gr.add_edge(('tonecurve', 'colorin'))
  gr.add_edge(('levels', 'colorin'))
  gr.add_edge(('relight', 'colorin'))
  gr.add_edge(('colorcorrection', 'colorin'))
  gr.add_edge(('sharpen', 'colorin'))
  gr.add_edge(('grain', 'colorin'))
  gr.add_edge(('lowpass', 'colorin'))
  gr.add_edge(('shadhi', 'colorin'))
  gr.add_edge(('highpass', 'colorin'))
  gr.add_edge(('colorcontrast', 'colorin'))
  gr.add_edge(('colorize', 'colorin'))
  gr.add_edge(('colisa', 'colorin'))
  gr.add_edge(('defringe', 'colorin'))
  
  # spot removal works on demosaiced data
  # and needs to be before geometric distortions:
  gr.add_edge(('spots', 'demosaic'))
  gr.add_edge(('scalepixels', 'spots'))
  gr.add_edge(('rotatepixels', 'spots'))
  gr.add_edge(('lens', 'spots'))
  gr.add_edge(('borders', 'spots'))
  gr.add_edge(('clipping', 'spots'))
  
  # want to do powerful color magic before monochroming it:
  gr.add_edge(('monochrome', 'colorzones'))
  # want to change contrast in monochrome images:
  gr.add_edge(('zonesystem', 'monochrome'))
  gr.add_edge(('tonecurve', 'monochrome'))
  gr.add_edge(('levels', 'monochrome'))
  gr.add_edge(('relight', 'monochrome'))
  gr.add_edge(('colisa', 'monochrome'))
  
  # want to splittone evenly, even when changing contrast:
  gr.add_edge(('colorcorrection', 'zonesystem'))
  gr.add_edge(('colorcorrection', 'tonecurve'))
  gr.add_edge(('colorcorrection', 'levels'))
  gr.add_edge(('colorcorrection', 'relight'))
  # want to split-tone monochrome images:
  gr.add_edge(('colorcorrection', 'monochrome'))
  
  # want to enhance detail/local contrast/sharpen denoised images:
  gr.add_edge(('bilat', 'nlmeans'))
  gr.add_edge(('atrous', 'nlmeans'))
  gr.add_edge(('sharpen', 'nlmeans'))
  gr.add_edge(('lowpass', 'nlmeans'))
  gr.add_edge(('shadhi', 'nlmeans'))
  gr.add_edge(('highpass', 'nlmeans'))
  gr.add_edge(('zonesystem', 'nlmeans'))
  gr.add_edge(('tonecurve', 'nlmeans'))
  gr.add_edge(('levels', 'nlmeans'))
  gr.add_edge(('relight', 'nlmeans'))
  gr.add_edge(('colorzones', 'nlmeans'))
  
  # don't sharpen grain:
  gr.add_edge(('grain', 'sharpen'))
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
  gr.add_edge(('gamma', 'dither'))
  gr.add_edge(('channelmixer', 'colorout'))
  gr.add_edge(('clahe', 'colorout'))
  gr.add_edge(('velvia', 'colorout'))
  gr.add_edge(('soften', 'colorout'))
  gr.add_edge(('vignette', 'colorout'))
  gr.add_edge(('splittoning', 'colorout'))
  gr.add_edge(('watermark', 'colorout'))
  gr.add_edge(('overexposed', 'colorout'))
  gr.add_edge(('dither', 'colorout'))
  
  # borders should not change shape/color:
  gr.add_edge(('borders', 'colorout'))
  gr.add_edge(('borders', 'vignette'))
  gr.add_edge(('borders', 'splittoning'))
  gr.add_edge(('borders', 'velvia'))
  gr.add_edge(('borders', 'soften'))
  gr.add_edge(('borders', 'clahe'))
  gr.add_edge(('borders', 'channelmixer'))
  # don't indicate borders as over/under exposed
  gr.add_edge(('borders', 'overexposed'))

  # but watermark can be drawn on top of borders
  gr.add_edge(('watermark', 'borders'))

  # want dithering very late
  gr.add_edge(('dither', 'watermark'))
  
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
  # global variant is Lab:
  gr.add_edge(('globaltonemap', 'colorin'))
  gr.add_edge(('colorout', 'globaltonemap'))
  # we want it to first tonemap, then adjust contrast:
  gr.add_edge(('tonecurve', 'globaltonemap'))
  gr.add_edge(('colorcorrection', 'globaltonemap'))
  gr.add_edge(('levels', 'globaltonemap'))
  gr.add_edge(('atrous', 'globaltonemap'))
  gr.add_edge(('shadhi', 'globaltonemap'))
  gr.add_edge(('zonesystem', 'globaltonemap'))
  gr.add_edge(('bilat', 'globaltonemap'))
  
  # want to fine-tune stuff after injection of color transfer:
  gr.add_edge(('atrous', 'colormapping'))
  gr.add_edge(('colorzones', 'colormapping'))
  gr.add_edge(('tonecurve', 'colormapping'))
  gr.add_edge(('levels', 'colormapping'))
  gr.add_edge(('monochrome', 'colormapping'))
  gr.add_edge(('zonesystem', 'colormapping'))
  gr.add_edge(('colisa', 'colormapping'))
  gr.add_edge(('colorcorrection', 'colormapping'))
  gr.add_edge(('relight', 'colormapping'))
  gr.add_edge(('lowpass', 'colormapping'))
  gr.add_edge(('shadhi', 'colormapping'))
  gr.add_edge(('highpass', 'colormapping'))
  gr.add_edge(('lowlight', 'colormapping'))
  gr.add_edge(('bloom', 'colormapping'))
  
  # colorize first in Lab pipe
  gr.add_edge(('colortransfer', 'colorize'))
  gr.add_edge(('colormapping', 'colortransfer'))
  
  # defringe before color manipulations (colorbalance is sufficient) and before equalizer
  gr.add_edge(('colorbalance', 'defringe'))
  gr.add_edge(('equalizer', 'defringe'))

  # levels come after tone curve
  gr.add_edge(('levels', 'tonecurve'))

  # colisa comes before other contrast adjustments:
  gr.add_edge(('zonesystem', 'colisa'))
  gr.add_edge(('tonecurve', 'colisa'))
  gr.add_edge(('levels', 'colisa'))
  gr.add_edge(('relight', 'colisa'))

  # want to do highpass filtering after lowpass:
  gr.add_edge(('highpass', 'lowpass'))

  # want to do shadows&highlights before tonecurve etc.
  gr.add_edge(('tonecurve', 'shadhi'))
  gr.add_edge(('atrous', 'shadhi'))
  gr.add_edge(('levels', 'shadhi'))
  gr.add_edge(('zonesystem', 'shadhi'))
  gr.add_edge(('relight', 'shadhi'))
  gr.add_edge(('colisa', 'shadhi'))

  # the bilateral filter, in linear input rgb
  gr.add_edge(('colorin', 'bilateral'))
  gr.add_edge(('bilateral', 'demosaic'))
  # same for denoise based on noise profiles.
  # also avoid any noise confusion potentially caused
  # by distortions/averages or exposure gain.
  gr.add_edge(('colorin', 'denoiseprofile'))
  gr.add_edge(('denoiseprofile', 'demosaic'))
  gr.add_edge(('basecurve', 'denoiseprofile'))
  gr.add_edge(('lens', 'denoiseprofile'))
  gr.add_edge(('exposure', 'denoiseprofile'))
  gr.add_edge(('graduatednd', 'denoiseprofile'))
  gr.add_edge(('tonemap', 'denoiseprofile'))

  gr.add_edge(('colorout', 'equalizer'))
  # for smooth b/w images, we want chroma denoise to go before
  # color zones, where chrome can affect luma:
  gr.add_edge(('colorzones', 'equalizer'))
  gr.add_edge(('equalizer', 'colorin'))

  # colorbalance needs a Lab buffer and should be after clipping. probably.
  gr.add_edge(('clipping', 'colorbalance'))
  gr.add_edge(('colorbalance', 'colorin'))

gr = digraph()
gr.add_nodes([
'atrous',
'basecurve',
'bilateral',
'bilat',
'bloom',
'borders',
'cacorrect',
'channelmixer',
'clahe', # deprecated
'clipping',
'letsgofloat',
'colisa',
'colorbalance',
'colorcorrection',
'colorin',
'colorize',
'colorout',
'colortransfer',
'colormapping',
'colorzones',
'colorcontrast',
'defringe',
'demosaic',
'denoiseprofile',
'dither',
'equalizer', # deprecated
'exposure',
'flip',
'gamma',
'globaltonemap',
'graduatednd',
'grain',
'highlights',
'highpass',
'invert',
'hotpixels',
'lens',
'levels',
'lowpass',
'lowlight',
'monochrome',
'nlmeans',
'overexposed',
'profile_gamma',
'rawdenoise',
'relight',
'scalepixels',
'rotatepixels',
'shadhi',
'sharpen',
'soften',
'splittoning',
'spots',
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
    if not n == "rawspeed":
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
