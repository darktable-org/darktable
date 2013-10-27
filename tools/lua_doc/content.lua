doc = require "core"
real_darktable = require "darktable"
darktable = doc.toplevel.darktable
types = doc.toplevel.types
events = doc.toplevel.events
attributes = doc.toplevel.attributes
local tmp_node


----------------------
--  EARLY TWEAKING  --
----------------------
for k, v in pairs(real_darktable.modules.format) do
	local res = v()
	doc.document_type_from_obj(res,types[real_darktable.debug.type(res)])
end

for k, v in pairs(real_darktable.modules.storage) do
	local res = v()
	if res then
		doc.document_type_from_obj(res,types[real_darktable.debug.type(res)])
	end
end


print("warning, avoid problems with picasa/facebook")
types.dt_imageio_module_storage_data_email:set_text([[TBSL, force first]])
local all_sons = {}
for k,v in types:all_children() do
	if k:sub(1,#"dt_imageio_module_storage") == "dt_imageio_module_storage" then
		table.insert(all_sons,v)
	end

end
doc.create_artificial_parent("dt_imageio_module_storage",types,all_sons);

all_sons = {}
for k,v in types:all_children() do
	if k:sub(1,#"dt_imageio_module_format") == "dt_imageio_module_format" then
		table.insert(all_sons,v)
	end

end
doc.create_artificial_parent("dt_imageio_module_format",types,all_sons);


----------------------
--  TOPLEVEL        --
----------------------
doc.toplevel:set_text([[
to access the darktable specific functions you must load the darktable environement

<pre>
darktable = require "darktable"
</pre>

all functions and data are accessed through the darktable module

This documentation was generated with version ]]..real_darktable.configuration.version)
----------------------
--  DARKTABLE       --
----------------------
darktable:set_text([[the darktable library is the main entry point for all access to the darktable internals.]])
darktable.print:set_text([[will print a string to the darktable control log (the long overlayed window that appears over the main panel) ]])
darktable.print:add_parameter("message","string",[[the string to display, should be a single line]])

darktable.print_error:set_text([[This function will print it's parameter if the lua logdomain is activated. Start darktable with the "-d lua" parameter to enable the lua logdomain]])
darktable.print_error:add_parameter("message","string",[[the string to display]])

darktable.register_event:set_text([[this function register a callback to be called when a given event happens

Events are documented in the event section]])
darktable.register_event:add_parameter("event_type","string",[[the name of the event to register to]])
darktable.register_event:add_parameter("callback","function",[[the function to call on event, the signature of the function depends on the type of event]])
darktable.register_event:add_parameter("...","depends on event_type",[[some events need extra parameters at registraion time. These must be specified here.]])

darktable.register_storage:set_text([[This function will add a new storage implemented in lua. 
A storage is a module that is responsible for using images that have been generated during export. Examples of core storages include filesystem, e-mail, facebook...]])
darktable.register_storage:add_parameter("plugin_name","string",[[unique name for the plugin]])
darktable.register_storage:add_parameter("name","string",[[human readable name for the plugin]])
tmp_node = darktable.register_storage:add_parameter("store","function",[[this function is called once for each exported image. Images can be exported in paralell but the calls to this function will be serialized]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage,[[the storage object used for the export]])
tmp_node:add_parameter("image",types.dt_lua_image_t,[[the exported image object]])
tmp_node:add_parameter("format",types.dt_imageio_module_format,[[the format object used for the export]])
tmp_node:add_parameter("filename","string",[[the name of a temporary file where the processed image is stored]])
tmp_node:add_parameter("number","integer",[[the number of the image out of the export serie]])
tmp_node:add_parameter("total","integer",[[the total number of images in the export serie]])
tmp_node:add_parameter("high_quality","boolean",[[true if the export is high quality]])
tmp_node:add_parameter("extra_data","table",[[an empty lua table to store extra data. This table is common to all calls to store and the call to finalize in a given export serie]])
tmp_node = darktable.register_storage:add_parameter("finalize","function",[[this function is called once all images are processed and all store calls are finished]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage,[[the storage object used for the export]])
tmp_node:add_parameter("image_table","table",[[a table keyed by the exported image objects and valued with the corresponding temporary export filename]])
tmp_node:add_parameter("extra_data","table",[[an empty lua table to store extra data. This table is common to all calls to store and the call to finalize in a given export serie]])
tmp_node = darktable.register_storage:add_parameter("supported","function",[[called to check if a given image format is supported by the lua storage. This is used to build the dropdown format list for the GUI

Note that the parameters in the format are the ones currently set in the GUI. The user might change them before export]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage,[[the storage object tested]])
tmp_node:add_parameter("format",types.dt_imageio_module_format,[[the format object to report about]])
tmp_node:add_return("boolean",[[true if the corresponding format is supported]])

darktable.films:set_text([[a table containing all the film objects in the database]])
darktable.films['#']:set_text([[each film has a numeric entry in the database]])
----------------------
--  DARKTABLE.GUI   --
----------------------
darktable.gui:set_text([[This subtable contains function and data to manipulate the darktable user interface with lua.

Most of these function won't do anything if gui is not enabled (i.e you are using darktabl-cli and not darktable)]])

darktable.gui.action_images:set_text([[A table of images on which the user expects us to act.
It is based on both the hovered image and the selection and is
consistent with the way darktable works.

It is recommended to use this table to implement lua actions rather than dt.gui.hovered or dt.gui.selected to be consistant with darktable's gui]])
for k, v in darktable.gui.action_images:all_children() do
	v:remove_parent(darktable.gui.action_images)
	darktable.gui.action_images[k] = nil
end

darktable.gui.hovered:set_text([[The image under the cursor or nil if no image is hovered]])
darktable.gui.selection:set_text([[Allows to change the set of selected images]])
darktable.gui.selection:add_parameter("selection","table",[[a table of images, will set the selected images, if this parameter is not given, the selection will be untouched. If an empty table is given, the selection will be emptied]]):set_attribute("optional",true)
darktable.gui.selection:add_return("table",[[a table containing the selection as it was before the function was called]])
----------------------
--  DARKTABLE.TAGS  --
----------------------
darktable.tags:set_text([[allows access to all existing tags]])

darktable.tags["#"]:set_text([[each existing tag has a numeric entry in the tags table. use ipairs to iterate over them]])
darktable.tags.create:set_text([[creates a new tag and return it. If the tag exists, return the existing tag]])
darktable.tags.create:add_parameter("name","string",[[the name of the new tag]])
darktable.tags.find:set_text([[returns the tag object or nil if the tag doesn't exist]])
darktable.tags.find:add_parameter("name","string",[[the name of the tag to find]])
darktable.tags.find:add_return(types.dt_lua_tag_t,[[the tag object or nil]])
darktable.tags.delete:set_text([[deletes the tag object, detaching it from all images]])
darktable.tags.delete:add_parameter("tag",types.dt_lua_tag_t,[[the tag to be deleted]])
darktable.tags.delete:set_main_parent(darktable.tags)
darktable.tags.attach:set_text([[attach a tag to an image, the order of the parameters can be reversed]])
darktable.tags.attach:add_parameter("tag",types.dt_lua_tag_t,[[the tag to be attached]])
darktable.tags.attach:add_parameter("image",types.dt_lua_image_t,[[the image to attach the tag to]])
darktable.tags.attach:set_main_parent(darktable.tags)
darktable.tags.detach:set_text([[detach a tag from an image, the order of the parameters can be reversed]])
darktable.tags.detach:add_parameter("tag",types.dt_lua_tag_t,[[the tag to be detached]])
darktable.tags.detach:add_parameter("image",types.dt_lua_image_t,[[the image to detach the tag from]])
darktable.tags.detach:set_main_parent(darktable.tags)
darktable.tags.get_tags:set_text([[gets all tags attached to an image]])
darktable.tags.get_tags:add_parameter("image",types.dt_lua_image_t,[[the image to get the tags from]])
darktable.tags.get_tags:add_return("table",[[a table of tags that are attached to the image]])
darktable.tags.get_tags:set_main_parent(darktable.tags)

------------------------------
--  DARKTABLE.CONFIGURATION --
------------------------------
darktable.configuration:set_text([[this table regroups values that describe details of the configuration of darktable]])
darktable.configuration.version:set_text([[the version number of darktable]])
darktable.configuration.has_gui:set_text([[true if darktable has a gui (launched through the main darktable command, not darktable-cli]])
darktable.configuration.verbose:set_text([[true if the lua logdomain is enabled]])
darktable.configuration.tmp_dir:set_text([[the name of the directory where darktable will store temporary files]])
darktable.configuration.config_dir:set_text([[the name of the directory where darktable will find its global configuration objects (modules)]])
darktable.configuration.cache_dir:set_text([[the name of the directory where darktable will store its mipmaps]])

-----------------------------
--  DARKTABLE.PREFERENCES  --
-----------------------------
darktable.preferences:set_text([[Lua allows you do manipulate preferences. Lua has its own namespace for preferences and you can't access nor write normal darktable preferences.

preference-handling functions take a _script_ parameter. This is a string used to avoid name collision in preferences (i.e namespace) . Set it to something unique, usually the name of the script handling the preference

preference-handling functions can't guess the type of a parameter. You must pass the type of the preference you are handling. Allowed values are the following strings

* string
* bool
* integer]])

darktable.preferences.register:set_text([[creates a new preference entry in the lua tab of the preference screen. If this function is not called the preference can't be set by the user (you can still read and write invisible preferences)]])
darktable.preferences.register:add_parameter("script","string",[[invisible prefix to guarantee unicity of preferences]])
darktable.preferences.register:add_parameter("name","string",[[a unique name used with the script part to identify the preference]])
darktable.preferences.register:add_parameter("type","string",[[the type of the preference, one of the string values described above]])
darktable.preferences.register:add_parameter("label","string",[[the label displayed in the preference screeen]])
darktable.preferences.register:add_parameter("tooltip","string",[[the tooltip to display in the preference menu]])
darktable.preferences.register:add_parameter("default","depends on type",[[default value to use when not set explicitely or by the user]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("min","int",[[(integer preferences only) minimum value]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("max","int",[[(integer preferences only) maximum value]]):set_attribute("optional",true)
    
darktable.preferences.read:set_text([[reads a value from a lua preference]])
darktable.preferences.read:add_parameter("script","string",[[invisible prefix to guarantee unicity of preferences]])
darktable.preferences.read:add_parameter("name","string",[[the name of the preference displayed in the preference screeen]])
darktable.preferences.read:add_parameter("type","string",[[the type of the preference, one of the string values described above]])
darktable.preferences.read:add_return("depends on type",[[the value of the preference]])
    
darktable.preferences.write:set_text([[writes a value to a lua preference.]])
darktable.preferences.write:add_parameter("script","string",[[invisible prefix to guarantee unicity of preferences]])
darktable.preferences.write:add_parameter("name","string",[[the name of the preference displayed in the preference screeen]])
darktable.preferences.write:add_parameter("type","string",[[the type of the preference, one of the string values described above]])
darktable.preferences.write:add_parameter("value","depends on type",[[the value to set the preference to]])


-----------------------
--  DARKTABLE.STYLES --
-----------------------

darktable.styles:set_text([[Allow you to access and manipulate styles]])

darktable.styles["#"]:set_text([[each existing style has a numeric index, you can iterate them using ipairs]])

darktable.styles.create:set_text([[create a new style based on an image]])
darktable.styles.create:add_parameter("image",types.dt_lua_image_t,[[the image to create the style from]])
darktable.styles.create:add_parameter("name","string",[[the name to give to the new style]])
darktable.styles.create:add_parameter("description","string",[[the description of the new style]]):set_attribute("optional")
darktable.styles.create:add_return(types.dt_style_t,[[the new style object]])
darktable.styles.create:set_main_parent(darktable.styles)

darktable.styles.delete:set_text([[deletes an existing style]])
darktable.styles.delete:add_parameter("style",types.dt_style_t,[[the style to delete]])
darktable.styles.delete:set_main_parent(darktable.styles)

darktable.styles.duplicate:set_text([[create a new style based on an existing style]])
darktable.styles.duplicate:add_parameter("style",types.dt_style_t,[[the style to base the new style on]])
darktable.styles.duplicate:add_parameter("name","string",[[the new style's name]])
darktable.styles.duplicate:add_parameter("description","string",[[the new style's description]]):set_attribute("optional")
darktable.styles.duplicate:add_return(types.dt_style_t,[[the new style object]])
darktable.styles.duplicate:set_main_parent(darktable.styles)

darktable.styles.apply:set_text([[apply a style to an image, order of parameters can be inverted]])
darktable.styles.apply:add_parameter("style",types.dt_style_t,[[the style to use]])
darktable.styles.apply:add_parameter("style",types.dt_lua_image_t,[[the image to apply the style to]])
darktable.styles.apply:set_main_parent(darktable.styles)

-------------------------
--  DARKTABLE.DATABASE --
-------------------------

darktable.database:set_text([[Allows to access the database of images, note that duplicate images (images with the same RAW but different XMP) will appear multiple times with different duplicate indexes. Also note that all images are here. This table is not influenced by any GUI filtering (collections, stars etc...)]])


darktable.database["#"]:set_text([[each image in the database appears with a numerical index, use ipairs to iterate]])
darktable.database.duplicate:set_text([[creates a duplicate of an image and returns it]])
darktable.database.duplicate:add_parameter("image",types.dt_lua_image_t,[[the image to duplicate]])
darktable.database.duplicate:add_return(types.dt_lua_image_t,[[the new image object]])
darktable.database.duplicate:set_main_parent(darktable.database)

darktable.database.import:set_text([[imports new images into the database]])
darktable.database.import:add_parameter("location","string",[[the filename or directory to import images from

NOTE : if the image are set to be imported recursively in preferences, only the toplevel film is returned (the whose path was given as a parameter) 

NOTE2 : if the parameter is a directory, the call is not blocking. The film object will not have the newly imported images yet. Use a post-import-film filtering on that film to react when images are actually imported 


]])
darktable.database.duplicate:add_return(types.dt_lua_image_t,[[the created image if an image is imported, the toplevel film object if a film was imported]])

------------------------
--  DARKTABLE.MODULES --
------------------------

darktable.modules:set_text([[This table describe the different loadable modules of darktable]])

darktable.modules.format:set_text([[functions to get parameter objects for the different export formats.]])

darktable.modules.format.png:set_text([[used to get a new png format object]])
darktable.modules.format.png:add_return(types.dt_imageio_module_format_data_png,[[a new format object describing the parameters to export to png. Initialised to the values contained in the gui]])

darktable.modules.format.png:set_alias(darktable.modules.format.tiff)
darktable.modules.format.png:set_alias(darktable.modules.format.exr)
darktable.modules.format.png:set_alias(darktable.modules.format.copy)
darktable.modules.format.png:set_alias(darktable.modules.format.pfm)
darktable.modules.format.png:set_alias(darktable.modules.format.jpeg)
darktable.modules.format.png:set_alias(darktable.modules.format.ppm)
darktable.modules.format.png:set_alias(darktable.modules.format.webp)
darktable.modules.format.png:set_alias(darktable.modules.format.j2k)

darktable.modules.storage:set_text([[functions to get parameter objects for the different export storages.

New values may appear in this table if new storages are registered using lua.]])
darktable.modules.storage.email:set_text([[used to get a new email storage object]])
darktable.modules.storage.email:add_return(types.dt_imageio_module_storage,[[a new storage object describing the parameters to export with. Initialised to the values contained in the gui]])
darktable.modules.storage.email:set_alias(darktable.modules.storage.latex)
darktable.modules.storage.email:set_alias(darktable.modules.storage.disk)
darktable.modules.storage.email:set_alias(darktable.modules.storage.gallery)
darktable.modules.storage.email:set_alias(darktable.modules.storage.flickr)
darktable.modules.storage.email:set_alias(darktable.modules.storage.facebook)
darktable.modules.storage.email:set_alias(darktable.modules.storage.picasa)

----------------------
--  DARKTABLE.DEBUG --
----------------------
darktable.debug:set_text([[This section must be activated separately by calling 
<pre>
require "darktable.debug"
</pre>]])

darktable.debug.dump:set_text([[will return a string describing everything lua knows about object, used to know what an object is.

this function is recursion-safe and can be used to dump _G if needed]])
darktable.debug.dump:add_parameter("object","anything",[[The object to dump]])
darktable.debug.dump:add_parameter("name","string",[[A name to use for the object]]):set_attribute("optional",true)
darktable.debug.dump:add_return("string",[[a string containing a text description of the object, can be very long]])

darktable.debug.debug:set_text([[initialized to false, set it to true to also dump information about metatables]])
darktable.debug.type:set_text([[similar to the system function type() but it will return the real type instead of "userdata" for darktable specific objects]])
darktable.debug.type:add_parameter("object","anything",[[the object object who's type must be reported]])
darktable.debug.type:add_return("string",[[a string describing the type of the object]])
	

----------------------
--  TYPES           --
----------------------
types:set_text([[This section documents types that are specific to darktable's lua API]])


types.dt_lua_image_t:set_text([[Image objects represent an image in the database. This is slightly different from a file on disk since a file can have multiple developements.

Note that this is the real image object, changing the value of a field will immediately change it in darktable and will be reflected on any copy of that image object you may have kept.]])


types.dt_lua_image_t.id:set_text([[a unique id identifying the image in the database]])
types.dt_lua_image_t.path:set_text([[the file the directory containing the image]])
types.dt_lua_image_t.film:set_text([[the film object that contains this image]])
types.dt_lua_image_t.filename:set_text([[the filename of the image]])
types.dt_lua_image_t.duplicate_index:set_text([[if there are multiple images based on a same file, each will have a unique number, starting from 0]])


types.dt_lua_image_t.publisher:set_text([[the publisher field of the image]])
types.dt_lua_image_t.title:set_text([[the title field of the image]])
types.dt_lua_image_t.creator:set_text([[the creator field of the image]])
types.dt_lua_image_t.rights:set_text([[the rights field of the image]])
types.dt_lua_image_t.description:set_text([[the description field for the image]])

types.dt_lua_image_t.exif_maker:set_text([[the maker exif data]])
types.dt_lua_image_t.exif_model:set_text([[the camera model used]])
types.dt_lua_image_t.exif_lens:set_text([[the id string of the lens used]])
types.dt_lua_image_t.exif_aperture:set_text([[the aperture saved in the exif data]])
types.dt_lua_image_t.exif_exposure:set_text([[the exposure time of the image]])
types.dt_lua_image_t.exif_focal_length:set_text([[the focal lens of the image]])
types.dt_lua_image_t.exif_iso:set_text([[the iso used on the image]])
types.dt_lua_image_t.exif_datetime_taken:set_text([[the date and time of the image]])
types.dt_lua_image_t.exif_focus_distance:set_text([[the distance of the subject]])
types.dt_lua_image_t.exif_crop:set_text([[the exif crop data]])
types.dt_lua_image_t.latitude:set_text([[GPS data for the image]])
types.dt_lua_image_t.longitude:set_text([[the GPS data for the image]])
types.dt_lua_image_t.is_raw:set_text([[true if the image is a RAW file]])
types.dt_lua_image_t.is_ldr:set_text([[true if the image is ldr]])
types.dt_lua_image_t.is_hdr:set_text([[true if the image is a hdr image]])
types.dt_lua_image_t.width:set_text([[the width of the image]])
types.dt_lua_image_t.height:set_text([[the height of the image]])
types.dt_lua_image_t.rating:set_text([[the rating of the image (-1 for rejected)]])
types.dt_lua_image_t.red:set_text([[true if the image has the corresponding colorlabel]])
types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.blue)
types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.green)
types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.yellow)
types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.purple)

types.dt_lua_image_t.group_with:set_text([[puts the first image in the same group as the second image. If no second image is provided, the image will be in its own group]])
types.dt_lua_image_t.group_with:add_parameter("image",types.dt_lua_image_t,[[the image whose group must be changed]])
types.dt_lua_image_t.group_with:add_parameter("image2",types.dt_lua_image_t,[[the image we want to group with]]):set_attribute("optional",true)
types.dt_lua_image_t.make_group_leader:set_text([[makes the image the leader of its group]])
types.dt_lua_image_t.make_group_leader:add_parameter("image",types.dt_lua_image_t,[[the image we want as the leader]])
types.dt_lua_image_t.get_group_members:set_text([[returns a table containing all images of the group. The group leader is both at a numeric key and at the "leader" special key (so you probably want to use ipairs to iterate through that table)]])
types.dt_lua_image_t.get_group_members:add_parameter("image",types.dt_lua_image_t,[[the image whose group we are querying]])
types.dt_lua_image_t.get_group_members:add_return("table",[[A table of image objects containing all images that are in the same group as the image]])
darktable.tags.attach:set_alias(types.dt_lua_image_t.attach_tag)
types.dt_lua_image_t.group_leader:set_text([[The image which is the leader of the group this image is a member of]])

types.dt_imageio_module_format:set_text([[A virtual type representing all format types]])
types.dt_imageio_module_format.plugin_name:set_text([[a unique name for the plugin]])
types.dt_imageio_module_format.name:set_text([[a human readable name for the plugin]])
types.dt_imageio_module_format.extension:set_text([[the typical filename extension for that format]])
types.dt_imageio_module_format.mime:set_text([[the mime type associated with the format]])
types.dt_imageio_module_format.max_width:set_text([[the max width allowed for the format ( 0 : unlimited )]])
types.dt_imageio_module_format.max_height:set_text([[the max height allowed for the format ( 0 : unlimited )]])
types.dt_imageio_module_format.write_image:set_text([[exports an image to a file. This is a blocking operation that will not return until the image is exported.]])
types.dt_imageio_module_format.write_image:add_parameter("format",types.dt_imageio_module_format,[[The format that will be used to export]])
types.dt_imageio_module_format.write_image:add_parameter("image",types.dt_lua_image_t,[[The image object to export]])
types.dt_imageio_module_format.write_image:add_parameter("filename","string",[[The filename to export to]])
types.dt_imageio_module_format.write_image:add_return("boolean",[[True on success]])

types.dt_imageio_module_format_data_png:set_text([[type object describing parameters to export to png]])
types.dt_imageio_module_format_data_png.bpp:set_text([[the bpp parameter to use when exporting]])
types.dt_imageio_module_format_data_tiff:set_text([[type object describing parameters to export to tiff]])
types.dt_imageio_module_format_data_tiff.bpp:set_text([[the bpp parameter to use when exporting]])
types.dt_imageio_module_format_data_exr:set_text([[type object describing parameters to export to exr]])
types.dt_imageio_module_format_data_copy:set_text([[type object describing parameters to export to copy]])
types.dt_imageio_module_format_data_pfm:set_text([[type object describing parameters to export to pfm]])
types.dt_imageio_module_format_data_jpeg:set_text([[type object describing parameters to export to jpeg]])
types.dt_imageio_module_format_data_jpeg.quality:set_text([[the quality to use at export time]])
types.dt_imageio_module_format_data_ppm:set_text([[type object describing parameters to export to ppm]])


types.dt_imageio_module_storage:set_text([[A virtual type representing all storage types]])
types.dt_imageio_module_storage.plugin_name:set_text([[a unique name for the plugin]])
types.dt_imageio_module_storage.name:set_text([[a human readable name for the plugin]])
types.dt_imageio_module_storage.width:set_text([[the currently selected width for the plugin]])
types.dt_imageio_module_storage.height:set_text([[the currently selected height for the plugin]])
types.dt_imageio_module_storage.recommended_width:set_text([[the recommended width for the plugin]])
types.dt_imageio_module_storage.recommended_height:set_text([[the recommended height for the plugin]])
types.dt_imageio_module_storage.supports_format:set_text([[checks if a format is supported by this storage]])
types.dt_imageio_module_storage.supports_format:add_parameter("storage",types.dt_imageio_module_storage,[[the storage type to check against]])
types.dt_imageio_module_storage.supports_format:add_parameter("format",types.dt_imageio_module_format,[[the format type to check]])
types.dt_imageio_module_storage.supports_format:add_return("boolean",[[true if the format is supported by the storage]])

types.dt_imageio_module_storage_data_flickr:set_text([[An object containing parameters to export to flickr]])
types.dt_imageio_module_storage_data_facebook:set_text([[An object containing parameters to export to facebook]])
types.dt_imageio_module_storage_data_latex:set_text([[An object containing parameters to export to latex]])
types.dt_imageio_module_storage_data_latex.filename:set_text([[The filename to export to]])
types.dt_imageio_module_storage_data_latex.title:set_text([[The title to use for export]])
types.dt_imageio_module_storage_data_picasa:set_text([[An object containing parameters to export to picasa]])
types.dt_imageio_module_storage_data_gallery:set_text([[An object containing parameters to export to gallery]])
types.dt_imageio_module_storage_data_gallery.filename:set_text([[The filename to export to]])
types.dt_imageio_module_storage_data_gallery.title:set_text([[The title to use for export]])
types.dt_imageio_module_storage_data_disk:set_text([[An object containing parameters to export to disk]])
types.dt_imageio_module_storage_data_disk.filename:set_text([[The filename to export to]])

types.dt_lua_film_t:set_text([[a film in darktable. This represents a directory containing imported images]])
types.dt_lua_film_t["#"]:set_text([[the different images within the film]])
types.dt_lua_film_t.id:set_text([[a unique numeric id used by this film]])
types.dt_lua_film_t.path:set_text([[the path represented by this film]])

types.dt_style_t:set_text([[A style that can be applied to an image]])
types.dt_style_t.name:set_text([[The name of the style]])
types.dt_style_t.description:set_text([[The description of the style]])
types.dt_style_t["#"]:set_text([[The different items that make the style]])

types.dt_style_item_t:set_text([[An element that is part of a style]])
types.dt_style_item_t.name:set_text([[The name of the style item]])
types.dt_style_item_t.num:set_text([[The position of the style item within its style]])

types.dt_lua_tag_t:set_text([[A tag that can be attached to an image]])
types.dt_lua_tag_t.name:set_text([[The name of the tag]])
types.dt_lua_tag_t["#"]:set_text([[The images that have that tag attached to them]])

----------------------
--  EVENTS          --
----------------------
events:set_text([[This section documents events that can be used to trigger lua callbacks]])


events["intermediate-export-image"]:set_text([[This event is called each time an image is exported, once for each image after the image has been processed to an image format but before the storage has moved the image to its final destination]])
events["intermediate-export-image"].callback:add_parameter("event","string",[[The name of the event that triggered the callback]])
events["intermediate-export-image"].callback:add_parameter("image",types.dt_lua_image_t,[[The image object that has been exporte]])
events["intermediate-export-image"].callback:add_parameter("filename","string",[[The name of the file that is the result of the image being porcessed]])
events["intermediate-export-image"].extra_registration_parameters:set_text([[This event has no extra registration parameters]])


events["post-import-image"]:set_text([[This event is triggered whenever a new image is imported into the database.

This event can be registered multiple times, all callbacks will be called]])
events["post-import-image"].callback:add_parameter("event","string",[[The name of the event that triggered the callback]])
events["post-import-image"].callback:add_parameter("image",types.dt_lua_image_t,[[The image object that has been exporte]])
events["post-import-image"].extra_registration_parameters:set_text([[This event has no extra registration parameters]])


events["shortcut"]:set_text([[This event registers a new keyboad shortcut. The shortcut isn't bound to any key until the users does so in the preference panel.

The event is triggered whenever the shortcut is triggered


This event can only be registered once per value of shortcut
]])
events["shortcut"].callback:add_parameter("event","string",[[The name of the event that triggered the callback]])

events["shortcut"].callback:add_parameter("shortcut","string",[[The tooltip string that was given at registration time]])
events["shortcut"].extra_registration_parameters:set_text("")
events["shortcut"].extra_registration_parameters:add_parameter("tooltip","string",[[The string that will be displayed on the shortcut preference panel describing the shortcut]])


events["post-import-film"]:set_text([[This event is triggered when an film import is finished (all post-import-image have already been triggered) This event can be registered multiple times.
]])
events["post-import-film"].callback:add_parameter("event","string",[[The name of the event that triggered the callback]])

events["post-import-film"].callback:add_parameter("film",types.dt_lua_film_t,[[The new film that has been added. If multiple films were added recursively, only the top level film is reported]])
events["post-import-film"].extra_registration_parameters:set_text([[This event has no extra registration parameters]])
----------------------
--  ATTRIBUTES      --
----------------------
attributes:set_text([[This section documents various attributes used throughout the documentation]])
attributes.ret_val:set_skiped()
attributes.signature:set_skiped()
attributes.reported_type:set_skiped()
attributes.is_singleton:set_skiped()
attributes.optional:set_skiped()
attributes.skiped:set_skiped()
attributes.is_attribute:set_skiped()
attributes.write:set_text([[This object is a variable that can be written to]])
attributes.read:set_text([[This object is a variable that can be read]])
attributes.has_pairs:set_text([[This object can be used as an argument to the system function "pairs" and iterated upon]])
attributes.has_ipairs:set_text([[This object can be used as an argument to the system function "ipairs" and iterated upon]])
attributes.has_equal:set_text([[This object has a specific comparison function that will be used when comparing it to an object of the same type]])
attributes.has_length:set_text([[This object has a specific length function that will be used by the # operator]])
attributes.has_tostring:set_text([[This object has a specific reimplementation of the "tostring" method that allows pretty-printing it]])

