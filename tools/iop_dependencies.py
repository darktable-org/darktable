#!/usr/bin/env python2

import sys
if sys.version_info[0] >= 3:
    raise "Must be using Python 2. Something is broken in Python 3."

def usage():
  sys.stderr.write("Usage: iop_dependencies.py [--apply]\n")
  sys.exit(2)

import sys
apply_changes = False

if len(sys.argv) > 2:
  usage()
elif len(sys.argv) == 2:
  if sys.argv[1] == "--apply":
    apply_changes = True
  else:
    usage()

# import graphviz (python-pygraph python-pygraphviz)
sys.path.append('..')
sys.path.append('/usr/lib/graphviz/python/')
sys.path.append('/usr/lib64/graphviz/python/')

# libgv-python
import pygraphviz as gv

# import pygraph
from pygraph.classes.digraph import digraph
from pygraph.algorithms.sorting import topological_sorting
from pygraph.algorithms.cycles import find_cycle
from pygraph.readwrite.dot import write
from pygraph.readwrite.dot import read

import fileinput
import sys
import os.path
import glob
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

  # these work on float, rescaled mosaic data:
  gr.add_edge(('demosaic', 'rawprepare'))

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

  # we want cropped and B/W rescaled pixels,
  # after after uint16 -> float conversion
  gr.add_edge(('temperature', 'rawprepare'))

  # and of course rawspeed needs to give us the pixels first:
  gr.add_edge(('rawprepare', 'rawspeed'))

  # inversion should be really early in the pipe
  gr.add_edge(('temperature', 'invert'))

  # but after cropping and B/W rescaling
  gr.add_edge(('invert', 'rawprepare'))

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
  # and lens correction, perspective correction which depend on original input buffers.
  # and after buffer has been downscaled/demosaiced
  gr.add_edge(('flip', 'demosaic'))
  gr.add_edge(('flip', 'scalepixels'))
  gr.add_edge(('flip', 'rotatepixels'))
  gr.add_edge(('flip', 'lens'))
  gr.add_edge(('flip', 'spots'))
  gr.add_edge(('flip', 'retouch'))
  gr.add_edge(('flip', 'liquify'))
  gr.add_edge(('flip', 'ashift'))

  # clipping is a convolution operation and needs linear data to avoid messing-up edges
  # therefore needs to go before any curve, gamma or contrast operation
  gr.add_edge(('basecurve', 'clipping'))
  gr.add_edge(('profile_gamma', 'clipping'))
  gr.add_edge(('tonecurve', 'clipping'))
  gr.add_edge(('graduatednd', 'clipping'))
  gr.add_edge(('colisa', 'clipping'))

  # ashift wants a lens corrected image with straight lines.
  # therefore lens should come before and liquify should come after ashift
  gr.add_edge(('ashift', 'lens'))
  gr.add_edge(('liquify', 'ashift'))

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
  gr.add_edge(('colorout', 'vibrance'))
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
  gr.add_edge(('colorout', 'colorreconstruction'))
  gr.add_edge(('bloom', 'colorin'))
  gr.add_edge(('nlmeans', 'colorin'))
  gr.add_edge(('colortransfer', 'colorin'))
  gr.add_edge(('colormapping', 'colorin'))
  gr.add_edge(('atrous', 'colorin'))
  gr.add_edge(('bilat', 'colorin'))
  gr.add_edge(('colorzones', 'colorin'))
  gr.add_edge(('lowlight', 'colorin'))
  gr.add_edge(('monochrome', 'colorin'))
  gr.add_edge(('vibrance', 'colorin'))
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
  gr.add_edge(('colorreconstruction', 'colorin'))

  # we want color reconstruction come before all other tone and color altering modules
  gr.add_edge(('bloom', 'colorreconstruction'))
  gr.add_edge(('nlmeans', 'colorreconstruction'))
  gr.add_edge(('colortransfer', 'colorreconstruction'))
  gr.add_edge(('colormapping', 'colorreconstruction'))
  gr.add_edge(('atrous', 'colorreconstruction'))
  gr.add_edge(('bilat', 'colorreconstruction'))
  gr.add_edge(('colorzones', 'colorreconstruction'))
  gr.add_edge(('lowlight', 'colorreconstruction'))
  gr.add_edge(('monochrome', 'colorreconstruction'))
  gr.add_edge(('vibrance', 'colorreconstruction'))
  gr.add_edge(('zonesystem', 'colorreconstruction'))
  gr.add_edge(('tonecurve', 'colorreconstruction'))
  gr.add_edge(('levels', 'colorreconstruction'))
  gr.add_edge(('relight', 'colorreconstruction'))
  gr.add_edge(('colorcorrection', 'colorreconstruction'))
  gr.add_edge(('sharpen', 'colorreconstruction'))
  gr.add_edge(('grain', 'colorreconstruction'))
  gr.add_edge(('lowpass', 'colorreconstruction'))
  gr.add_edge(('shadhi', 'colorreconstruction'))
  gr.add_edge(('highpass', 'colorreconstruction'))
  gr.add_edge(('colorcontrast', 'colorreconstruction'))
  gr.add_edge(('colorize', 'colorreconstruction'))
  gr.add_edge(('colisa', 'colorreconstruction'))
  gr.add_edge(('defringe', 'colorreconstruction'))

  # we want haze removal in RGB space before color reconstruction
  gr.add_edge(('colorin', 'hazeremoval'))
  gr.add_edge(('hazeremoval', 'profile_gamma'))

  # spot removal works on demosaiced data
  # and needs to be before geometric distortions:
  gr.add_edge(('spots', 'demosaic'))
  gr.add_edge(('scalepixels', 'spots'))
  gr.add_edge(('rotatepixels', 'spots'))
  gr.add_edge(('lens', 'spots'))
  gr.add_edge(('borders', 'spots'))
  gr.add_edge(('clipping', 'spots'))

  # retouch as well:
  gr.add_edge(('retouch', 'demosaic'))
  gr.add_edge(('scalepixels', 'retouch'))
  gr.add_edge(('rotatepixels', 'retouch'))
  gr.add_edge(('lens', 'retouch'))
  gr.add_edge(('borders', 'retouch'))
  gr.add_edge(('clipping', 'retouch'))

  # liquify immediately after spot removal / retouch
  gr.add_edge(('liquify', 'spots'))
  gr.add_edge(('liquify', 'retouch'))
  gr.add_edge(('liquify', 'lens'))
  gr.add_edge(('rotatepixels', 'liquify'))
  gr.add_edge(('scalepixels', 'liquify'))

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
  gr.add_edge(('colorcorrection', 'filmic'))
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
  gr.add_edge(('gamma', 'rawoverexposed'))
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
  gr.add_edge(('rawoverexposed', 'colorout'))
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
  gr.add_edge(('borders', 'rawoverexposed')) # can, but no need to
  # don't resample borders when scaling to the output dimensions
  gr.add_edge(('borders', 'finalscale'))

  # do want to downsample very late
  gr.add_edge(('finalscale', 'colorout'))
  gr.add_edge(('finalscale', 'vignette'))
  gr.add_edge(('finalscale', 'splittoning'))
  gr.add_edge(('finalscale', 'velvia'))
  gr.add_edge(('finalscale', 'soften'))
  gr.add_edge(('finalscale', 'clahe'))
  gr.add_edge(('finalscale', 'channelmixer'))

  # but can display overexposure after scaling
  # NOTE: finalscale is only done in export pipe,
  #       while *overexposed is only done in full darkroom preview pipe
  gr.add_edge(('overexposed', 'finalscale'))
  gr.add_edge(('rawoverexposed', 'finalscale'))

  # let's display raw overexposure indication after usual overexposed
  gr.add_edge(('rawoverexposed', 'overexposed'))

  # but watermark can be drawn on top of borders
  gr.add_edge(('watermark', 'borders'))
  # also, do not resample watermark
  gr.add_edge(('watermark', 'finalscale'))

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
  gr.add_edge(('vibrance', 'defringe'))
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

  # filmic is a simple parametric tonecurve + log that simulates film
  # we want it as the last step in the linear pipe
  gr.add_edge(('colorout', 'filmic'))
  gr.add_edge(('tonecurve', 'filmic'))
  gr.add_edge(('levels', 'filmic'))
  gr.add_edge(('zonesystem', 'filmic'))
  gr.add_edge(('relight', 'filmic'))
  gr.add_edge(('colisa', 'filmic'))
  gr.add_edge(('filmic', 'colorbalance'))

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
  gr.add_edge(('colorbalance', 'colorin'))
  gr.add_edge(('colorout', 'colorbalance'))
  gr.add_edge(('colorize', 'colorbalance'))

  # colorchecker should happen early in Lab mode, after
  # highlight colour reconstruction, but with the ability to mess with everything
  # after transforming the colour space
  gr.add_edge(('colorout', 'colorchecker'))
  gr.add_edge(('bloom', 'colorchecker'))
  gr.add_edge(('nlmeans', 'colorchecker'))
  gr.add_edge(('colorbalance', 'colorchecker'))
  gr.add_edge(('colortransfer', 'colorchecker'))
  gr.add_edge(('colormapping', 'colorchecker'))
  gr.add_edge(('atrous', 'colorchecker'))
  gr.add_edge(('bilat', 'colorchecker'))
  gr.add_edge(('colorzones', 'colorchecker'))
  gr.add_edge(('lowlight', 'colorchecker'))
  gr.add_edge(('monochrome', 'colorchecker'))
  gr.add_edge(('vibrance', 'colorchecker'))
  gr.add_edge(('zonesystem', 'colorchecker'))
  gr.add_edge(('tonecurve', 'colorchecker'))
  gr.add_edge(('levels', 'colorchecker'))
  gr.add_edge(('relight', 'colorchecker'))
  gr.add_edge(('colorcorrection', 'colorchecker'))
  gr.add_edge(('sharpen', 'colorchecker'))
  gr.add_edge(('grain', 'colorchecker'))
  gr.add_edge(('lowpass', 'colorchecker'))
  gr.add_edge(('shadhi', 'colorchecker'))
  gr.add_edge(('highpass', 'colorchecker'))
  gr.add_edge(('colorcontrast', 'colorchecker'))
  gr.add_edge(('colorize', 'colorchecker'))
  gr.add_edge(('colisa', 'colorchecker'))
  gr.add_edge(('defringe', 'colorchecker'))
  gr.add_edge(('filmic', 'colorchecker'))
  gr.add_edge(('colorchecker', 'colorreconstruction'))

  # ugly hack: don't let vibrance drift any more
  # gr.add_edge(('vibrance', 'defringe'))
  gr.add_edge(('colorbalance', 'vibrance'))
  gr.add_edge(('colorize', 'vibrance'))

