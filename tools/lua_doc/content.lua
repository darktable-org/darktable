real_darktable = require "darktable"
require "darktable.debug"
local tmp_node

local function sorted_pairs (t, f)
  local a = {}
  for n in pairs(t) do table.insert(a, n) end
  table.sort(a, f)
  local i = 0      -- iterator variable
  local iter = function ()   -- iterator function
    i = i + 1
    if a[i] == nil then return nil
    else return a[i], t[a[i]]
    end
  end
  return iter
end


---------------------
-- check for generator functions
---------------------
for _,v in pairs({"node_to_string","para","startlist","listel","endlist","code","emphasis","url"})   do
	if _ENV[v]== nil then
		error("function '"..v.."' not defined when requiring content")
	end
end
---------------------
-- check for database content
---------------------
if  #real_darktable.database == 0 then
	error("The database needs to contain at least one image to generate documentation")
end
if  #real_darktable.styles == 0 then
	error("The database needs to contain at least one style to generate documentation")
end

real_darktable.gui.libs.collect.filter({})

doc = require "core"
darktable = doc.toplevel.darktable
types = doc.toplevel.types
events = doc.toplevel.events
attributes = doc.toplevel.attributes


local function my_tostring(obj)
	if not obj then
		error("incorrect object")
	end
	return tostring(obj)
end

local function remove_all_children(node)
	for k, v in node:all_children() do
		v:remove_parent(node)
		node[k] = nil
	end
end
-- prevent some objects to appear at the wrong end of the tree
remove_all_children(types.dt_lua_lib_t.views)

----------------------
--  REANAMINGS      --
----------------------

----------------------
--  TOPLEVEL        --
----------------------
local prefix
if real_darktable.configuration.api_version_suffix ~= "" then
  prefix = [[This documentation is for the *development* version of darktable. for the stable version, please visit the user manual]]..para()
else
  prefix = ""
end
doc.toplevel:set_text(prefix..[[To access the darktable specific functions you must load the darktable environment:]]..
code([[darktable = require "darktable"]])..
[[All functions and data are accessed through the darktable module.]]..para()..
[[This documentation for API version ]]..real_darktable.configuration.api_version_string..[[.]])
----------------------
--  DARKTABLE       --
----------------------
darktable:set_text([[The darktable library is the main entry point for all access to the darktable internals.]])
darktable.print:set_text([[Will print a string to the darktable control log (the long overlaid window that appears over the main panel).]])
darktable.print:add_parameter("message","string",[[The string to display which should be a single line.]])

darktable.print_log:set_text([[This function will print its parameter if the Lua logdomain is activated. Start darktable with the "-d lua" command line option to enable the Lua logdomain.]])
darktable.print_log:add_parameter("message","string",[[The string to display.]])
darktable.print_error:set_text([[This function is similar to]]..my_tostring(darktable.print_log)..[[ but adds an ERROR prefix for clarity.]])
darktable.print_error:add_parameter("message","string",[[The string to display.]])

darktable.register_event:set_text([[This function registers a callback to be called when a given event happens.]]..para()..
[[Events are documented ]]..node_to_string(events,[[in the event section.]]))
darktable.register_event:add_parameter("event_type","string",[[The name of the event to register to.]])
darktable.register_event:add_parameter("callback","function",[[The function to call on event. The signature of the function depends on the type of event.]])
darktable.register_event:add_parameter("...","variable",[[Some events need extra parameters at registration time; these must be specified here.]])

