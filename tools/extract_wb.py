#!/usr/bin/python3
# direct translation of extract_wb in python using as little external deps as possible

from __future__ import print_function
import sys
from sys import argv
import os
import xml.etree.ElementTree as ET
import subprocess
from subprocess import PIPE
import shlex

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

if len(argv) < 2 :
    sys.exit("Usage: extract_wb <file1> [file2] ...")

IGNORED_PRESETS = {"Auto", "Kelvin", "Measured", "AsShot","Preset", "Natural Auto",
                 "Multi Auto", "Color Temperature Enhancement", "Custom",
                 "One Touch WB 1", "One Touch WB 2", "One Touch WB 3",
                 "One Touch WB 4", "Custom WB 1", "Auto0", "Auto1", "Auto2",
                 "CWB1", "CWB2", "CWB3", "CWB4", "Black", "Illuminator1",
                 "Illuminator2", "Uncorrected"}

FL_PRESET_REPLACE = {
  "Fluorescent" : "CoolWhiteFluorescent",
  "FluorescentP1" : "DayWhiteFluorescent",
  "FluorescentP2" : "DaylightFluorescent",
  "FluorescentM1" : "WarmWhiteFluorescent",
  "FluorescentD"  : "DaylightFluorescent",
  "FluorescentN"  : "NeutralFluorescent",
  "FluorescentW"  : "WhiteFluorescent",
  "Daylight Fluorescent" : "DaylightFluorescent",
  "Day White Fluorescent" : "DayWhiteFluorescent",
  "White Fluorescent" : "WhiteFluorescent",
  "Unknown (0x600)" : "Underwater",
  "Sunny" : "DirectSunlight",
  "Fine Weather" : "DirectSunlight",
  "Tungsten (Incandescent)" : "Tungsten",
  "ISO Studio Tungsten" : "Tungsten",
  "Cool WHT FL" : "CoolWhiteFluorescent",
  "Daylight FL" : "DaylightFluorescent",
  "Warm WHT FL" : "WarmWhiteFluorescent",
  "Warm White Fluorescent" : "WarmWhiteFluorescent",
  "White FL" : "WhiteFluorescent",
  "Mercury Lamp" : "HighTempMercuryVaporFluorescent",
  "Day White FL" : "DayWhiteFluorescent",
  "Sodium Lamp" : "SodiumVaporFluorescent",
  "3000K (Tungsten light)" : "Tungsten",
  "4000K (Cool white fluorescent)" : "CoolWhiteFluorescent",
  "5300K (Fine Weather)" : "Daylight",
  "5500K (Flash)" : "Flash",
  "6000K (Cloudy)" : "Cloudy",
  "7500K (Fine Weather with Shade)" : "Shade",
  }

PRESET_ORDER = ["DirectSunlight", "Daylight", "D55", "Shade","Cloudy",
              "Tungsten", "Incandescent","Fluorescent", 
              "WarmWhiteFluorescent", "CoolWhiteFluorescent",
              "DayWhiteFluorescent","DaylightFluorescent",
              "DaylightFluorescent", "NeutralFluorescent", "WhiteFluorescent",
              "HighTempMercuryVaporFluorescent", "HTMercury", 
              "SodiumVaporFluorescent", "Underwater", "Flash", "Unknown"]

PRESET_SORT_MAPPING = {}

for index,name in enumerate(PRESET_ORDER):
    PRESET_SORT_MAPPING[name] = index + 1

cams_from_source = os.path.dirname(os.path.abspath(__file__)) + "/../src/external/rawspeed/data/cameras.xml"
cams_from_dist = os.path.dirname(os.path.abspath(__file__)) + "/../rawspeed/cameras.xml"

CAMERAS = os.path.abspath(cams_from_source) if os.path.exists(os.path.abspath(cams_from_source)) else os.path.abspath(cams_from_dist)

if not os.path.exists(CAMERAS):
    sys.exit("Can't find cameras mapping file, should be in {0}".format(CAMERAS))