gr = digraph()
gr.add_nodes([
'atrous',
'ashift',
'basecurve',
'bilateral',
'bilat',
'bloom',
'borders',
'cacorrect',
'channelmixer',
'clahe', # deprecated
'clipping',
'colisa',
'colorbalance',
'colorchecker',
'colorcorrection',
'colorin',
'colorize',
'colorout',
'colortransfer',
'colormapping',
'colorzones',
'colorcontrast',
'colorreconstruction',
'defringe',
'demosaic',
'denoiseprofile',
'dither',
'equalizer', # deprecated
'exposure',
'finalscale',
'filmic',
'flip',
'gamma',
'globaltonemap',
'graduatednd',
'grain',
'highlights',
'highpass',
'invert',
'hazeremoval',
'hotpixels',
'lens',
'levels',
'liquify',
'lowpass',
'lowlight',
'monochrome',
'nlmeans',
'overexposed',
'rawoverexposed',
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
'retouch',
'temperature',
'tonecurve',
'tonemap',
'velvia',
'vibrance',
'vignette',
'watermark',
'zonesystem',
'rawprepare',
'rawspeed' ])

add_edges(gr)

# make sure we don't have cycles:
cycle_list = find_cycle(gr)
if cycle_list:
  print("cycles:")
  print(cycle_list)
  exit(1)

