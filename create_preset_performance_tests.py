#!/usr/bin/env python3
import struct
import binascii
import os

XMP_TEMPLATE = '''<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 4.4.0-Exiv2">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
   xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"
   xmlns:darktable="http://darktable.sf.net/"
   xmpMM:DerivedFrom="DSC_9034.NEF"
   darktable:xmp_version="4"
   darktable:raw_params="0"
   darktable:auto_presets_applied="1">
   <darktable:history>
    <rdf:Seq>
     <rdf:li
      darktable:num="0"
      darktable:operation="diffuse"
      darktable:enabled="1"
      darktable:modversion="2"
      darktable:params="{params_hex}"
      darktable:multi_name=""
      darktable:multi_priority="0"
      darktable:blendop_version="11"
      darktable:blendop_params="gz12eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRxQkA5qIZBA=="/>
    </rdf:Seq>
   </darktable:history>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
'''

def make_params_v2(iterations=20, sharpness=0.0, radius=32, regularization=1.0, variance_threshold=0.0,
                   aniso_first=0.0, aniso_second=0.0, aniso_third=0.0, aniso_fourth=0.0,
                   threshold=0.0, first=0.0, second=0.0, third=0.0, fourth=0.0,
                   radius_center=0):
    fmt = '<i f i f f  f f f f  f  f f f f  i'
    data = struct.pack(fmt,
                       iterations, sharpness, radius, regularization, variance_threshold,
                       aniso_first, aniso_second, aniso_third, aniso_fourth,
                       threshold, first, second, third, fourth,
                       radius_center)
    return binascii.hexlify(data).decode()

# Representative Presets
presets = {
    "deblur_medium": {
        "iterations": 20, "radius": 10, "radius_center": 0,
        "first": -0.25, "second": 0.125, "third": -0.5, "fourth": 0.25,
        "aniso_first": 1.0, "aniso_second": 0.0, "aniso_third": 1.0, "aniso_fourth": 0.0,
        "regularization": 3.0, "variance_threshold": 1.0
    },
    "denoise_medium": {
        "iterations": 20, "radius": 3, "radius_center": 4,
        "first": 0.05, "second": 0.0, "third": 0.05, "fourth": 0.0,
        "aniso_first": 2.0, "aniso_second": 0.0, "aniso_third": 2.0, "aniso_fourth": 0.0,
        "regularization": 4.0, "variance_threshold": -0.25
    },
    "local_contrast_normal": {
        "iterations": 20, "radius": 384, "radius_center": 512,
        "first": -0.5, "second": 0.0, "third": 0.0, "fourth": -0.5,
        "aniso_first": -2.5, "aniso_second": 0.0, "aniso_third": 0.0, "aniso_fourth": -2.5,
        "regularization": 1.0, "variance_threshold": 1.0
    },
    "sharpness_fast": {
        "iterations": 20, "radius": 128, "radius_center": 0,
        "first": 0.0, "second": 0.0, "third": -0.5, "fourth": 0.0,
        "aniso_first": 0.0, "aniso_second": 0.0, "aniso_third": 5.0, "aniso_fourth": 0.0,
        "regularization": 0.25, "variance_threshold": 0.25
    },
    "bloom": {
        "iterations": 20, "radius": 32, "radius_center": 0,
        "first": 0.5, "second": 0.5, "third": 0.5, "fourth": 0.5,
        "aniso_first": 0.0, "aniso_second": 0.0, "aniso_third": 0.0, "aniso_fourth": 0.0,
        "regularization": 0.0, "variance_threshold": 0.0
    }
}

test_dir = "/workspace/darktable/diffuse-perf-test-files"
for name, params in presets.items():
    params_hex = make_params_v2(**params)
    xmp_content = XMP_TEMPLATE.format(params_hex=params_hex)
    xmp_path = os.path.join(test_dir, f"preset_{name}.xmp")
    with open(xmp_path, "w") as f:
        f.write(xmp_content)
    print(f"Created {xmp_path}")
