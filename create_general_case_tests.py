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

def make_params_v2(iterations=1, sharpness=0.0, radius=32, regularization=1.0, variance_threshold=0.0,
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

tests = {
    "G1": {
        "iterations": 20, "sharpness": 0.01, "radius": 8, "regularization": 2.0, "variance_threshold": 0.5,
        "aniso_first": 1.5, "aniso_second": -0.5, "aniso_third": 0.7, "aniso_fourth": 1.2,
        "first": -0.12, "second": 0.08, "third": -0.15, "fourth": 0.11,
        "radius_center": 4
    },
    "G2": {
        "iterations": 20, "sharpness": 0.005, "radius": 12, "regularization": 1.5, "variance_threshold": -0.1,
        "aniso_first": -2.0, "aniso_second": 3.0, "aniso_third": -1.0, "aniso_fourth": 0.5,
        "first": 0.25, "second": -0.15, "third": 0.10, "fourth": -0.05,
        "radius_center": 0
    },
    "G3": {
        "iterations": 20, "sharpness": 0.02, "radius": 4, "regularization": 3.5, "variance_threshold": 0.0,
        "aniso_first": 5.0, "aniso_second": 2.5, "aniso_third": 7.5, "aniso_fourth": 1.0,
        "first": -0.05, "second": -0.05, "third": -0.05, "fourth": -0.05,
        "radius_center": 2
    },
}

test_dir = "/workspace/darktable/diffuse-perf-test-files"
for name, params in tests.items():
    params_hex = make_params_v2(**params)
    xmp_content = XMP_TEMPLATE.format(params_hex=params_hex)
    xmp_path = os.path.join(test_dir, f"general_{name}.xmp")
    with open(xmp_path, "w") as f:
        f.write(xmp_content)
    print(f"Created {xmp_path}")