# replace all the priorities with garbage. to make sure all the iops are in this file.
for filename in glob.glob(os.path.join(os.path.dirname(__file__), '../src/iop/*.c')) + glob.glob(os.path.join(os.path.dirname(__file__), '../src/iop/*.cc')):
  if apply_changes:
    replace_all(filename, "( )*?(?P<identifier>((\w)*))( )*?->( )*?priority( )*?(=).*?(;).*\n", "  \g<identifier>->priority = %s; // module order created by iop_dependencies.py, do not edit!\n"%"NAN")

# get us some sort order!
sorted_nodes = topological_sorting(gr)
length=len(sorted_nodes)
priority=1000
for n in sorted_nodes:
  # now that should be the priority in the c file:
  print("%d %s"%(priority, n))
  filename=os.path.join(os.path.dirname(__file__), "../src/iop/%s.c"%n)
  if not os.path.isfile(filename):
    filename=os.path.join(os.path.dirname(__file__), "../src/iop/%s.cc"%n)
  if not os.path.isfile(filename):
    if not n == "rawspeed":
      print("could not find file `%s'"%filename)
    continue
  if apply_changes:
    replace_all(filename, "( )*?(?P<identifier>((\w)*))( )*?->( )*?priority( )*?(=).*?(;).*\n", "  \g<identifier>->priority = %d; // module order created by iop_dependencies.py, do not edit!\n"%priority)
  priority -= 1000.0/(length-1.0)

# beauty-print the sorted pipe as pdf:
dot = write(gr)
gvv = gv.AGraph(dot)
gvv.layout(prog='dot')
gvv.draw(os.path.join(os.path.dirname(__file__), 'iop_deps.pdf'))