exif_name_map = {}
xml_doc = ET.parse(CAMERAS)
for camera in xml_doc.getroot().findall('Camera'):
    maker = exif_maker = camera.get('make')
    model = exif_model = camera.get('model')
    exif_id = maker,model
    if camera.find('ID') is not None:
        cid = camera.find('ID')
        maker = cid.get('make')
        model = cid.get('model')
    exif_name_map[exif_id] = maker,model
    for alias in camera.findall('Alias'):
        exif_model = alias.text
        exif_id = exif_maker, exif_model
        exif_name_map[exif_id] = maker,model

found_presets = []

for filename in argv[1:]:
    red = green = blue = maker = model = preset = None
    finetune = fl_count = rlevel = blevel = glevel = 0
    listed_presets = []
    preset_names = {}
    gm_skew = False
    command = "exiftool -Make -Model \"-WBType*\" \"-WB_*\" \"-ColorTemp*\"    "\
    "-WhiteBalance -WhiteBalance2 -WhitePoint -ColorCompensationFilter       "\
    "-WBShiftAB -WBShiftAB_GM -WBShiftAB_GM_Precise -WBShiftGM -WBScale      "\
    "-WhiteBalanceFineTune -WhiteBalanceComp -WhiteBalanceSetting            "\
    "-WhiteBalanceBracket -WhiteBalanceBias -WBMode -WhiteBalanceMode        "\
    "-WhiteBalanceTemperature -WhiteBalanceDetected -ColorTemperature        "\
    "-WBShiftIntelligentAuto -WBShiftCreativeControl -WhiteBalanceSetup      "\
    "-WBRedLevel -WBBlueLevel -WBGreenLevel -RedBalance -BlueBalance         "\
    "\"{0}\"".format(filename)
    if filename.endswith(('.txt','.TXT')):
        command = 'cat "{0}"'.format(filename)
    command = shlex.split(command)
    proc = subprocess.check_output(command, universal_newlines=True)
    for io in proc.splitlines():
        lineparts = io.split(':')
        tag = lineparts[0].strip()
        values = lineparts[1].strip().split(' ')
        if 'Make' in tag.split():
            maker = lineparts[1].strip()
        elif 'Model' in tag.split():
            model = lineparts[1].strip()
        elif tag == "WB RGGB Levels":
            green = (float(values[1])+float(values[2]))/2.0
            red = float(values[0])/green
            blue = float(values[3])/green
            green = 1
        elif tag == "WB RB Levels":
            red = float(values[0])
            blue = float(values[1])
            if len(values) == 4 and values[2] == "256" and values[3] == "256":
                red /= 256.0
                blue /= 256.0
            green = 1
            print("red:",red, "green:", green, "blue:", blue, sep=' ')
        elif tag == "WB GRB Levels":
            green = float(values[0])
            red = float(values[1])/green
            blue = float(values[2])/green
            green = 1
        # elif tag == "WB GRB Levels Auto" and maker == "FUJIFILM" # fuji seems to use "WB GRB Levels Auto to describe manual finetuning
        #  green = float(values[0])
        #  red = float(values[1])/green
        #  blue = float(values[2])/green
        #  green = 1
        elif tag == "White Point" and len(values) > 3:
            green = (float(values[1])+float(values[2]))/2.0
            red = float(values[0])/green
            blue = float(values[3])/green
            green = 1
        elif tag == "White Balance" or tag == "White Balance 2":
            preset = ' '.join(values)
            preset = FL_PRESET_REPLACE.get(preset,preset)
        elif ' '.join(tag.split()[:2]) == "WB Type":
            preset_names[' '.join(tag.split()[:2])] = ' '.join(values)
        elif ' '.join(tag.split()[:3]) in ['WB RGB Levels', 'WB RRGB Levels', 'WB RB Levels']:
            p = ' '.join(tag.split()[3:])
            p = preset_names[p] if p and preset_names[p] else p
            r=g=b=0

            if len(values) == 4:
                g = (float(values[1])+float(values[2]))/2.0
                r = float(values[0])/g
                b = float(values[3])/g
                g = 1
            elif len(values) == 3:
                g = float(values[1])
                r = float(values[0])/g
                b = float(values[2])/g
                g = 1
            elif len(values) == 2:
                r = float(values[0])
                b = float(values[2])
                g = 1
            else:
                eprint("Found RGB tag '{0}' with {1} values instead of 2, 3 or 4".format(p, len(values)))
            
            if 'Fluorescent' in p:
                fl_count += 1
            
            if not p:
                p= 'Unknown'
            
            listed_presets.append(tuple([p,r,g,b]))
        elif tag == "WB Red Level":
            rlevel = float(values[0])
        elif tag == "WB Blue Level":
            blevel = float(values[0])
        elif tag == "WB Green Level":
            glevel = float(values[0])
        elif tag == "WB Shift AB": # canon - positive is towards amber, panasonic/leica/pentax - positive is towards blue?
            finetune = values[0]
        elif tag == "WB Shift GM": # detect GM shift and warn about it
            gm_skew = gm_skew or (int(values[0]) != 0)
        elif tag == "WB Shift AB GM": # sony
            finetune = values[0]
            gm_skew = gm_skew or (int(values[1]) != 0)
        elif tag == "White Balance Fine Tune" and maker.startswith("NIKON"): # nikon
            finetune = 0-(int(values[0]) * 2) # nikon lies about half-steps (eg 6->6->5 instead of 6->5.5->5, need to address this later on, so rescalling this now)
            gm_skew = gm_skew or (int(values[1]) != 0)
        elif tag == "White Balance Fine Tune" and maker == "FUJIFILM": # fuji
            eprint("Warning: Fuji does not seem to produce any sensible data for finetuning! If all finetuned values are identical, use one with no finetuning (0)")
            finetune = int(values[3]) / 20 # Fuji has -180..180 but steps are every 20
            gm_skew = gm_skew or (int(values[1]) != 0)
        elif tag == "White Balance Fine Tune" and maker == "SONY" and preset == "CoolWhiteFluorescent":
            # Sony's Fluorescent Fun
            if values[0] == "-1":
                preset = "WarmWhiteFluorescent"
            elif values[0] == "0":
                preset = "CoolWhiteFluorescent"
            elif values[0] == "1":
                preset = "DayWhiteFluorescent"
            elif values[0] == "2":
                preset = "DaylightFluorescent"
            else:
                eprint("Warning: Unknown Sony Fluorescent WB Preset!")
        elif tag == "White Balance Bracket": # olympus
            finetune = values[0]
            gm_skew = gm_skew or (int(values[1]) != 0)
        elif tag == "Color Compensation Filter": # minolta?
            gm_skew = gm_skew or (int(values[0]) != 0)

        if rlevel > 0 and glevel > 0 and blevel > 0:
            red = rlevel/glevel
            blue = blevel/glevel
            green = 1

    if gm_skew:
        eprint('WARNING: {0} has finetuning over GM axis! Data is skewed!'.format(filename))
    else:
        eprint("no skew")

    # Adjust the maker/model we found with the map we generated before
    if exif_name_map[maker,model]:
        enm = exif_name_map[maker,model]
        print(enm)
        maker = enm[0]
        model = enm[1]
    else:
        eprint("WARNING: Couldn't find model in cameras.xml ('{0}', '{1}')".format(maker, model))

    print("found:")
    print(found_presets)
    print("listed:")
    print(listed_presets)

    for preset_arr in listed_presets:
        # ugly hack. Canon's Fluorescent is listed as WhiteFluorescent in usermanual
        preset = preset_arr[0]
        if maker and maker == "Canon" and preset == "Fluorescent":
            preset = "WhiteFluorescent"
        preset = FL_PRESET_REPLACE.get(preset,preset)
        if preset not in IGNORED_PRESETS:
            found_presets.append(tuple([maker,model,preset, "0", red, green, blue]))
    
    # Print out the WB value that was used in the file
    if not preset:
        eprint("empty prest!")
        preset = filename
    if red and green and blue and preset not in IGNORED_PRESETS:
        eprint("not empty preset!")
        found_presets.append(tuple([maker, model, preset, finetune, red, green, blue]))
    
    print("after loopend:")
    print(found_presets)
# what's wrong with this code? found_presets[:]=[preset_arrt in found_presets if preset_arrt[2] not in IGNORED_PRESETS]

print("before getting rid of duplicates")
print(found_presets)
# get rid of duplicate presets

found_presets = list(set(found_presets))

print("after getting rid")
print(found_presets)


 