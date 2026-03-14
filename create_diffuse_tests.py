#!/usr/bin/env python3
"""Create integration test XMPs for diffuse module with different anisotropy configs."""

import struct
import binascii
import os
import shutil

# Template XMP — based on 0086-diffuse but with modversion=2
XMP_TEMPLATE = '''<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 4.4.0-Exiv2">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
   xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"
   xmlns:darktable="http://darktable.sf.net/"
   xmpMM:DerivedFrom="mire1.cr2"
   darktable:import_timestamp="1604438919"
   darktable:change_timestamp="1625496703"
   darktable:export_timestamp="-1"
   darktable:print_timestamp="-1"
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

# Test configurations
tests = {
    # All isotrope (anisotropy = 0 for all)
    "0087-diffuse-isotrope": {
        "iterations": 2, "radius": 32, "regularization": 1.0,
        "aniso_first": 0.0, "aniso_second": 0.0, "aniso_third": 0.0, "aniso_fourth": 0.0,
        "first": 0.5, "second": 0.5, "third": 0.5, "fourth": 0.5,
    },
    # All gradient (negative anisotropy)
    "0088-diffuse-gradient": {
        "iterations": 2, "radius": 32, "regularization": 2.0, "variance_threshold": 0.25,
        "aniso_first": -3.0, "aniso_second": -3.0, "aniso_third": -3.0, "aniso_fourth": -3.0,
        "first": -0.25, "second": -0.25, "third": -0.25, "fourth": -0.25,
    },
    # Mixed isotropy: isophote, isotrope, gradient, isophote
    "0089-diffuse-mixed": {
        "iterations": 2, "radius": 32, "regularization": 2.0, "variance_threshold": 0.5,
        "aniso_first": 3.0, "aniso_second": 0.0, "aniso_third": -3.0, "aniso_fourth": 2.0,
        "first": -0.25, "second": 0.1, "third": -0.5, "fourth": 0.25,
    },
    # Sharpen preset style (negative speeds, positive anisotropy)
    "0090-diffuse-sharpen": {
        "iterations": 2, "radius": 8, "regularization": 3.0, "variance_threshold": 1.0,
        "aniso_first": 1.0, "aniso_second": 0.0, "aniso_third": 1.0, "aniso_fourth": 0.0,
        "first": -0.25, "second": 0.125, "third": -0.50, "fourth": 0.25,
    },
}

integration_dir = "src/tests/integration"
for test_name, params in tests.items():
    test_dir = os.path.join(integration_dir, test_name)
    os.makedirs(test_dir, exist_ok=True)

    # Operation name from test dir (strip leading digits and dash)
    op_name = test_name.split("-", 1)[1]

    params_hex = make_params_v2(**params)
    xmp_content = XMP_TEMPLATE.format(params_hex=params_hex)

    xmp_path = os.path.join(test_dir, f"{op_name}.xmp")
    with open(xmp_path, "w") as f:
        f.write(xmp_content)

    print(f"Created {xmp_path}")
    print(f"  params: {params}")

print("\nDone. Now generate expected.png for each test from the master branch.")
