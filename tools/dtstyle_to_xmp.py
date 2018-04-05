#!/usr/bin/python3
# Simple script for converting darktable .dtstyle files to .xmp (version 2)

from sys import argv
from os import path
import xml.etree.ElementTree as ET

if len(argv) != 3 :
    print ("usage:",argv[0],"style.dtstyle file.xmp")
    exit(1)

# Check if the input file exists.
if not path.exists(argv[1]):
    print ("ERROR: input file:",argv[1],"doesn't exists.")
    exit(1)

# Check if the requested output file already exist
if path.exists(argv[2]):
    print ("ERROR: output file:",argv[2],"already exists.")
    exit(1)

try:
    styletree = ET.parse(argv[1]) or die("This doesn't work for me")
except:
    print ("ERROR: input file:",argv[1],"is not a valid dtsyle-file.")
    exit(1)

styleroot = styletree.getroot()

# Create a new xml structure.
xmpmeta=ET.Element("x:xmpmeta",{"xmlns:x":"adobe:ns:meta/",
                                "x:xmptk":"XMP Core 4.4.0-Exiv2"})
rdf=ET.SubElement(xmpmeta,"rdf:RDF",{"xmlns:rdf":"http://www.w3.org/1999/02/22-rdf-syntax-ns#"})
description=ET.SubElement(rdf,"rdf:Description",{"rdf:about":"",
                              "xmlns:xmp":"http://ns.adobe.com/xap/1.0/",
                              "xmlns:xmpMM":"http://ns.adobe.com/xap/1.0/mm/",
                              "xmlns:dc":"http://purl.org/dc/elements/1.1/",
                              "xmlns:darktable":"http://darktable.sf.net/",
                              "xmp:Rating":"0",
                              "xmpMM:DerivedFrom":"PureAwesome.raw",
                              "darktable:xmp_version":"2",
                              "darktable:raw_params":"0",
                              "darktable:auto_presets_applied":"1"})
# darktable:history_end is not needed with a conversion, darktable will defaults to the topmost element in the history stack
# "darktable:history_end":"8"})
maskid=ET.SubElement(description,"darktable:mask_id")
maskid.append(ET.Element("rdf:Seq"))
masktype=ET.SubElement(description,"darktable:mask_type")
masktype.append(ET.Element("rdf:Seq"))
maskname=ET.SubElement(description,"darktable:mask_name")
maskname.append(ET.Element("rdf:Seq"))
maskversion=ET.SubElement(description,"darktable:mask_version")
maskversion.append(ET.Element("rdf:Seq"))
mask=ET.SubElement(description,"darktable:mask")
mask.append(ET.Element("rdf:Seq"))
masknb=ET.SubElement(description,"darktable:mask_nb")
masknb.append(ET.Element("rdf:Seq"))
masksrc=ET.SubElement(description,"darktable:mask_src")
masksrc.append(ET.Element("rdf:Seq"))
darktablehistory=ET.SubElement(description,"darktable:history")
darktablehistoryid=ET.SubElement(darktablehistory,"rdf:Seq")

# iterate through the dtstyle to find and add the used plugins to the xmp
for plugins in styleroot.findall('./style/plugin'):
    enabled = plugins.find('enabled')
    if enabled.text == "1":
        li=ET.SubElement(darktablehistoryid,"rdf:li")
        li.set("darktable:enabled","1")

        modversion = plugins.find('module')
        if modversion != None:
            li.set("darktable:modversion",modversion.text)

        operation = plugins.find('operation')
        if operation != None:
            li.set("darktable:operation",operation.text)

        params = plugins.find('op_params')
        if params != None:
            li.set("darktable:params",params.text)

        blendop_params = plugins.find('blendop_params')
        if blendop_params != None:
            li.set("darktable:blendop_params",blendop_params.text)

        blendop_version = plugins.find('blendop_version')
        if blendop_version != None:
            li.set("darktable:blendop_version",blendop_version.text)

        multi_name = plugins.find('multi_name')
        if multi_name != None:
            if multi_name.text == None:
                li.set("darktable:multi_name","")
            else:
                li.set("darktable:multi_name",multi_name.text)

        multi_priority = plugins.find('multi_priority')
        if multi_priority != None:
            li.set("darktable:multi_priority",multi_priority.text)

# Write the newly created xmp to a file
ET.ElementTree(xmpmeta).write(argv[2],"UTF-8")