darktable.register_storage:set_text([[This function will add a new storage implemented in Lua.]]..para()..
[[A storage is a module that is responsible for handling images once they have been generated during export. Examples of core storages include filesystem, e-mail, facebook...]])
darktable.register_storage:add_parameter("plugin_name","string",[[A Unique name for the plugin.]])
darktable.register_storage:add_parameter("name","string",[[A human readable name for the plugin.]])
tmp_node = darktable.register_storage:add_parameter("store","function",[[This function is called once for each exported image. Images can be exported in parallel but the calls to this function will be serialized.]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage_t,[[The storage object used for the export.]])
tmp_node:add_parameter("image",types.dt_lua_image_t,[[The exported image object.]])
tmp_node:add_parameter("format",types.dt_imageio_module_format_t,[[The format object used for the export.]])
tmp_node:add_parameter("filename","string",[[The name of a temporary file where the processed image is stored.]])
tmp_node:add_parameter("number","integer",[[The number of the image out of the export series.]])
tmp_node:add_parameter("total","integer",[[The total number of images in the export series.]])
tmp_node:add_parameter("high_quality","boolean",[[True if the export is high quality.]])
tmp_node:add_parameter("extra_data","table",[[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export series.]])
tmp_node = darktable.register_storage:add_parameter("finalize","function",[[This function is called once all images are processed and all store calls are finished.]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage_t,[[The storage object used for the export.]])
tmp_node:add_parameter("image_table","table",[[A table keyed by the exported image objects and valued with the corresponding temporary export filename.]])
tmp_node:add_parameter("extra_data","table",[[An empty Lua table to store extra data. This table is common to all calls to store and the call to finalize in a given export series.]])
tmp_node = darktable.register_storage:add_parameter("supported","function",[[A function called to check if a given image format is supported by the Lua storage; this is used to build the dropdown format list for the GUI.]]..para()..
[[Note that the parameters in the format are the ones currently set in the GUI; the user might change them before export.]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage_t,[[The storage object tested.]])
tmp_node:add_parameter("format",types.dt_imageio_module_format_t,[[The format object to report about.]])
tmp_node:add_return("boolean",[[True if the corresponding format is supported.]])
tmp_node = darktable.register_storage:add_parameter("initialize","function",[[A function called before storage happens]]..para()..
[[This function can change the list of exported functions]])
tmp_node:set_attribute("optional",true)
tmp_node:add_parameter("storage",types.dt_imageio_module_storage_t,[[The storage object tested.]])
tmp_node:add_parameter("format",types.dt_imageio_module_format_t,[[The format object to report about.]])
tmp_node:add_parameter("images","table of "..my_tostring(types.dt_lua_image_t),[[A table containing images to be exported.]])
tmp_node:add_parameter("high_quality","boolean",[[True if the export is high quality.]])
tmp_node:add_parameter("extra_data","table",[[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export series.]])
tmp_node:add_return("table or nil",[[The modified table of images to export or nil]]..para()..
[[If nil (or nothing) is returned, the original list of images will be exported]]..para()..
[[If a table of images is returned, that table will be used instead. The table can be empty. The images parameter can be modified and returned]])
darktable.register_storage:add_parameter("widget",types.lua_widget,[[A widget to display in the export section of darktable's UI]]):set_attribute("optional",true)
darktable.register_lib:set_text("Register a new lib object. A lib is a graphical element of darktable's user interface")
darktable.register_lib:add_parameter("plugin_name","string","A unique name for your library")
darktable.register_lib:add_parameter("name","string","A user-visible name for your library")
darktable.register_lib:add_parameter("expandable","boolean","whether this lib should be expandable or not")
darktable.register_lib:add_parameter("resettable","boolean","whether this lib has a reset button or not")
darktable.register_lib:add_parameter("containers","table of "..my_tostring(types.dt_lua_view_t).." => [ "..my_tostring(types.dt_ui_container_t)..", int ]","A table associating to each view containing the lib the corresponding container and position")
darktable.register_lib:add_parameter("widget",types.lua_widget,"The widget to display in the lib")
tmp = darktable.register_lib:add_parameter("view_enter","function","A callback called when a view displaying the lib is entered")
tmp:add_parameter("self",types.dt_lua_lib_t,"The lib on which the callback is called"):set_attribute("is_self",true)
tmp:add_parameter("old_view",types.dt_lua_view_t,"The view that we are leaving")
tmp:add_parameter("new_view",types.dt_lua_view_t,"The view that we are entering")
tmp = darktable.register_lib:add_parameter("view_leave","function","A callback called when leaving a view displaying the lib")
tmp:add_parameter("self",types.dt_lua_lib_t,"The lib on which the callback is called"):set_attribute("is_self",true)
tmp:add_parameter("old_view",types.dt_lua_view_t,"The view that we are leaving")
tmp:add_parameter("new_view",types.dt_lua_view_t,"The view that we are entering")



darktable.films:set_text([[A table containing all the film objects in the database.]])
darktable.films['#']:set_text([[Each film has a numeric entry in the database.]])
darktable.films.new:set_text([[Creates a new empty film]]..para()..
[[ see ]]..my_tostring(darktable.database.import)..[[ to import a directory with all its images and to add images to a film]])
darktable.films.new:add_parameter("directory","string",[[The directory that the new film will represent. The directory must exist]])
darktable.films.new:add_return(types.dt_lua_film_t,"The newly created film, or the existing film if the directory is already imported")

darktable.new_format:set_text("Creates a new format object to export images")
tmp =""
for k,v in sorted_pairs(debug.getregistry().dt_lua_modules.format) do
  tmp = tmp..listel(k)
end
darktable.new_format:add_parameter("type","string",[[The type of format object to create, one of : ]]..  startlist().. tmp..endlist())
darktable.new_format:add_return(types.dt_imageio_module_format_t,"The newly created object. Exact type depends on the type passed")

darktable.new_storage:set_text("Creates a new storage object to export images")
tmp =""
for k,v in sorted_pairs(debug.getregistry().dt_lua_modules.storage) do
  tmp = tmp..listel(k)
end
darktable.new_storage:add_parameter("type","string",[[The type of storage object to create, one of : ]]..  startlist().. tmp..endlist().."(Other, lua-defined, storage types may appear.)")
darktable.new_storage:add_return(types.dt_imageio_module_storage_t,"The newly created object. Exact type depends on the type passed")

darktable.new_widget:set_text("Creates a new widget object to display in the UI")
tmp =""
for k,v in sorted_pairs(debug.getregistry().dt_lua_modules.widget) do
  tmp = tmp..listel(k)
end
darktable.new_widget:add_parameter("type","string",[[The type of storage object to create, one of : ]]..  startlist().. tmp..endlist())
darktable.new_widget:add_parameter("...","variable",[[Extra parameters, exact value are documented with each type]])
darktable.new_widget:add_return(types.lua_widget,"The newly created object. Exact type depends on the type passed")
----------------------
--  DARKTABLE.GUI   --
----------------------
darktable.gui:set_text([[This subtable contains function and data to manipulate the darktable user interface with Lua.]]..para()..
[[Most of these function won't do anything if the GUI is not enabled (i.e you are using the command line version darktable-cli instead of darktable).]])

darktable.gui.action_images:set_text([[A table of ]]..my_tostring(types.dt_lua_image_t)..[[ on which the user expects UI actions to happen.]]..para()..
[[It is based on both the hovered image and the selection and is consistent with the way darktable works.]]..para()..
[[It is recommended to use this table to implement Lua actions rather than ]]..my_tostring(darktable.gui.hovered)..[[ or ]]..my_tostring(darktable.gui.selection)..[[ to be consistent with darktable's GUI.]])

remove_all_children(darktable.gui.action_images)

darktable.gui.hovered:set_text([[The image under the cursor or nil if no image is hovered.]])
darktable.gui.hovered:set_reported_type(types.dt_lua_image_t)
darktable.gui.selection:set_text([[Allows to change the set of selected images.]])
darktable.gui.selection:add_parameter("selection","table of "..my_tostring(types.dt_lua_image_t),[[A table of images which will define the selected images. If this parameter is not given the selection will be untouched. If an empty table is given the selection will be emptied.]]):set_attribute("optional",true)
darktable.gui.selection:add_return("table of "..my_tostring(types.dt_lua_image_t),[[A table containing the selection as it was before the function was called.]])
darktable.gui.selection:set_attribute("implicit_yield",true)
darktable.gui.current_view:set_text([[Allows to change the current view.]])
darktable.gui.current_view:add_parameter("view",types.dt_lua_view_t,[[The view to switch to. If empty the current view is unchanged]]):set_attribute("optional",true)
darktable.gui.current_view:add_return(types.dt_lua_view_t,[[the current view]])
darktable.gui.create_job:set_text([[Create a new progress_bar displayed in ]]..my_tostring(darktable.gui.libs.backgroundjobs))
darktable.gui.create_job:add_parameter("text","string",[[The text to display in the job entry]])
darktable.gui.create_job:add_parameter("percentage","boolean",[[Should a progress bar be displayed]]):set_attribute("optional",true)
tmp = darktable.gui.create_job:add_parameter("cancel_callback","function",[[A function called when the cancel button for that job is pressed]]..para().."note that the job won't be destroyed automatically. You need to set "..my_tostring(types.dt_lua_backgroundjob_t.valid).." to false for that")
tmp:set_attribute("optional",true)
tmp:add_parameter("job",types.dt_lua_backgroundjob_t,[[The job who is being cancelled]])
darktable.gui.create_job:add_return(types.dt_lua_backgroundjob_t,[[The newly created job object]])

-------------------------
--  DARKTABLE.GUIDES   --
-------------------------
darktable.guides:set_text([[Guide lines to overlay over an image in crop and rotate.]]..para()..[[All guides are clipped to the drawing area.]])
darktable.guides.register_guide:set_text([[Register a new guide.]])
darktable.guides.register_guide:add_parameter("name", "string", [[The name of the guide to show in the GUI.]])
tmp_node = darktable.guides.register_guide:add_parameter("draw_callback", "function", [[The function to call to draw the guide lines. The drawn lines will be stroked by darktable.]]..para()..[[THIS IS RUNNING IN THE GUI THREAD AND HAS TO BE FAST!]])
tmp_node:add_parameter("cr", types.dt_lua_cairo_t, [[The cairo object used for drawing.]])
tmp_node:add_parameter("x", "float", [[The x coordinate of the top left corner of the drawing area.]])
tmp_node:add_parameter("y", "float", [[The y coordinate of the top left corner of the drawing area.]])
tmp_node:add_parameter("width", "float", [[The width of the drawing area.]])
tmp_node:add_parameter("height", "float", [[The height of the drawing area.]])
tmp_node:add_parameter("zoom_scale", "float", [[The current zoom_scale. Only needed when setting the line thickness.]])
darktable.guides.register_guide:add_parameter("gui_callback", "function", [[A function returning a widget to show when the guide is selected. It takes no arguments.]]):set_attribute("optional",true)

----------------------
--  DARKTABLE.TAGS  --
----------------------
darktable.tags:set_text([[Allows access to all existing tags.]])

darktable.tags["#"]:set_text([[Each existing tag has a numeric entry in the tags table - use ipairs to iterate over them.]])
darktable.tags.create:set_text([[Creates a new tag and return it. If the tag exists return the existing tag.]])
darktable.tags.create:add_parameter("name","string",[[The name of the new tag.]])
darktable.tags.find:set_text([[Returns the tag object or nil if the tag doesn't exist.]])
darktable.tags.find:add_parameter("name","string",[[The name of the tag to find.]])
darktable.tags.find:add_return(types.dt_lua_tag_t,[[The tag object or nil.]])
darktable.tags.delete:set_text([[Deletes the tag object, detaching it from all images.]])
darktable.tags.delete:add_parameter("tag",types.dt_lua_tag_t,[[The tag to be deleted.]])
darktable.tags.delete:set_main_parent(darktable.tags)
darktable.tags.attach:set_text([[Attach a tag to an image; the order of the parameters can be reversed.]])
darktable.tags.attach:add_parameter("tag",types.dt_lua_tag_t,[[The tag to be attached.]])
darktable.tags.attach:add_parameter("image",types.dt_lua_image_t,[[The image to attach the tag to.]])
darktable.tags.attach:set_main_parent(darktable.tags)
darktable.tags.detach:set_text([[Detach a tag from an image; the order of the parameters can be reversed.]])
darktable.tags.detach:add_parameter("tag",types.dt_lua_tag_t,[[The tag to be detached.]])
darktable.tags.detach:add_parameter("image",types.dt_lua_image_t,[[The image to detach the tag from.]])
darktable.tags.detach:set_main_parent(darktable.tags)
darktable.tags.get_tags:set_text([[Gets all tags attached to an image.]])
darktable.tags.get_tags:add_parameter("image",types.dt_lua_image_t,[[The image to get the tags from.]])
darktable.tags.get_tags:add_return("table of "..my_tostring(types.dt_lua_tag_t),[[A table of tags that are attached to the image.]])
darktable.tags.get_tags:set_main_parent(darktable.tags)

------------------------------
--  DARKTABLE.CONFIGURATION --
------------------------------
darktable.configuration:set_text([[This table regroups values that describe details of the configuration of darktable.]])
darktable.configuration.version:set_text([[The version number of darktable.]])
darktable.configuration.has_gui:set_text([[True if darktable has a GUI (launched through the main darktable command, not darktable-cli).]])
darktable.configuration.verbose:set_text([[True if the Lua logdomain is enabled.]])
darktable.configuration.tmp_dir:set_text([[The name of the directory where darktable will store temporary files.]])
darktable.configuration.config_dir:set_text([[The name of the directory where darktable will find its global configuration objects (modules).]])
darktable.configuration.cache_dir:set_text([[The name of the directory where darktable will store its mipmaps.]])
darktable.configuration.api_version_major:set_text([[The major version number of the lua API.]])
darktable.configuration.api_version_minor:set_text([[The minor version number of the lua API.]])
darktable.configuration.api_version_patch:set_text([[The patch version number of the lua API.]])
darktable.configuration.api_version_suffix:set_text([[The version suffix of the lua API.]])
darktable.configuration.api_version_string:set_text([[The version description of the lua API. This is a string compatible with the semantic versioning convention]])
darktable.configuration.running_os:set_text([[The name of the Operating system darktable is currently running on]])
darktable.configuration.check_version:set_text([[Check that a module is compatible with the running version of darktable]]..para().."Add the following line at the top of your module : "..
code("darktable.configuration.check(...,{M,m,p},{M2,m2,p2})").."To document that your module has been tested with API version M.m.p and M2.m2.p2."..para()..
"This will raise an error if the user is running a released version of DT and a warning if he is running a development version"..para().."(the ... here will automatically expand to your module name if used at the top of your script")
darktable.configuration.check_version:add_parameter("module_name","string","The name of the module to report on error")
darktable.configuration.check_version:add_parameter("...","table...","Tables of API versions that are known to work with the script")


-----------------------------
--  DARKTABLE.PREFERENCES  --
-----------------------------
darktable.preferences:set_text([[Lua allows you to manipulate preferences. Lua has its own namespace for preferences and you can't access nor write normal darktable preferences.]]..para()..
[[Preference handling functions take a _script_ parameter. This is a string used to avoid name collision in preferences (i.e namespace). Set it to something unique, usually the name of the script handling the preference.]]..para()..
[[Preference handling functions can't guess the type of a parameter. You must pass the type of the preference you are handling. ]]..para()..
[[Note that the directory, enum, lua and file type preferences are stored internally as string. The user can only select valid values, but a lua script can set it to any string]])


darktable.preferences.register:set_text([[Creates a new preference entry in the Lua tab of the preference screen. If this function is not called the preference can't be set by the user (you can still read and write invisible preferences).]])
darktable.preferences.register:add_parameter("script","string",[[Invisible prefix to guarantee unicity of preferences.]])
darktable.preferences.register:add_parameter("name","string",[[A unique name used with the script part to identify the preference.]])
darktable.preferences.register:add_parameter("type",types.lua_pref_type,[[The type of the preference - one of the string values described above.]])
darktable.preferences.register:add_parameter("label","string",[[The label displayed in the preference screen.]])
darktable.preferences.register:add_parameter("tooltip","string",[[The tooltip to display in the preference menu.]])
darktable.preferences.register:add_parameter("default","depends on type",[[Default value to use when not set explicitly or by the user.]]..para().."For the enum type of pref, this is mandatory"):set_attribute("optional",true)
darktable.preferences.register:add_parameter("min","int or float",[[Minimum value (integer and float preferences only).]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("max","int or float",[[Maximum value (integer and float preferences only).]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("step","float",[[Step of the spinner (float preferences only).]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("values","string...",[[Other allowed values (enum preferences only)]]):set_attribute("optional",true)
darktable.preferences.register:add_parameter("wiget",types.lua_widget,[[The widget to use in preference(lua preferences only)]]):set_attribute("optional",true)
tmp = darktable.preferences.register:add_parameter("set_callback","function",[[A function called when the widget needs to be updated from the preference]])
tmp:set_attribute("optional",true)
tmp:add_parameter("widget",types.lua_widget,"The widget to update")

darktable.preferences.read:set_text([[Reads a value from a Lua preference.]])
darktable.preferences.read:add_parameter("script","string",[[Invisible prefix to guarantee unicity of preferences.]])
darktable.preferences.read:add_parameter("name","string",[[The name of the preference displayed in the preference screen.]])
darktable.preferences.read:add_parameter("type",types.lua_pref_type,[[The type of the preference.]])
darktable.preferences.read:add_return("depends on type",[[The value of the preference.]])

darktable.preferences.write:set_text([[Writes a value to a Lua preference.]])
darktable.preferences.write:add_parameter("script","string",[[Invisible prefix to guarantee unicity of preferences.]])
darktable.preferences.write:add_parameter("name","string",[[The name of the preference displayed in the preference screen.]])
darktable.preferences.write:add_parameter("type",types.lua_pref_type,[[The type of the preference.]])
darktable.preferences.write:add_parameter("value","depends on type",[[The value to set the preference to.]])


-----------------------
--  DARKTABLE.STYLES --
-----------------------

darktable.styles:set_text([[This pseudo table allows you to access and manipulate styles.]])

darktable.styles["#"]:set_text([[Each existing style has a numeric index; you can iterate them using ipairs.]])

darktable.styles.create:set_text([[Create a new style based on an image.]])
darktable.styles.create:add_parameter("image",types.dt_lua_image_t,[[The image to create the style from.]])
darktable.styles.create:add_parameter("name","string",[[The name to give to the new style.]])
darktable.styles.create:add_parameter("description","string",[[The description of the new style.]]):set_attribute("optional")
darktable.styles.create:add_return(types.dt_style_t,[[The new style object.]])
darktable.styles.create:set_main_parent(darktable.styles)

darktable.styles.delete:set_text([[Deletes an existing style.]])
darktable.styles.delete:add_parameter("style",types.dt_style_t,[[the style to delete]])
darktable.styles.delete:set_main_parent(darktable.styles)

darktable.styles.duplicate:set_text([[Create a new style based on an existing style.]])
darktable.styles.duplicate:add_parameter("style",types.dt_style_t,[[The style to base the new style on.]])
darktable.styles.duplicate:add_parameter("name","string",[[The new style's name.]])
darktable.styles.duplicate:add_parameter("description","string",[[The new style's description.]]):set_attribute("optional")
darktable.styles.duplicate:add_return(types.dt_style_t,[[The new style object.]])
darktable.styles.duplicate:set_main_parent(darktable.styles)

darktable.styles.apply:set_text([[Apply a style to an image. The order of parameters can be inverted.]])
darktable.styles.apply:add_parameter("style",types.dt_style_t,[[The style to use.]])
darktable.styles.apply:add_parameter("image",types.dt_lua_image_t,[[The image to apply the style to.]])
darktable.styles.apply:set_main_parent(darktable.styles)

darktable.styles.import:set_text([[Import a style from an external .dtstyle file]])
darktable.styles.import:add_parameter("filename","string","The file to import");
darktable.styles.import:set_main_parent(darktable.styles)

darktable.styles.export:set_text([[Export a style to an external .dtstyle file]])
darktable.styles.export:add_parameter("style",types.dt_style_t,"The style to export");
darktable.styles.export:add_parameter("directory","string","The directory to export to");
darktable.styles.export:add_parameter("overwrite","boolean","Is overwriting an existing file allowed"):set_attribute("optional")
darktable.styles.export:set_main_parent(darktable.styles)
-------------------------
--  DARKTABLE.DATABASE --
-------------------------

darktable.database:set_text([[Allows to access the database of images. Note that duplicate images (images with the same RAW but different XMP) will appear multiple times with different duplicate indexes. Also note that all images are here. This table is not influenced by any GUI filtering (collections, stars etc...).]])


darktable.database["#"]:set_text([[Each image in the database appears with a numerical index; you can iterate them using ipairs.]])
darktable.database.duplicate:set_text([[Creates a duplicate of an image and returns it.]])
darktable.database.duplicate:add_parameter("image",types.dt_lua_image_t,[[the image to duplicate]])
darktable.database.duplicate:add_return(types.dt_lua_image_t,[[The new image object.]])
darktable.database.duplicate:set_main_parent(darktable.database)

darktable.database.import:set_text([[Imports new images into the database.]])
darktable.database.import:add_parameter("location","string",[[The filename or directory to import images from.

NOTE: If the images are set to be imported recursively in preferences only the toplevel film is returned (the one whose path was given as a parameter).

NOTE2: If the parameter is a directory the call is non-blocking; the film object will not have the newly imported images yet. Use a post-import-film filtering on that film to react when images are actually imported.


]])
darktable.database.import:add_return(types.dt_lua_image_t,[[The created image if an image is imported or the toplevel film object if a film was imported.]])
darktable.database.move_image:set_text([[Physically moves an image (and all its duplicates) to another film.]]..para()..
[[This will move the image file, the related XMP and all XMP for the duplicates to the directory of the new film]]..para()..
[[Note that the parameter order is not relevant.]])
darktable.database.move_image:add_parameter("image",types.dt_lua_image_t,[[The image to move]])
darktable.database.move_image:add_parameter("film",types.dt_lua_film_t,[[The film to move to]])
darktable.database.move_image:set_main_parent(darktable.database)
darktable.database.copy_image:set_text([[Physically copies an image to another film.]]..para()..
[[This will copy the image file and the related XMP to the directory of the new film]]..para()..
[[If there is already a file with the same name as the image file, it will create a duplicate from that file instead]]..para()..
[[Note that the parameter order is not relevant.]])
darktable.database.copy_image:add_parameter("image",types.dt_lua_image_t,[[The image to copy]])
darktable.database.copy_image:add_parameter("film",types.dt_lua_film_t,[[The film to copy to]])
darktable.database.copy_image:add_return(types.dt_lua_image_t,[[The new image]])
darktable.database.copy_image:set_main_parent(darktable.database)
darktable.collection:set_text([[Allows to access the currently worked on images, i.e the ones selected by the collection lib. Filtering (rating etc) does not change that collection.]])


darktable.collection["#"]:set_text([[Each image in the collection appears with a numerical index; you can iterate them using ipairs.]])


for k, v in darktable.gui.views:unskipped_children() do
	v:set_main_parent(darktable.gui.views)
end
darktable.gui.views:set_text([[The different views in darktable]])
darktable.gui.views.map:set_text([[The map view]])
darktable.gui.views.map.latitude:set_text([[The latitude of the center of the map]])
darktable.gui.views.map.longitude:set_text([[The longitude of the center of the map]])
darktable.gui.views.map.zoom:set_text([[The current zoom level of the map]])

darktable.gui.views.darkroom:set_text([[The darkroom view]])
darktable.gui.views.lighttable:set_text([[The lighttable view]])
darktable.gui.views.tethering:set_text([[The tethering view]])
darktable.gui.views.slideshow:set_text([[The slideshow view]])
darktable.gui.views.print:set_text([[The print view]])
darktable.gui.views.knight:set_skipped()

--[[
for k, v in darktable.gui.libs:unskipped_children() do
	local real_node = real_darktable.gui.libs[k]
	v:set_attribute("position",real_node.position);
	v:set_attribute("container",real_node.container);
	local matching_views={}
	for k2,v2 in pairs(real_node.views) do
		table.insert(matching_views,darktable.gui.views[v2.id])
	end
	v:set_attribute("views",matching_views);
end
]]
darktable.gui.libs:set_text([[This table allows to reference all lib objects]]..para()..
[[lib are the graphical blocks within each view.]]..para()..
[[To quickly figure out what lib is what, you can use the following code which will make a given lib blink.]]..para()..
code([[local tested_module="global_toolbox"
dt.gui.libs[tested_module].visible=false
coroutine.yield("WAIT_MS",2000)
while true do
	dt.gui.libs[tested_module].visible = not dt.gui.libs[tested_module].visible
	coroutine.yield("WAIT_MS",2000)
end]]))


darktable.gui.libs.snapshots:set_text([[The UI element that manipulates snapshots in darkroom]])
darktable.gui.libs.snapshots.ratio:set_text([[The place in the screen where the line separating the snapshot is. Between 0 and 1]])
darktable.gui.libs.snapshots.direction:set_text([[The direction of the snapshot overlay]]):set_reported_type(types.snapshot_direction_t)

darktable.gui.libs.snapshots["#"]:set_text([[The different snapshots for the image]])
darktable.gui.libs.snapshots.selected:set_text([[The currently selected snapshot]])
darktable.gui.libs.snapshots.selected:set_reported_type(types.dt_lua_snapshot_t)
darktable.gui.libs.snapshots.take_snapshot:set_text([[Take a snapshot of the current image and add it to the UI]]..para()..[[The snapshot file will be generated at the next redraw of the main window]])
darktable.gui.libs.snapshots.max_snapshot:set_text([[The maximum number of snapshots]])

darktable.gui.libs.collect:set_text([[The collection UI element that allows to filter images by collection]])
darktable.gui.libs.collect.filter:set_text([[Allows to get or change the list of visible images]])
darktable.gui.libs.collect.filter:add_parameter("rules","array of"..my_tostring(types.dt_lib_collect_params_rule_t),[[A table of rules describing the filter. These rules will be applied after this call]]):set_attribute("optional",true)
darktable.gui.libs.collect.filter:add_return("array of"..my_tostring(types.dt_lib_collect_params_rule_t),[[The rules that were applied before this call.]])
darktable.gui.libs.collect.filter:set_attribute("implicit_yield",true)
darktable.gui.libs.collect.new_rule:set_text([[Returns a newly created rule object]])
darktable.gui.libs.collect.new_rule:add_return(my_tostring(types.dt_lib_collect_params_rule_t),[[The newly created rule]])

darktable.gui.libs.import:set_text([[The buttons to start importing images]])
darktable.gui.libs.import.register_widget:set_text([[Add a widget in the option expander of the import dialog]])
darktable.gui.libs.import.register_widget:add_parameter("widget",types.lua_widget,[[The widget to add to the dialog. The reset callback of the widget will be called whenever the dialog is opened]])



darktable.gui.libs.styles:set_text([[The style selection menu]])
darktable.gui.libs.metadata_view:set_text([[The widget displaying metadata about the current image]])
darktable.gui.libs.metadata_view.register_info:set_text([[Register a function providing extra info to display in the widget]])
darktable.gui.libs.metadata_view.register_info:add_parameter("name","string","The name displayed for the new information")
tmp = darktable.gui.libs.metadata_view.register_info:add_parameter("callback","function","The function providing the info")
tmp:add_parameter("image",types.dt_lua_image_t,"The image to analyze")
tmp:add_return("string","The extra information to display")
darktable.gui.libs.metadata:set_text([[The widget allowing modification of metadata fields on the current image]])
darktable.gui.libs.hinter:set_text([[The small line of text at the top of the UI showing the number of selected images]])
darktable.gui.libs.filmstrip:set_text([[The filmstrip at the bottom of some views]])
darktable.gui.libs.viewswitcher:set_text([[The labels allowing to switch view]])
darktable.gui.libs.darktable_label:set_text([[The darktable logo in the upper left corner]])
darktable.gui.libs.tagging:set_text([[The tag manipulation UI]])
darktable.gui.libs.geotagging:set_text([[The geotagging time synchronisation UI]])
darktable.gui.libs.recentcollect:set_text([[The recent collection UI element]])
darktable.gui.libs.global_toolbox:set_text([[The common tools to all view (settings, grouping...)]])
darktable.gui.libs.global_toolbox.grouping:set_text([[The current status of the image grouping option]])
darktable.gui.libs.global_toolbox.show_overlays:set_text([[the current status of the image overlays option]])
darktable.gui.libs.filter:set_text([[The image-filter menus at the top of the UI]])
darktable.gui.libs.ratings:set_text([[The starts to set the rating of an image]])
darktable.gui.libs.select:set_text([[The buttons that allow to quickly change the selection]])
darktable.gui.libs.select.register_selection:set_text([[Add a new button and call a callback when it is clicked]])
darktable.gui.libs.select.register_selection:add_parameter("label","string","The label to display on the button")
tmp = darktable.gui.libs.select.register_selection:add_parameter("callback","function","The function to call when the button is pressed")
tmp:add_parameter("event","string","The name of the button that was pressed")
tmp:add_parameter("images","table of"..tostring(types.dt_lua_image_t),"The images in the current collection. This is the same content as"..my_tostring(darktable.collection))
tmp:add_return("table of"..tostring(types.dt_lua_image_t),"The images to set the selection to")
darktable.gui.libs.select.register_selection:add_parameter("tooltip","string","The tooltip to use on the new button"):set_attribute("optional",true)
darktable.gui.libs.colorlabels:set_text([[The color buttons that allow to set labels on an image]])
darktable.gui.libs.lighttable_mode:set_text([[The navigation and zoom level UI in lighttable]])
darktable.gui.libs.copy_history:set_text([[The UI element that manipulates history]])
darktable.gui.libs.image:set_text([[The UI element that manipulates the current images]])
darktable.gui.libs.image.register_action:set_text([[Add a new button and call a callback when it is clicked]])
darktable.gui.libs.image.register_action:add_parameter("label","string","The label to display on the button")
tmp = darktable.gui.libs.image.register_action:add_parameter("callback","function","The function to call when the button is pressed")
tmp:add_parameter("event","string","The name of the button that was pressed")
tmp:add_parameter("images","table of"..tostring(types.dt_lua_image_t),"The images to act on when the button was clicked")
darktable.gui.libs.image.register_action:add_parameter("tooltip","string","The tooltip to use on the new button"):set_attribute("optional",true)
darktable.gui.libs.modulegroups:set_text([[The icons describing the different iop groups]])
darktable.gui.libs.module_toolbox:set_text([[The tools on the bottom line of the UI (overexposure)]])
darktable.gui.libs.session:set_text([[The session UI when tethering]])
darktable.gui.libs.histogram:set_text([[The histogram widget]])
darktable.gui.libs.export:set_text([[The export menu]])
darktable.gui.libs.history:set_text([[The history manipulation menu]])
darktable.gui.libs.colorpicker:set_text([[The colorpicker menu]])
darktable.gui.libs.navigation:set_text([[The full image preview to allow navigation]])
darktable.gui.libs.masks:set_text([[The masks window]])
darktable.gui.libs.view_toolbox:set_text([[]])
darktable.gui.libs.live_view:set_text([[The liveview window]])
darktable.gui.libs.map_settings:set_text([[The map setting window]])
darktable.gui.libs.camera:set_text([[The camera selection UI]])
darktable.gui.libs.location:set_text([[The location ui]])
darktable.gui.libs.backgroundjobs:set_text([[The window displaying the currently running jobs]])
darktable.gui.libs.print_settings:set_text([[The settings window in the print view]])


darktable.control:set_text([[This table contain function to manipulate the control flow of lua programs. It provides ways to do background jobs and other related functions]])
darktable.control.ending:set_text([[TRUE when darktable is terminating]]..para()..
[[Use this variable to detect when you should finish long running jobs]])
darktable.control.dispatch:set_text([[Runs a function in the background. This function will be run at a later point, after luarc has finished running. If you do a loop in such a function, please check ]]..my_tostring(darktable.control.ending)..[[ in your loop to finish the function when DT exits]])
darktable.control.dispatch:add_parameter("function","function",[[The call to dispatch]])
darktable.control.dispatch:add_parameter("...","anything",[[extra parameters to pass to the function]])
darktable.control.sleep:set_text("Suspends execution while not blocking darktable")
darktable.control.sleep:add_parameter("delay","int","The delay in millisecond to sleep")
darktable.control.execute:set_text("Run a command in a shell while not blocking darktable")
darktable.control.execute:add_parameter("command","string","The command to run, as in 'sh -c'")
darktable.control.execute:add_return("int","The result of the system call")
darktable.control.read:set_text("Block until a file is readable while not blocking darktable"..para()..emphasis("This function is not available on Windows builds"))
darktable.control.read:add_parameter("file","file","The file object to wait for")


darktable.gettext:set_text([[This table contains functions related to translating lua scripts]])
darktable.gettext.gettext:set_text([[Translate a string using the darktable textdomain]])
darktable.gettext.gettext:add_parameter("msgid","string","The string to translate");
darktable.gettext.gettext:add_return("string","The translated string");
darktable.gettext.dgettext:set_text([[Translate a string using the specified textdomain]])
darktable.gettext.dgettext:add_parameter("domainname","string","The domain to use for that translation");
darktable.gettext.dgettext:add_parameter("msgid","string","The string to translate");
darktable.gettext.dgettext:add_return("string","The translated string");
darktable.gettext.ngettext:set_text([[Translate a string depending on the number of objects using the darktable textdomain]])
darktable.gettext.ngettext:add_parameter("msgid","string","The string to translate");
darktable.gettext.ngettext:add_parameter("msgid_plural","string","The string to translate in plural form");
darktable.gettext.ngettext:add_parameter("n","int","The number of objects");
darktable.gettext.ngettext:add_return("string","The translated string");
darktable.gettext.dngettext:set_text([[Translate a string depending on the number of objects using the specified textdomain]])
darktable.gettext.dngettext:add_parameter("domainname","string","The domain to use for that translation");
darktable.gettext.dngettext:add_parameter("msgid","string","The string to translate");
darktable.gettext.dngettext:add_parameter("msgid_plural","string","The string to translate in plural form");
darktable.gettext.dngettext:add_parameter("n","int","The number of objects");
darktable.gettext.dngettext:add_return("string","The translated string");
darktable.gettext.bindtextdomain:set_text([[Tell gettext where to find the .mo file translating messages for a particular domain]])
darktable.gettext.bindtextdomain:add_parameter("domainname","string","The domain to use for that translation");
darktable.gettext.bindtextdomain:add_parameter("dirname","string","The base directory to look for the file. The file should be placed in "..emphasis("dirname").."/"..emphasis("locale name").."/LC_MESSAGES/"..emphasis("domain")..".mo");

----------------------
--  DARKTABLE.DEBUG --
----------------------
darktable.debug:set_text([[This section must be activated separately by calling

require "darktable.debug"
]])

darktable.debug.dump:set_text([[This will return a string describing everything Lua knows about an object, used to know what an object is.

This function is recursion-safe and can be used to dump _G if needed.]])
darktable.debug.dump:add_parameter("object","anything",[[The object to dump.]])
darktable.debug.dump:add_parameter("name","string",[[A name to use for the object.]]):set_attribute("optional",true)
tmp_node = darktable.debug.dump:add_parameter("known","table",[[A table of object,string pairs. Any object in that table will not be dumped, the string will be printed instead.]]..para().."defaults to "..my_tostring(darktable.debug.known).." if not set")
tmp_node:set_attribute("optional",true)
darktable.debug.dump:add_return("string",[[A string containing a text description of the object - can be very long.]])

darktable.debug.debug:set_text([[Initialized to false; set it to true to also dump information about metatables.]])
darktable.debug.max_depth:set_text([[Initialized to 10; The maximum depth to recursively dump content.]])

remove_all_children(darktable.debug.known) -- debug values, not interesting
darktable.debug.known:set_text([[A table containing the default value of ]]..my_tostring(tmp_node))
darktable.debug.type:set_text([[Similar to the system function type() but it will return the real type instead of "userdata" for darktable specific objects.]])
  darktable.debug.type:add_parameter("object","anything",[[The object whose type must be reported.]])
	darktable.debug.type:add_return("string",[[A string describing the type of the object.]])

	----------------------
	--  TYPES           --
	----------------------
	types:set_text([[This section documents types that are specific to darktable's Lua API.]])

	types.lua_os_type:set_text([[The type of OS we darktable can run on]])

	types.dt_lua_image_t:set_text([[Image objects represent an image in the database. This is slightly different from a file on disk since a file can have multiple developments.

	Note that this is the real image object; changing the value of a field will immediately change it in darktable and will be reflected on any copy of that image object you may have kept.]])


	types.dt_lua_image_t.id:set_text([[A unique id identifying the image in the database.]])
	types.dt_lua_image_t.path:set_text([[The file the directory containing the image.]])
	types.dt_lua_image_t.film:set_text([[The film object that contains this image.]])
	types.dt_lua_image_t.filename:set_text([[The filename of the image.]])
  types.dt_lua_image_t.sidecar:set_text([[The filename of the image's sidecar file.]])
	types.dt_lua_image_t.duplicate_index:set_text([[If there are multiple images based on a same file, each will have a unique number, starting from 0.]])


	types.dt_lua_image_t.publisher:set_text([[The publisher field of the image.]])
	types.dt_lua_image_t.title:set_text([[The title field of the image.]])
	types.dt_lua_image_t.creator:set_text([[The creator field of the image.]])
	types.dt_lua_image_t.rights:set_text([[The rights field of the image.]])
	types.dt_lua_image_t.description:set_text([[The description field for the image.]])

	types.dt_lua_image_t.exif_maker:set_text([[The maker exif data.]])
	types.dt_lua_image_t.exif_model:set_text([[The camera model used.]])
	types.dt_lua_image_t.exif_lens:set_text([[The id string of the lens used.]])
	types.dt_lua_image_t.exif_aperture:set_text([[The aperture saved in the exif data.]])
	types.dt_lua_image_t.exif_exposure:set_text([[The exposure time of the image.]])
	types.dt_lua_image_t.exif_focal_length:set_text([[The focal length of the image.]])
	types.dt_lua_image_t.exif_iso:set_text([[The iso used on the image.]])
	types.dt_lua_image_t.exif_datetime_taken:set_text([[The date and time of the image.]])
	types.dt_lua_image_t.exif_focus_distance:set_text([[The distance of the subject.]])
	types.dt_lua_image_t.exif_crop:set_text([[The exif crop data.]])
	types.dt_lua_image_t.latitude:set_text([[GPS latitude data of the image, nil if not set.]])
	types.dt_lua_image_t.latitude:set_reported_type("float or nil")
	types.dt_lua_image_t.longitude:set_text([[GPS longitude data of the image, nil if not set.]])
	types.dt_lua_image_t.longitude:set_reported_type("float or nil")
	types.dt_lua_image_t.elevation:set_text([[GPS altitude data of the image, nil if not set.]])
	types.dt_lua_image_t.elevation:set_reported_type("float or nil")
	types.dt_lua_image_t.is_raw:set_text([[True if the image is a RAW file.]])
	types.dt_lua_image_t.is_ldr:set_text([[True if the image is a ldr image.]])
	types.dt_lua_image_t.is_hdr:set_text([[True if the image is a hdr image.]])
  types.dt_lua_image_t.has_txt:set_text([[True if the image has a txt sidecar file.]])
  types.dt_lua_image_t.width:set_text([[The width of the image.]])
	types.dt_lua_image_t.height:set_text([[The height of the image.]])
  types.dt_lua_image_t.rating:set_text([[The rating of the image (-1 for rejected).]])
  types.dt_lua_image_t.red:set_text([[True if the image has the corresponding colorlabel.]])
	types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.blue)
	types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.green)
	types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.yellow)
	types.dt_lua_image_t.red:set_alias(types.dt_lua_image_t.purple)
	types.dt_lua_image_t.reset:set_text([[Removes all processing from the image, resetting it back to its original state]])
	types.dt_lua_image_t.reset:add_parameter("self",types.dt_lua_image_t,[[The image whose history will be deleted]]):set_attribute("is_self",true)
	types.dt_lua_image_t.delete:set_text([[Removes an image from the database]])
	types.dt_lua_image_t.delete:add_parameter("self",types.dt_lua_image_t,[[The image to remove]]):set_attribute("is_self",true)

	types.dt_lua_image_t.group_with:set_text([[Puts the first image in the same group as the second image. If no second image is provided the image will be in its own group.]])
	types.dt_lua_image_t.group_with:add_parameter("self",types.dt_lua_image_t,[[The image whose group must be changed.]]):set_attribute("is_self",true)
	types.dt_lua_image_t.group_with:add_parameter("image",types.dt_lua_image_t,[[The image we want to group with.]]):set_attribute("optional",true)
	types.dt_lua_image_t.make_group_leader:set_text([[Makes the image the leader of its group.]])
	types.dt_lua_image_t.make_group_leader:add_parameter("self",types.dt_lua_image_t,[[The image we want as the leader.]]):set_attribute("is_self",true)
	types.dt_lua_image_t.get_group_members:set_text([[Returns a table containing all ]]..my_tostring(types.dt_lua_image_t)..[[ of the group. The group leader is both at a numeric key and at the "leader" special key (so you probably want to use ipairs to iterate through that table).]])
	types.dt_lua_image_t.get_group_members:add_parameter("self",types.dt_lua_image_t,[[The image whose group we are querying.]]):set_attribute("is_self",true)
	types.dt_lua_image_t.get_group_members:add_return("table of "..my_tostring(types.dt_lua_image_t),[[A table of image objects containing all images that are in the same group as the image.]])
	darktable.tags.attach:set_alias(types.dt_lua_image_t.attach_tag)
	types.dt_lua_image_t.group_leader:set_text([[The image which is the leader of the group this image is a member of.]])
	types.dt_lua_image_t.local_copy:set_text([[True if the image has a copy in the local cache]])
	types.dt_lua_image_t.drop_cache:set_text("drops the cached version of this image."..para()..
	"This function should be called if an image is modified out of darktable to force DT to regenerate the thumbnail"..para()..
	"darktable will regenerate the thumbnail by itself when it is needed")
	types.dt_lua_image_t.drop_cache:add_parameter("self",types.dt_lua_image_t,[[The image whose cache must be dropped.]]):set_attribute("is_self",true)

	types.dt_imageio_module_format_t:set_text([[A virtual type representing all format types.]])
	types.dt_imageio_module_format_t.plugin_name:set_text([[A unique name for the plugin.]])
	types.dt_imageio_module_format_t.name:set_text([[A human readable name for the plugin.]])
	types.dt_imageio_module_format_t.extension:set_text([[The typical filename extension for that format.]])
	types.dt_imageio_module_format_t.mime:set_text([[The mime type associated with the format.]])
	types.dt_imageio_module_format_t.max_width:set_text([[The max width allowed for the format (0 = unlimited).]])
	types.dt_imageio_module_format_t.max_height:set_text([[The max height allowed for the format (0 = unlimited).]])
	types.dt_imageio_module_format_t.write_image:set_text([[Exports an image to a file. This is a blocking operation that will not return until the image is exported.]])
	types.dt_imageio_module_format_t.write_image:set_attribute("implicit_yield",true)
	types.dt_imageio_module_format_t.write_image:add_parameter("self",types.dt_imageio_module_format_t,[[The format that will be used to export.]]):set_attribute("is_self",true)
	types.dt_imageio_module_format_t.write_image:add_parameter("image",types.dt_lua_image_t,[[The image object to export.]])
	types.dt_imageio_module_format_t.write_image:add_parameter("filename","string",[[The filename to export to.]])
	types.dt_imageio_module_format_t.write_image:add_parameter("allow_upscale","boolean",[[Set to true to allow upscaling of the image.]]):set_attribute("optional",true)
	types.dt_imageio_module_format_t.write_image:add_return("boolean",[[Returns true on success.]])

	types.dt_imageio_module_format_data_png:set_text([[Type object describing parameters to export to png.]])
	types.dt_imageio_module_format_data_png.bpp:set_text([[The bpp parameter to use when exporting.]])
	types.dt_imageio_module_format_data_tiff:set_text([[Type object describing parameters to export to tiff.]])
	types.dt_imageio_module_format_data_tiff.bpp:set_text([[The bpp parameter to use when exporting.]])
	types.dt_imageio_module_format_data_exr:set_text([[Type object describing parameters to export to exr.]])
	types.dt_imageio_module_format_data_exr.compression:set_text([[The compression parameter to use when exporting.]])
	types.dt_imageio_module_format_data_exr.pixel_type:set_text([[The pixel type parameter to use when exporting.]])
	types.dt_imageio_module_format_data_copy:set_text([[Type object describing parameters to export to copy.]])
	types.dt_imageio_module_format_data_pfm:set_text([[Type object describing parameters to export to pfm.]])
	types.dt_imageio_module_format_data_jpeg:set_text([[Type object describing parameters to export to jpeg.]])
	types.dt_imageio_module_format_data_jpeg.quality:set_text([[The quality to use at export time.]])
	types.dt_imageio_module_format_data_ppm:set_text([[Type object describing parameters to export to ppm.]])
	types.dt_imageio_module_format_data_webp:set_text([[Type object describing parameters to export to webp.]])
	types.dt_imageio_module_format_data_webp.quality:set_text([[The quality to use at export time.]])
	types.dt_imageio_module_format_data_webp.comp_type:set_text([[The overall quality to use; can be one of "webp_lossy" or "webp_lossless".]]):set_reported_type(types.comp_type_t);
	types.dt_imageio_module_format_data_webp.hint:set_text([[A hint on the overall content of the image.]]):set_reported_type(types.hint_t)
	types.dt_imageio_module_format_data_j2k:set_text([[Type object describing parameters to export to jpeg2000.]])
	types.dt_imageio_module_format_data_j2k.quality:set_text([[The quality to use at export time.]])
	types.dt_imageio_module_format_data_j2k.bpp:set_text([[The bpp parameter to use when exporting.]])
	types.dt_imageio_module_format_data_j2k.format:set_text([[The format to use.]]):set_reported_type(types.dt_imageio_j2k_format_t)
	types.dt_imageio_module_format_data_j2k.preset:set_text([[The preset to use.]]):set_reported_type(types.dt_imageio_j2k_preset_t)


	types.dt_imageio_module_format_data_pdf:set_text([[Type object describing parameters to export to pdf.]])
  types.dt_imageio_module_format_data_pdf.dpi:set_text([[The dot per inch value to use at export]])
  types.dt_imageio_module_format_data_pdf.icc:set_text([[Should the images be tagged with their embedded profile]])
  types.dt_imageio_module_format_data_pdf.border:set_text([[Empty space around the PDF images]])
  types.dt_imageio_module_format_data_pdf.orientation:set_text([[Orientation of the pages in the document]])
  types.dt_imageio_module_format_data_pdf.title:set_text([[The title for the document
  types.dt_imageio_module_format_data_pdf.rotate:set_text([[Should the images be rotated to match the PDF orientation]])
  types.dt_imageio_module_format_data_pdf.mode:set_text([[The image mode to use at export time]])
  types.dt_imageio_module_format_data_pdf.size:set_text([[The paper size to use]])
  types.dt_imageio_module_format_data_pdf.compression:set_text([[Compression mode to use for images]])
  types.dt_imageio_module_format_data_pdf.pages:set_text([[The page type to use]])
  types.dt_imageio_module_format_data_pdf.rotate:set_text([[Should the images be rotated in the resulting PDF]])
  types._pdf_mode_t:set_text([[The export mode to use for PDF document]])
  types._pdf_pages_t:set_text([[The different page types for PDF export]])
  types.dt_pdf_stream_encoder_t:set_text([[The compression mode for PDF document]])


	types.dt_imageio_module_storage_t:set_text([[A virtual type representing all storage types.]])
	types.dt_imageio_module_storage_t.plugin_name:set_text([[A unique name for the plugin.]])
	types.dt_imageio_module_storage_t.name:set_text([[A human readable name for the plugin.]])
	types.dt_imageio_module_storage_t.width:set_text([[The currently selected width for the plugin.]])
	types.dt_imageio_module_storage_t.height:set_text([[The currently selected height for the plugin.]])
	types.dt_imageio_module_storage_t.recommended_width:set_text([[The recommended width for the plugin.]])
	types.dt_imageio_module_storage_t.recommended_height:set_text([[The recommended height for the plugin.]])
	types.dt_imageio_module_storage_t.supports_format:set_text([[Checks if a format is supported by this storage.]])
	types.dt_imageio_module_storage_t.supports_format:add_parameter("self",types.dt_imageio_module_storage_t,[[The storage type to check against.]]):set_attribute("is_self",true)
	types.dt_imageio_module_storage_t.supports_format:add_parameter("format",types.dt_imageio_module_format_t,[[The format type to check.]])
	types.dt_imageio_module_storage_t.supports_format:add_return("boolean",[[True if the format is supported by the storage.]])

	types.dt_imageio_module_storage_data_email:set_text([[An object containing parameters to export to email.]])
	types.dt_imageio_module_storage_data_flickr:set_text([[An object containing parameters to export to flickr.]])
	types.dt_imageio_module_storage_data_facebook:set_text([[An object containing parameters to export to facebook.]])
	types.dt_imageio_module_storage_data_latex:set_text([[An object containing parameters to export to latex.]])
	types.dt_imageio_module_storage_data_latex.filename:set_text([[The filename to export to.]])
	types.dt_imageio_module_storage_data_latex.title:set_text([[The title to use for export.]])
	types.dt_imageio_module_storage_data_picasa:set_text([[An object containing parameters to export to picasa.]])
	types.dt_imageio_module_storage_data_gallery:set_text([[An object containing parameters to export to gallery.]])
	types.dt_imageio_module_storage_data_gallery.filename:set_text([[The filename to export to.]])
	types.dt_imageio_module_storage_data_gallery.title:set_text([[The title to use for export.]])
	types.dt_imageio_module_storage_data_disk:set_text([[An object containing parameters to export to disk.]])
	types.dt_imageio_module_storage_data_disk.filename:set_text([[The filename to export to.]])

	types.dt_lua_film_t:set_text([[A film in darktable; this represents a directory containing imported images.]])
	types.dt_lua_film_t["#"]:set_text([[The different images within the film.]])
	types.dt_lua_film_t.id:set_text([[A unique numeric id used by this film.]])
	types.dt_lua_film_t.path:set_text([[The path represented by this film.]])
	types.dt_lua_film_t.delete:set_text([[Removes the film from the database.]])
	types.dt_lua_film_t.delete:add_parameter("self",types.dt_lua_film_t,[[The film to remove.]]):set_attribute("is_self",true)
	types.dt_lua_film_t.delete:add_parameter("force","Boolean",[[Force removal, even if the film is not empty.]]):set_attribute("optional",true)

	types.dt_style_t:set_text([[A style that can be applied to an image.]])
	types.dt_style_t.name:set_text([[The name of the style.]])
	types.dt_style_t.description:set_text([[The description of the style.]])
	types.dt_style_t["#"]:set_text([[The different items that make the style.]])

	types.dt_style_item_t:set_text([[An element that is part of a style.]])
	types.dt_style_item_t.name:set_text([[The name of the style item.]])
	types.dt_style_item_t.num:set_text([[The position of the style item within its style.]])

	types.dt_lua_tag_t:set_text([[A tag that can be attached to an image.]])
	types.dt_lua_tag_t.name:set_text([[The name of the tag.]])
	types.dt_lua_tag_t["#"]:set_text([[The images that have that tag attached to them.]])
	types.dt_lua_tag_t["#"]:set_reported_type(types.dt_lua_image_t)

	types.dt_lua_lib_t:set_text([[The type of a UI lib]])
	types.dt_lua_lib_t.id:set_text([[A unit string identifying the lib]])
	types.dt_lua_lib_t.name:set_text([[The translated title of the UI element]])
	types.dt_lua_lib_t.version:set_text([[The version of the internal data of this lib]])
	types.dt_lua_lib_t.visible:set_text([[Allow to make a lib module completely invisible to the user.]]..para()..
	[[Note that if the module is invisible the user will have no way to restore it without lua]])
	types.dt_lua_lib_t.visible:set_attribute("implicit_yield",true)
	types.dt_lua_lib_t.container:set_text([[The location of the lib in the darktable UI]]):set_reported_type(types.dt_ui_container_t)
	types.dt_lua_lib_t.expandable:set_text([[True if the lib can be expanded/retracted]]);
	types.dt_lua_lib_t.expanded:set_text([[True if the lib is expanded]]);
	types.dt_lua_lib_t.position:set_text([[A value deciding the position of the lib within its container]])
	types.dt_lua_lib_t.views:set_text([[A table of all the views that display this widget]])
	types.dt_lua_lib_t.reset:set_text([[A function to reset the lib to its default values]]..para()..
	[[This function will do nothing if the lib is not visible or can't be reset]])
	types.dt_lua_lib_t.reset:add_parameter("self",types.dt_lua_lib_t,[[The lib to reset]]):set_attribute("is_self",true)
	types.dt_lua_lib_t.on_screen:set_text([[True if the lib is currently visible on the screen]])

	types.dt_lua_view_t:set_text([[A darktable view]])
	types.dt_lua_view_t.id:set_text([[A unique string identifying the view]])
	types.dt_lua_view_t.name:set_text([[The name of the view]])


	types.dt_lua_backgroundjob_t:set_text([[A lua-managed entry in the backgroundjob lib]])
	types.dt_lua_backgroundjob_t.percent:set_text([[The value of the progress bar, between 0 and 1. will return nil if there is no progress bar, will raise an error if read or written on an invalid job]])
	types.dt_lua_backgroundjob_t.valid:set_text([[True if the job is displayed, set it to false to destroy the entry]]..para().."An invalid job cannot be made valid again")


	types.dt_lua_snapshot_t:set_text([[The description of a snapshot in the snapshot lib]])
	types.dt_lua_snapshot_t.filename:set_text([[The filename of an image containing the snapshot]])
	types.dt_lua_snapshot_t.select:set_text([[Activates this snapshot on the display. To deactivate all snapshot you need to call this function on the active snapshot]])
	types.dt_lua_snapshot_t.select:add_parameter("self",types.dt_lua_snapshot_t,[[The snapshot to activate]]):set_attribute("is_self",true)
	types.dt_lua_snapshot_t.name:set_text([[The name of the snapshot, as seen in the UI]])

	types.hint_t:set_text([[a hint on the way to encode a webp image]])
	types.dt_ui_container_t:set_text([[A place in the darktable UI where a lib can be placed]])
	types.snapshot_direction_t:set_text([[Which part of the main window is occupied by a snapshot]])
	types.dt_imageio_j2k_format_t:set_text([[J2K format type]])
	types.dt_imageio_j2k_preset_t:set_text([[J2K preset type]])
	types.comp_type_t:set_text([[Type of compression for webp]])
	types.lua_pref_type:set_text([[The type of value to save in a preference]])


  types.dt_imageio_exr_compression_t:set_text("The type of compression to use for the EXR image")
  types.dt_imageio_exr_pixeltype_t:set_text("The pixel type to use for the EXR image")

  types.dt_lib_collect_params_rule_t:set_text("A single rule for filtering a collection");
  types.dt_lib_collect_params_rule_t.mode:set_text("How this rule is applied after the previous one. Unused for the first rule");
  types.dt_lib_collect_params_rule_t.mode:set_reported_type(types.dt_lib_collect_mode_t)
  types.dt_lib_collect_params_rule_t.data:set_text("The text segment of the rule. Exact content depends on the type of rule");
  types.dt_lib_collect_params_rule_t.item:set_text("The item on which this rule filter. i.e the type of the rule");
  types.dt_lib_collect_params_rule_t.item:set_reported_type(types.dt_collection_properties_t)
  types.dt_lib_collect_mode_t:set_text("The logical operators to apply between rules");
  types.dt_collection_properties_t:set_text("The different elements on which a collection can be filtered");

  types.dt_lua_orientation_t:set_text("A possible orientation for a widget")

  types.dt_lua_align_t:set_text("The alignment of a label")

  types.dt_lua_ellipsize_mode_t:set_text("The ellipsize mode of a label")

  types.dt_lua_cairo_t:set_text("A wrapper around a cairo drawing context."..para().."You probably shouldn't use this after the callback that got it passed returned."..para().."For more details of the member functions have a look at the cairo documentation for "..url("http://www.cairographics.org/manual/cairo-cairo-t.html", "the drawing context")..", "..url("http://www.cairographics.org/manual/cairo-Transformations.html", "transformations").." and "..url("http://www.cairographics.org/manual/cairo-Paths.html", "paths")..".")
  types.dt_lua_cairo_t.save:set_text("Save the state of the drawing context.")
  types.dt_lua_cairo_t.save:set_reported_type("function")
  types.dt_lua_cairo_t.save:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.restore:set_text("Restore a previously saved state.")
  types.dt_lua_cairo_t.restore:set_reported_type("function")
  types.dt_lua_cairo_t.restore:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.move_to:set_text("Begin a new sub-path.")
  types.dt_lua_cairo_t.move_to:set_reported_type("function")
  types.dt_lua_cairo_t.move_to:add_parameter("self", types.dt_lua_cairo_t, "The context to modify"):set_attribute("is_self", true)
  types.dt_lua_cairo_t.move_to:add_parameter("x", "float", "The x coordinate of the new position.")
  types.dt_lua_cairo_t.move_to:add_parameter("y", "float", "The y coordinate of the new position.")
  types.dt_lua_cairo_t.line_to:set_text("Add a line to the path.")
  types.dt_lua_cairo_t.line_to:set_reported_type("function")
  types.dt_lua_cairo_t.line_to:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.line_to:add_parameter("x", "float", "The x coordinate of the end of the new line.")
  types.dt_lua_cairo_t.line_to:add_parameter("y", "float", "The y coordinate of the end of the new line.")
  types.dt_lua_cairo_t.rectangle:set_text("Add a closed sub-path rectangle.")
  types.dt_lua_cairo_t.rectangle:set_reported_type("function")
  types.dt_lua_cairo_t.rectangle:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.rectangle:add_parameter("x", "float", "The x coordinate of the top left corner of the rectangle.")
  types.dt_lua_cairo_t.rectangle:add_parameter("y", "float", "The y coordinate of the top left corner of the rectangle.")
  types.dt_lua_cairo_t.rectangle:add_parameter("width", "float", "The width of the rectangle.")
  types.dt_lua_cairo_t.rectangle:add_parameter("height", "float", "The height of the rectangle.")
  types.dt_lua_cairo_t.arc:set_text("Add a circular arc.")
  types.dt_lua_cairo_t.arc:set_reported_type("function")
  types.dt_lua_cairo_t.arc:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.arc:add_parameter("x", "float", "The x position of the center of the arc.")
  types.dt_lua_cairo_t.arc:add_parameter("y", "float", "The y position of the center of the arc.")
  types.dt_lua_cairo_t.arc:add_parameter("radius", "float", "The radius of the arc.")
  types.dt_lua_cairo_t.arc:add_parameter("angle1", "float", "The start angle, in radians.")
  types.dt_lua_cairo_t.arc:add_parameter("angle2", "float", "The end angle, in radians.")
  types.dt_lua_cairo_t.arc_negative:set_text("Add a circular arc. It only differs in the direction from "..my_tostring(types.dt_lua_cairo_t.arc)..".")
  types.dt_lua_cairo_t.arc_negative:set_reported_type("function")
  types.dt_lua_cairo_t.arc_negative:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.arc_negative:add_parameter("x", "float", "The x position of the center of the arc.")
  types.dt_lua_cairo_t.arc_negative:add_parameter("y", "float", "The y position of the center of the arc.")
  types.dt_lua_cairo_t.arc_negative:add_parameter("radius", "float", "The radius of the arc.")
  types.dt_lua_cairo_t.arc_negative:add_parameter("angle1", "float", "The start angle, in radians.")
  types.dt_lua_cairo_t.arc_negative:add_parameter("angle2", "float", "The end angle, in radians.")
  types.dt_lua_cairo_t.rotate:set_text("Add a rotation to the transformation matrix.")
  types.dt_lua_cairo_t.rotate:set_reported_type("function")
  types.dt_lua_cairo_t.rotate:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.rotate:add_parameter("angle", "float", "The angle (in radians) by which the user-space axes will be rotated.")
  types.dt_lua_cairo_t.scale:set_text("Add a scaling to the transformation matrix.")
  types.dt_lua_cairo_t.scale:set_reported_type("function")
  types.dt_lua_cairo_t.scale:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.scale:add_parameter("x", "float", "The scale factor for the x dimension.")
  types.dt_lua_cairo_t.scale:add_parameter("y", "float", "The scale factor for the y dimension.")
  types.dt_lua_cairo_t.translate:set_text("Add a translation to the transformation matrix.")
  types.dt_lua_cairo_t.translate:set_reported_type("function")
  types.dt_lua_cairo_t.translate:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.translate:add_parameter("x", "float", "Amount to translate in the x direction")
  types.dt_lua_cairo_t.translate:add_parameter("y", "float", "Amount to translate in the y direction")
  types.dt_lua_cairo_t.new_sub_path:set_text("Begin a new sub-path.")
  types.dt_lua_cairo_t.new_sub_path:set_reported_type("function")
  types.dt_lua_cairo_t.new_sub_path:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.draw_line:set_text("Helper function to draw a line with a given start and end.")
  types.dt_lua_cairo_t.draw_line:set_reported_type("function")
  types.dt_lua_cairo_t.draw_line:add_parameter("self", types.dt_lua_cairo_t, "The context to modify."):set_attribute("is_self", true)
  types.dt_lua_cairo_t.draw_line:add_parameter("x_start", "float", "The x coordinate of the start of the new line.")
  types.dt_lua_cairo_t.draw_line:add_parameter("y_start", "float", "The y coordinate of the start of the new line.")
  types.dt_lua_cairo_t.draw_line:add_parameter("x_end", "float", "The x coordinate of the end of the new line.")
  types.dt_lua_cairo_t.draw_line:add_parameter("y_end", "float", "The y coordinate of the end of the new line.")


  types.lua_widget:set_text("Common parent type for all lua-handled widgets");
  types.lua_widget.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_widget.sensitive:set_text("Set if the widget is enabled/disabled");
  types.lua_widget.tooltip:set_text("Tooltip to display for the widget");
  types.lua_widget.tooltip:set_reported_type("string or nil")
  types.lua_widget.reset_callback:set_text("A function to call when the widget needs to reset itself"..para()..
  "Note that some widgets have a default implementation that can be overridden, (containers in particular will recursively reset their children). If you replace that default implementation you need to reimplement that functionality or call the original function within your callback")
  types.lua_widget.reset_callback:set_reported_type("function")
  types.lua_widget.reset_callback:add_parameter("widget",types.lua_widget,"The widget that triggered the callback")
  types.lua_widget.__call:set_main_parent(types.lua_widget)
  types.lua_widget.__call:set_text("Using a lua widget as a function Allows to set multiple attributes of that widget at once. This is mainly used to create UI elements in a more readable way"..para()..
      "For example:"..code([[
local widget = dt.new_widget("button"){
    label ="my label",
    clicked_callback = function() print "hello world" end
    }]]))
  types.lua_widget.__call:add_parameter("attributes","table","A table of attributes => value to set")
  types.lua_widget.__call:add_return(types.lua_widget,"The object called itself, to allow chaining")


  types.lua_container:set_text("A widget containing other widgets");
  types.lua_container.extra_registration_parameters:set_text("This widget has no extra registration parameters")
	types.lua_container["#"]:set_reported_type(types.lua_widget)
	types.lua_container["#"]:set_text("The widgets contained by the box"..para()..
      "You can append widgets by adding them at the end of the list"..para()..
      "You can remove widgets by setting them to nil")

  types.lua_check_button:set_text("A checkable button with a label next to it");
  types.lua_check_button.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_check_button.label:set_reported_type("string")
  types.lua_check_button.label:set_text("The label displayed next to the button");
  types.lua_check_button.value:set_text("If the widget is checked or not");
  types.lua_check_button.clicked_callback:set_text("A function to call on button click")
  types.lua_check_button.clicked_callback:set_reported_type("function")
  types.lua_check_button.clicked_callback:add_parameter("widget",types.lua_widget,"The widget that triggered the callback")

  types.lua_label:set_text("A label containing some text");
  types.lua_label.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_label.label:set_text("The label displayed");
  types.lua_label.selectable:set_text("True if the label content should be selectable");
  types.lua_label.halign:set_text("The horizontal alignment of the label");
  types.lua_label.halign:set_reported_type(types.dt_lua_align_t)
  types.lua_label.ellipsize:set_text("The ellipsize mode of the label");
  types.lua_label.ellipsize:set_reported_type(types.dt_lua_ellipsize_mode_t)

  types.lua_button:set_text("A clickable button");
  types.lua_button.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_button.label:set_reported_type("string")
  types.lua_button.label:set_text("The label displayed on the button");
  types.lua_button.clicked_callback:set_text("A function to call on button click")
  types.lua_button.clicked_callback:set_reported_type("function")
  types.lua_button.clicked_callback:add_parameter("widget",types.lua_widget,"The widget that triggered the callback")

  types.lua_box:set_text("A container for widget in a horizontal or vertical list");
  types.lua_box.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_box.orientation:set_text("The orientation of the box.")
  types.lua_box.orientation:set_reported_type(types.dt_lua_orientation_t)

  types.lua_entry:set_text("A widget in which the user can input text")
  types.lua_entry.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_entry.text:set_text("The content of the entry")
  types.lua_entry.placeholder:set_reported_type("string")
  types.lua_entry.placeholder:set_text("The text to display when the entry is empty")
  types.lua_entry.is_password:set_text("True if the text content should be hidden")
  types.lua_entry.editable:set_text("False if the entry should be read-only")

  types.lua_separator:set_text("A widget providing a separation in the UI.")
  types.lua_separator.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_separator.orientation:set_text("The orientation of the separator.")

  types.lua_combobox:set_text("A widget with multiple text entries in a menu"..para()..
      "This widget can be set as editable at construction time."..para()..
      "If it is editable the user can type a value and is not constrained by the values in the menu")
  types.lua_combobox.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_combobox.value:set_reported_type("string")
  types.lua_combobox.value:set_text("The text content of the selected entry, can be nil"..para()..
      "You can set it to a number to select the corresponding entry from the menu"..para()..
      "If the combo box is editable, you can set it to any string"..para()..
      "You can set it to nil to deselect all entries")
  types.lua_combobox.selected:set_text("The index of the selected entry, or 0 if nothing is selected"..para()..
      "You can set it to a number to select the corresponding entry from the menu, or to 0 to select nothing"..para()..
      "You can set it to nil to deselect all entries")
  types.lua_combobox.selected:set_reported_type("integer")
  types.lua_combobox["#"]:set_text("The various menu entries."..para()..
      "You can add new entries by writing to the first element beyond the end"..para()..
      "You can removes entries by setting them to nil")
  types.lua_combobox["#"]:set_reported_type("string")
  types.lua_combobox.changed_callback:set_text("A function to call when the value field changes (character entered or value selected)")
  types.lua_combobox.changed_callback:set_reported_type("function")
  types.lua_combobox.changed_callback:add_parameter("widget",types.lua_widget,"The widget that triggered the callback")
  types.lua_combobox.editable:set_text("True is the user is allowed to type a string in the combobox")
  types.lua_combobox.label:set_text("The label displayed on the combobox");

  types.lua_file_chooser_button:set_text("A button that allows the user to select an existing file")
  types.lua_file_chooser_button.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_file_chooser_button.title:set_text("The title of the window when choosing a file")
  types.lua_file_chooser_button.value:set_text("The currently selected file")
  types.lua_file_chooser_button.value:set_reported_type("string")
  types.lua_file_chooser_button.changed_callback:set_text("A function to call when the value field changes (character entered or value selected)")
  types.lua_file_chooser_button.changed_callback:set_reported_type("function")
  types.lua_file_chooser_button.changed_callback:add_parameter("widget",types.lua_widget,"The widget that triggered the callback")
  types.lua_file_chooser_button.is_directory:set_text("True if the file chooser button only allows directories to be selected")

  types.lua_stack:set_text("A container that will only show one of its child at a time")
  types.lua_stack.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_stack.active:set_text("The currently selected child, can be nil if the container has no child, can be set to one of the child widget or to an index in the child table")
  types.lua_stack.active:set_reported_type(my_tostring(types.lua_widget).." or nil")

  types.lua_slider:set_text("A slider that can be set by the user")
  types.lua_slider.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_slider.soft_min:set_text("The soft minimum value for the slider, the slider can't go beyond this point")
  types.lua_slider.soft_max:set_text("The soft maximum value for the slider, the slider can't go beyond this point")
  types.lua_slider.hard_min:set_text("The hard minimum value for the slider, the user can't manually enter a value beyond this point")
  types.lua_slider.hard_max:set_text("The hard maximum value for the slider, the user can't manually enter a value beyond this point")
  types.lua_slider.step:set_text("The step width of the slider")
  types.lua_slider.digits:set_text("The number of decimal digits shown on the slider")
  types.lua_slider.digits:set_reported_type("integer")
  types.lua_slider.value:set_text("The current value of the slider")
  types.lua_slider.label:set_text("The label next to the slider")
  types.lua_slider.label:set_reported_type("string")

  types.lua_text_view:set_text("A multiline text input widget")
  types.lua_text_view.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_text_view.text:set_text("The text in the widget")
  types.lua_text_view.editable:set_text("False if the entry should be read-only")

  types.lua_section_label:set_text("A section label");
  types.lua_section_label.extra_registration_parameters:set_text("This widget has no extra registration parameters")
  types.lua_section_label.label:set_text("The section name");

	----------------------
	--  EVENTS          --
	----------------------
	events:set_text([[This section documents events that can be used to trigger Lua callbacks.]])


	events["intermediate-export-image"]:set_text([[This event is called each time an image is exported, once for each image after the image has been processed to an image format but before the storage has moved the image to its final destination. The call is blocking.]])
	events["intermediate-export-image"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])
	events["intermediate-export-image"].callback:add_parameter("image",types.dt_lua_image_t,[[The image object that has been exported.]])
	events["intermediate-export-image"].callback:add_parameter("filename","string",[[The name of the file that is the result of the image being processed.]])
	events["intermediate-export-image"].callback:add_parameter("format",types.dt_imageio_module_format_t,[[The format used to export the image.]])
	events["intermediate-export-image"].callback:add_parameter("storage",types.dt_imageio_module_storage_t,[[The storage used to export the image (can be nil).]])
	events["intermediate-export-image"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])


	events["post-import-image"]:set_text([[This event is triggered whenever a new image is imported into the database.

	This event can be registered multiple times, all callbacks will be called. The call is blocking.]])
	events["post-import-image"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])
	events["post-import-image"].callback:add_parameter("image",types.dt_lua_image_t,[[The image object that has been imported.]])
	events["post-import-image"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])


	events["shortcut"]:set_text([[This event registers a new keyboard shortcut. The shortcut isn't bound to any key until the users does so in the preference panel.

	The event is triggered whenever the shortcut is triggered.


	This event can only be registered once per value of shortcut.
	]])
	events["shortcut"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])

	events["shortcut"].callback:add_parameter("shortcut","string",[[The tooltip string that was given at registration time.]])
	events["shortcut"].extra_registration_parameters:set_text("")
	events["shortcut"].extra_registration_parameters:add_parameter("tooltip","string",[[The string that will be displayed on the shortcut preference panel describing the shortcut.]])



	events["post-import-film"]:set_text([[This event is triggered when an film import is finished (all post-import-image callbacks have already been triggered). This event can be registered multiple times.
	]])
	events["post-import-film"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])

	events["post-import-film"].callback:add_parameter("film",types.dt_lua_film_t,[[The new film that has been added. If multiple films were added recursively only the top level film is reported.]])
	events["post-import-film"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])

	events["view-changed"]:set_text([[This event is triggered after the user changed the active view]])
	events["view-changed"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])
	events["view-changed"].callback:add_parameter("old_view",types.dt_lua_view_t,[[The view that we just left]])
	events["view-changed"].callback:add_parameter("new_view",types.dt_lua_view_t,[[The view we are now in]])
	events["view-changed"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])

	events["global_toolbox-grouping_toggle"]:set_text([[This event is triggered after the user toggled the grouping button.]])
	events["global_toolbox-grouping_toggle"].callback:add_parameter("toggle", "boolean", [[the new grouping status.]]);
	events["global_toolbox-grouping_toggle"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])
	events["global_toolbox-overlay_toggle"]:set_text([[This event is triggered after the user toggled the overlay button.]])
	events["global_toolbox-overlay_toggle"].callback:add_parameter("toggle", "boolean", [[the new overlay status.]]);
	events["global_toolbox-overlay_toggle"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])

  events["mouse-over-image-changed"]:set_text([[This event is triggered whenever the image under the mouse changes]])
	events["mouse-over-image-changed"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])
  events["mouse-over-image-changed"].callback:add_parameter("image",types.dt_lua_image_t,[[The new image under the mouse, can be nil if there is no image under the mouse]])
	events["mouse-over-image-changed"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])
  events["exit"]:set_text([[This event is triggered when darktable exits, it allows lua scripts to do cleanup jobs]])
	events["exit"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])

  events["pre-import"]:set_text("This event is trigger before any import action");
	events["pre-import"].callback:add_parameter("event","string",[[The name of the event that triggered the callback.]])
	events["pre-import"].callback:add_parameter("images","table of string",[[The files that will be imported. Modifying this table will change the list of files that will be imported"]])
	events["pre-import"].extra_registration_parameters:set_text([[This event has no extra registration parameters.]])
	----------------------
	--  ATTRIBUTES      --
	----------------------
	function invisible_attr(attr)
		attr:set_skipped()
		attr:set_attribute("internal_attr",true);
	end
	attributes:set_text([[This section documents various attributes used throughout the documentation.]])
	invisible_attr(attributes.ret_val)
	invisible_attr(attributes.signature)
	invisible_attr(attributes.reported_type)
	invisible_attr(attributes.is_singleton)
	invisible_attr(attributes.optional)
	invisible_attr(attributes.skipped)
	invisible_attr(attributes.is_attribute)
	invisible_attr(attributes.internal_attr)
	invisible_attr(attributes.read)
	invisible_attr(attributes.has_pairs)
	invisible_attr(attributes.is_self)
	invisible_attr(attributes.has_length)
	attributes.write:set_text([[This object is a variable that can be written to.]])
  --attributes.has_pairs:set_text([[This object can be used as an argument to the system function "pairs" and iterated upon.]])
	--attributes.has_equal:set_text([[This object has a specific comparison function that will be used when comparing it to an object of the same type.]])
	--attributes.has_length:set_text([[This object has a specific length function that will be used by the # operator.]])
	attributes.has_tostring:set_text([[This object has a specific reimplementation of the "tostring" method that allows pretty-printing it.]])
	attributes.implicit_yield:set_text([[This call will release the Lua lock while executing, thus allowing other Lua callbacks to run.]])
	attributes.parent:set_text([[This object inherits some methods from another object. You can call the methods from the parent on the child object]])
	--attributes.views:set_skipped();
	--attributes.position:set_skipped();
	--attributes.container:set_skipped();
	attributes.values:set_skipped();

--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
