API = {
["__text"] = [==[This documentation is for the *development* version of darktable. for the stable version, please visit the user manual
To access the darktable specific functions you must load the darktable environment:<code>darktable = require "darktable"</code>All functions and data are accessed through the darktable module.
This documentation for API version 3.0.0.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["darktable"] = {
["__text"] = [==[The darktable library is the main entry point for all access to the darktable internals.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["print"] = {
["__text"] = [==[Will print a string to the darktable control log (the long overlaid window that appears over the main panel).]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The string to display which should be a single line.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["print_error"] = {
["__text"] = [==[This function will print its parameter if the Lua logdomain is activated. Start darktable with the "-d lua" command line option to enable the Lua logdomain.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The string to display.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["register_event"] = {
["__text"] = [==[This function registers a callback to be called when a given event happens.
Events are documented in the event section.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event to register to.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The function to call on event. The signature of the function depends on the type of event.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
},
},
["3"] = {
["__text"] = [==[Some events need extra parameters at registration time; these must be specified here.]==],
["__attributes"] = {
["reported_type"] = [==[variable]==],
},
},
},
},
},
["register_storage"] = {
["__text"] = [==[This function will add a new storage implemented in Lua.
A storage is a module that is responsible for handling images once they have been generated during export. Examples of core storages include filesystem, e-mail, facebook...]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[A Unique name for the plugin.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[A human readable name for the plugin.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[This function is called once for each exported image. Images can be exported in parallel but the calls to this function will be serialized.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The storage object used for the export.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A virtual type representing all storage types.]==],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["plugin_name"] = {
["__text"] = [==[A unique name for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["name"] = {
["__text"] = [==[A human readable name for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["width"] = {
["__text"] = [==[The currently selected width for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["height"] = {
["__text"] = [==[The currently selected height for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["recommended_width"] = {
["__text"] = [==[The recommended width for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["recommended_height"] = {
["__text"] = [==[The recommended height for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["supports_format"] = {
["__text"] = [==[Checks if a format is supported by this storage.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[True if the format is supported by the storage.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The storage type to check against.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The format type to check.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A virtual type representing all format types.]==],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["plugin_name"] = {
["__text"] = [==[A unique name for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["name"] = {
["__text"] = [==[A human readable name for the plugin.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["extension"] = {
["__text"] = [==[The typical filename extension for that format.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["mime"] = {
["__text"] = [==[The mime type associated with the format.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["max_width"] = {
["__text"] = [==[The max width allowed for the format (0 = unlimited).]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["max_height"] = {
["__text"] = [==[The max height allowed for the format (0 = unlimited).]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["write_image"] = {
["__text"] = [==[Exports an image to a file. This is a blocking operation that will not return until the image is exported.]==],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[Returns true on success.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The format that will be used to export.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The image object to export.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[Image objects represent an image in the database. This is slightly different from a file on disk since a file can have multiple developments.

	Note that this is the real image object; changing the value of a field will immediately change it in darktable and will be reflected on any copy of that image object you may have kept.]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["attach_tag"] = {
["__text"] = [==[Attach a tag to an image; the order of the parameters can be reversed.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The tag to be attached.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A tag that can be attached to an image.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["delete"] = {
["__text"] = [==[Deletes the tag object, detaching it from all images.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The tag to be deleted.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["attach"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]]=],
["detach"] = {
["__text"] = [==[Detach a tag from an image; the order of the parameters can be reversed.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The tag to be detached.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The image to detach the tag from.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["name"] = {
["__text"] = [==[The name of the tag.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["#"] = {
["__text"] = [==[The images that have that tag attached to them.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["2"] = {
["__text"] = [==[The image to attach the tag to.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["detach_tag"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]]=],
["get_tags"] = {
["__text"] = [==[Gets all tags attached to an image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[A table of tags that are attached to the image.]==],
["__attributes"] = {
["reported_type"] = [==[table of types.dt_lua_tag_t]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The image to get the tags from.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["create_style"] = {
["__text"] = [==[Create a new style based on an image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The new style object.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A style that can be applied to an image.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["delete"] = {
["__text"] = [==[Deletes an existing style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[the style to delete]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
},
},
},
["duplicate"] = {
["__text"] = [==[Create a new style based on an existing style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The new style object.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The style to base the new style on.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The new style's name.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The new style's description.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["apply"] = {
["__text"] = [==[Apply a style to an image. The order of parameters can be inverted.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The style to use.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The image to apply the style to.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["export"] = {
["__text"] = [==[Export a style to an external .dtstyle file]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The style to export]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The directory to export to]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[Is overwriting an existing file allowed]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
},
},
},
["name"] = {
["__text"] = [==[The name of the style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["description"] = {
["__text"] = [==[The description of the style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["#"] = {
["__text"] = [==[The different items that make the style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[An element that is part of a style.]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["name"] = {
["__text"] = [==[The name of the style item.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["num"] = {
["__text"] = [==[The position of the style item within its style.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
},
},
},
},
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The image to create the style from.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The name to give to the new style.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The description of the new style.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["apply_style"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]]=],
["duplicate"] = {
["__text"] = [==[Creates a duplicate of an image and returns it.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The new image object.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[the image to duplicate]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["move"] = {
["__text"] = [==[Physically moves an image (and all its duplicates) to another film.
This will move the image file, the related XMP and all XMP for the duplicates to the directory of the new film
Note that the parameter order is not relevant.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image to move]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The film to move to]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A film in darktable; this represents a directory containing imported images.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["move_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]]=],
["copy_image"] = {
["__text"] = [==[Physically copies an image to another film.
This will copy the image file and the related XMP to the directory of the new film
If there is already a file with the same name as the image file, it will create a duplicate from that file instead
Note that the parameter order is not relevant.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The new image]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The image to copy]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The film to copy to]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["#"] = {
["__text"] = [==[The different images within the film.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["id"] = {
["__text"] = [==[A unique numeric id used by this film.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["path"] = {
["__text"] = [==[The path represented by this film.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["delete"] = {
["__text"] = [==[Removes the film from the database.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The film to remove.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[Force removal, even if the film is not empty.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[Boolean]==],
},
},
},
},
},
},
},
},
},
},
},
["copy"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]]=],
["id"] = {
["__text"] = [==[A unique id identifying the image in the database.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["path"] = {
["__text"] = [==[The file the directory containing the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["film"] = {
["__text"] = [==[The film object that contains this image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["filename"] = {
["__text"] = [==[The filename of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["sidecar"] = {
["__text"] = [==[The filename of the image's sidecar file.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["duplicate_index"] = {
["__text"] = [==[If there are multiple images based on a same file, each will have a unique number, starting from 0.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["publisher"] = {
["__text"] = [==[The publisher field of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["title"] = {
["__text"] = [==[The title field of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["creator"] = {
["__text"] = [==[The creator field of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["rights"] = {
["__text"] = [==[The rights field of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["description"] = {
["__text"] = [==[The description field for the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["exif_maker"] = {
["__text"] = [==[The maker exif data.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["exif_model"] = {
["__text"] = [==[The camera model used.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["exif_lens"] = {
["__text"] = [==[The id string of the lens used.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["exif_aperture"] = {
["__text"] = [==[The aperture saved in the exif data.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["exif_exposure"] = {
["__text"] = [==[The exposure time of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["exif_focal_length"] = {
["__text"] = [==[The focal length of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["exif_iso"] = {
["__text"] = [==[The iso used on the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["exif_datetime_taken"] = {
["__text"] = [==[The date and time of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["exif_focus_distance"] = {
["__text"] = [==[The distance of the subject.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["exif_crop"] = {
["__text"] = [==[The exif crop data.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["latitude"] = {
["__text"] = [==[GPS latitude data of the image, nil if not set.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[float or nil]==],
["write"] = true,
},
},
["longitude"] = {
["__text"] = [==[GPS longitude data of the image, nil if not set.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[float or nil]==],
["write"] = true,
},
},
["elevation"] = {
["__text"] = [==[GPS altitude data of the image, nil if not set.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[float or nil]==],
["write"] = true,
},
},
["is_raw"] = {
["__text"] = [==[True if the image is a RAW file.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
["is_ldr"] = {
["__text"] = [==[True if the image is a ldr image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
["is_hdr"] = {
["__text"] = [==[True if the image is a hdr image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
["has_txt"] = {
["__text"] = [==[True if the image has a txt sidecar file.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["width"] = {
["__text"] = [==[The width of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["height"] = {
["__text"] = [==[The height of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["rating"] = {
["__text"] = [==[The rating of the image (-1 for rejected).]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["red"] = {
["__text"] = [==[True if the image has the corresponding colorlabel.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["blue"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["green"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["yellow"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["purple"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["reset"] = {
["__text"] = [==[Removes all processing from the image, resetting it back to its original state]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image whose history will be deleted]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["delete"] = {
["__text"] = [==[Removes an image from the database]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image to remove]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["group_with"] = {
["__text"] = [==[Puts the first image in the same group as the second image. If no second image is provided the image will be in its own group.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image whose group must be changed.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The image we want to group with.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["make_group_leader"] = {
["__text"] = [==[Makes the image the leader of its group.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image we want as the leader.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["get_group_members"] = {
["__text"] = [==[Returns a table containing all types.dt_lua_image_t of the group. The group leader is both at a numeric key and at the "leader" special key (so you probably want to use ipairs to iterate through that table).]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[A table of image objects containing all images that are in the same group as the image.]==],
["__attributes"] = {
["reported_type"] = [==[table of types.dt_lua_image_t]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The image whose group we are querying.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["group_leader"] = {
["__text"] = [==[The image which is the leader of the group this image is a member of.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["local_copy"] = {
["__text"] = [==[True if the image has a copy in the local cache]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["drop_cache"] = {
["__text"] = [==[drops the cached version of this image.
This function should be called if an image is modified out of darktable to force DT to regenerate the thumbnail
darktable will regenerate the thumbnail by itself when it is needed]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The image whose cache must be dropped.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
},
},
},
["3"] = {
["__text"] = [==[The filename to export to.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["4"] = {
["__text"] = [==[Set to true to allow upscaling of the image.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[boolean]==],
},
},
},
},
},
},
},
},
},
},
},
},
},
},
["2"] = {
["__text"] = [==[The exported image object.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [==[The format object used for the export.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["4"] = {
["__text"] = [==[The name of a temporary file where the processed image is stored.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["5"] = {
["__text"] = [==[The number of the image out of the export series.]==],
["__attributes"] = {
["reported_type"] = [==[integer]==],
},
},
["6"] = {
["__text"] = [==[The total number of images in the export series.]==],
["__attributes"] = {
["reported_type"] = [==[integer]==],
},
},
["7"] = {
["__text"] = [==[True if the export is high quality.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["8"] = {
["__text"] = [==[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export serie.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
},
},
},
["4"] = {
["__text"] = [==[This function is called once all images are processed and all store calls are finished.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The storage object used for the export.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[A table keyed by the exported image objects and valued with the corresponding temporary export filename.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
["3"] = {
["__text"] = [==[An empty Lua table to store extra data. This table is common to all calls to store and the call to finalize in a given export series.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
},
},
},
["5"] = {
["__text"] = [==[A function called to check if a given image format is supported by the Lua storage; this is used to build the dropdown format list for the GUI.
Note that the parameters in the format are the ones currently set in the GUI; the user might change them before export.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[True if the corresponding format is supported.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The storage object tested.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The format object to report about.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["6"] = {
["__text"] = [==[A function called before storage happens
This function can change the list of exported functions]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The modified table of images to export or nil
If nil (or nothing) is returned, the original list of images will be exported
If a table of images is returned, that table will be used instead. The table can be empty. The images parameter can be modified and returned]==],
["__attributes"] = {
["reported_type"] = [==[table or nil]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The storage object tested.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The format object to report about.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [==[A table containing images to be exported.]==],
["__attributes"] = {
["reported_type"] = [==[table of types.dt_lua_image_t]==],
},
},
["4"] = {
["__text"] = [==[True if the export is high quality.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["5"] = {
["__text"] = [==[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export serie.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
},
},
},
["7"] = {
["__text"] = [==[A widget to display in the export section of darktable's UI]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = {
["__text"] = [==[Common parent type for all lua-handled widgets]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["sensitive"] = {
["__text"] = [==[Set if the widget is enabled/disabled]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["tooltip"] = {
["__text"] = [==[Tooltip to display for the widget]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string or nil]==],
["write"] = true,
},
},
["reset_callback"] = {
["__text"] = [==[A function to call when the widget needs to reset itself
Note that some widgets have a default implementation that can be overridden, (containers in particular will recursively reset their children). If you replace that default implementation you need to reimplement that functionality or call the original function within your callback]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget that triggered the callback]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["write"] = true,
},
},
["__call"] = {
["__text"] = [==[Using a lua widget as a function Allows to set multiple attributes of that widget at once. This is mainly used to create UI elements in a more readable way
For example:<code>local widget = dt.new_widget("button"){
    label ="my label",
    clicked_callback = function() print "hello world" end
    }</code>]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The object called itself, to allow chaining]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[A table of attributes => value to set]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
},
},
},
},
},
},
},
},
},
["register_lib"] = {
["__text"] = [==[Register a new lib object. A lib is a graphical element of darktable's user interface]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[A unique name for your library]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[A user-visible name for your library]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[whether this lib should be expandable or not]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["4"] = {
["__text"] = [==[whether this lib has a reset button or not]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["5"] = {
["__text"] = [==[A table associating to each view containing the lib the corresponding container and position]==],
["__attributes"] = {
["reported_type"] = [==[table of types.dt_lua_view_t => [ types.dt_ui_container_t, int ]]==],
},
},
["6"] = {
["__text"] = [==[The widget to display in the lib]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
["7"] = {
["__text"] = [==[A callback called when a view displaying the lib is entered]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The lib on which the callback is called]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {
["__text"] = [==[The type of a UI lib]==],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["id"] = {
["__text"] = [==[A unit string identifying the lib]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["name"] = {
["__text"] = [==[The translated title of the UI element]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["version"] = {
["__text"] = [==[The version of the internal data of this lib]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["visible"] = {
["__text"] = [==[Allow to make a lib module completely invisible to the user.
Note that if the module is invisible the user will have no way to restore it without lua]==],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["container"] = {
["__text"] = [==[The location of the lib in the darktable UI]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[A place in the darktable UI where a lib can be placed]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[DT_UI_CONTAINER_PANEL_LEFT_TOP]==],
["2"] = [==[DT_UI_CONTAINER_PANEL_LEFT_CENTER]==],
["3"] = [==[DT_UI_CONTAINER_PANEL_LEFT_BOTTOM]==],
["4"] = [==[DT_UI_CONTAINER_PANEL_RIGHT_TOP]==],
["5"] = [==[DT_UI_CONTAINER_PANEL_RIGHT_CENTER]==],
["6"] = [==[DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM]==],
["7"] = [==[DT_UI_CONTAINER_PANEL_TOP_LEFT]==],
["8"] = [==[DT_UI_CONTAINER_PANEL_TOP_CENTER]==],
["9"] = [==[DT_UI_CONTAINER_PANEL_TOP_RIGHT]==],
["10"] = [==[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT]==],
["11"] = [==[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER]==],
["12"] = [==[DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT]==],
["13"] = [==[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT]==],
["14"] = [==[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER]==],
["15"] = [==[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT]==],
["16"] = [==[DT_UI_CONTAINER_PANEL_BOTTOM]==],
},
},
},
},
},
["expandable"] = {
["__text"] = [==[True if the lib can be expanded/retracted]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
["expanded"] = {
["__text"] = [==[True if the lib is expanded]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["position"] = {
["__text"] = [==[A value deciding the position of the lib within its container]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
["views"] = {
["__text"] = [==[A table of all the views that display this widget]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[table]==],
},
},
["reset"] = {
["__text"] = [==[A function to reset the lib to its default values
This function will do nothing if the lib is not visible or can't be reset]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The lib to reset]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["on_screen"] = {
["__text"] = [==[True if the lib is currently visible on the screen]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
},
},
},
["2"] = {
["__text"] = [==[The view that we are leaving]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A darktable view]==],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["id"] = {
["__text"] = [==[A unique string identifying the view]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["name"] = {
["__text"] = [==[The name of the view]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
},
},
},
["3"] = {
["__text"] = [==[The view that we are entering]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["8"] = {
["__text"] = [==[A callback called when leaving a view displaying the lib]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The lib on which the callback is called]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The view that we are leaving]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [==[The view that we are entering]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
},
},
},
["films"] = {
["__text"] = [==[A table containing all the film objects in the database.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["#"] = {
["__text"] = [==[Each film has a numeric entry in the database.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["new"] = {
["__text"] = [==[Creates a new empty film
 see darktable.database.import to import a directory with all its images and to add images to a film]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created film, or the existing film if the directory is already imported]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The directory that the new film will represent. The directory must exist]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]]=],
},
["new_format"] = {
["__text"] = [==[Creates a new format object to export images]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created object. Exact type depends on the type passed]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The type of format object to create, one of : 

* copy
* exr
* j2k
* jpeg
* pdf
* pfm
* png
* ppm
* tiff
* webp
]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["new_storage"] = {
["__text"] = [==[Creates a new storage object to export images]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created object. Exact type depends on the type passed]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The type of storage object to create, one of : 

* disk
* email
* facebook
* flickr
* gallery
* latex
* picasa
(Other, lua-defined, storage types may appear.)]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["new_widget"] = {
["__text"] = [==[Creates a new widget object to display in the UI]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created object. Exact type depends on the type passed]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The type of storage object to create, one of : 

* box
* button
* check_button
* combobox
* container
* entry
* file_chooser_button
* label
* separator
* slider
* stack
]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["gui"] = {
["__text"] = [==[This subtable contains function and data to manipulate the darktable user interface with Lua.
Most of these function won't do anything if the GUI is not enabled (i.e you are using the command line version darktabl-cli instead of darktable).]==],
["__attributes"] = {
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["action_images"] = {
["__text"] = [==[A table of types.dt_lua_image_t on which the user expects UI actions to happen.
It is based on both the hovered image and the selection and is consistent with the way darktable works.
It is recommended to use this table to implement Lua actions rather than darktable.gui.hovered or darktable.gui.selection to be consistent with darktable's GUI.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[table]==],
},
},
["hovered"] = {
["__text"] = [==[The image under the cursor or nil if no image is hovered.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["selection"] = {
["__text"] = [==[Allows to change the set of selected images.]==],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[A table containing the selection as it was before the function was called.]==],
["__attributes"] = {
["reported_type"] = [==[table of types.dt_lua_image_t]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[A table of images which will define the selected images. If this parameter is not given the selection will be untouched. If an empty table is given the selection will be emptied.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[table of types.dt_lua_image_t]==],
},
},
},
},
},
["current_view"] = {
["__text"] = [==[Allows to change the current view.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[the current view]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The view to switch to. If empty the current view is unchanged]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["create_job"] = {
["__text"] = [==[Create a new progress_bar displayed in darktable.gui.libs.backgroundjobs]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created job object]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A lua-managed entry in the backgroundjob lib]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["percent"] = {
["__text"] = [==[The value of the progress bar, between 0 and 1. will return nil if there is no progress bar, will raise an error if read or written on an invalid job]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["valid"] = {
["__text"] = [==[True if the job is displayed, set it to false to destroy the entry
An invalid job cannot be made valid again]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
},
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The text to display in the job entry]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[Should a progress bar be displayed]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[boolean]==],
},
},
["3"] = {
["__text"] = [==[A function called when the cancel button for that job is pressed
note that the job won't be destroyed automatically. You need to set types.dt_lua_backgroundjob_t.valid to false for that]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The extra information to display]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The job who is being cancelded]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The image to analyze]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
},
},
},
["views"] = {
["__text"] = [==[The different views in darktable]==],
["__attributes"] = {
["has_pairs"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["map"] = {
["__text"] = [==[The map view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["latitude"] = {
["__text"] = [==[The latitude of the center of the map]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["longitude"] = {
["__text"] = [==[The longitude of the center of the map]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["zoom"] = {
["__text"] = [==[The current zoom level of the map]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
},
["darkroom"] = {
["__text"] = [==[The darkroom view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["lighttable"] = {
["__text"] = [==[The lighttable view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["tethering"] = {
["__text"] = [==[The tethering view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["slideshow"] = {
["__text"] = [==[The slideshow view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["print"] = {
["__text"] = [==[The print view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
},
["libs"] = {
["__text"] = [==[This table allows to reference all lib objects
lib are the graphical blocks within each view.
To quickly figure out what lib is what, you can use the following code which will make a given lib blink.
<code>local tested_module="global_toolbox"
dt.gui.libs[tested_module].visible=false
coroutine.yield("WAIT_MS",2000)
while true do
	dt.gui.libs[tested_module].visible = not dt.gui.libs[tested_module].visible
	coroutine.yield("WAIT_MS",2000)
end</code>]==],
["__attributes"] = {
["has_pairs"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["snapshots"] = {
["__text"] = [==[The UI element that manipulates snapshots in darkroom]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["ratio"] = {
["__text"] = [==[The place in the screen where the line separating the snapshot is. Between 0 and 1]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["direction"] = {
["__text"] = [==[The direction of the snapshot overlay]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[Which part of the main window is occupied by a snapshot]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[left]==],
["2"] = [==[right]==],
["3"] = [==[top]==],
["4"] = [==[bottom]==],
},
},
},
["write"] = true,
},
},
["#"] = {
["__text"] = [==[The different snapshots for the image]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[The description of a snapshot in the snapshot lib]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [==[dt_type]==],
},
["filename"] = {
["__text"] = [==[The filename of an image containing the snapshot]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["select"] = {
["__text"] = [==[Activates this snapshot on the display. To deactivate all snapshot you need to call this function on the active snapshot]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The snapshot to activate]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]]=],
},
},
},
},
},
["name"] = {
["__text"] = [==[The name of the snapshot, as seen in the UI]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
},
},
},
["selected"] = {
["__text"] = [==[The currently selected snapshot]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]]=],
},
},
["take_snapshot"] = {
["__text"] = [==[Take a snapshot of the current image and add it to the UI
The snapshot file will be generated at the next redraw of the main window]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
},
},
},
["max_snapshot"] = {
["__text"] = [==[The maximum number of snapshots]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
},
},
},
["collect"] = {
["__text"] = [==[The collection UI element that allows to filter images by collection]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["filter"] = {
["__text"] = [==[Allows to get or change the list of visible images]==],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The rules that were applied before this call.]==],
["__attributes"] = {
["reported_type"] = [==[array oftypes.dt_lib_collect_params_rule_t]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[A table of rules describing the filter. These rules will be applied after this call]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[array oftypes.dt_lib_collect_params_rule_t]==],
},
},
},
},
},
["new_rule"] = {
["__text"] = [==[Returns a newly created rule object]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The newly created rule]==],
["__attributes"] = {
["reported_type"] = [==[types.dt_lib_collect_params_rule_t]==],
},
},
["signature"] = {
},
},
},
},
["import"] = {
["__text"] = [==[The buttons to start importing images]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["register_widget"] = {
["__text"] = [==[Add a widget in the option expander of the import dialog]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget to add to the dialog. The reset callback of the widget will be called whenever the dialog is opened]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
},
},
},
["styles"] = {
["__text"] = [==[The style selection menu]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["metadata_view"] = {
["__text"] = [==[The widget displaying metadata about the current image]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["metadata"] = {
["__text"] = [==[The widget allowing modification of metadata fields on the current image]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["hinter"] = {
["__text"] = [==[The small line of text at the top of the UI showing the number of selected images]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["modulelist"] = {
["__text"] = [==[The window allowing to set modules as visible/hidden/favorite]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["filmstrip"] = {
["__text"] = [==[The filmstrip at the bottom of some views]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["viewswitcher"] = {
["__text"] = [==[The labels allowing to switch view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["darktable_label"] = {
["__text"] = [==[The darktable logo in the upper left corner]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["tagging"] = {
["__text"] = [==[The tag manipulation UI]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["geotagging"] = {
["__text"] = [==[The geotagging time synchronisation UI]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["recentcollect"] = {
["__text"] = [==[The recent collection UI element]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["global_toolbox"] = {
["__text"] = [==[The common tools to all view (settings, grouping...)]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["grouping"] = {
["__text"] = [==[The current status of the image grouping option]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["show_overlays"] = {
["__text"] = [==[the current status of the image overlays option]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
},
["filter"] = {
["__text"] = [==[The image-filter menus at the top of the UI]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["ratings"] = {
["__text"] = [==[The starts to set the rating of an image]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["select"] = {
["__text"] = [==[The buttons that allow to quickly change the selection]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["register_selection"] = {
["__text"] = [==[Add a new button and call a callback when it is clicked]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The label to display on the button]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The function to call when the button is pressed]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The images to set the selection to]==],
["__attributes"] = {
["reported_type"] = [==[table oftypes.dt_lua_image_t]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The name of the button that was pressed]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The images in the current collection. This is the same content asdarktable.collection]==],
["__attributes"] = {
["reported_type"] = [==[table oftypes.dt_lua_image_t]==],
},
},
},
},
},
["3"] = {
["__text"] = [==[The tooltip to use on the new button]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[string]==],
},
},
},
},
},
},
["colorlabels"] = {
["__text"] = [==[The color buttons that allow to set labels on an image]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["lighttable_mode"] = {
["__text"] = [==[The navigation and zoom level UI in lighttable]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["copy_history"] = {
["__text"] = [==[The UI element that manipulates history]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["image"] = {
["__text"] = [==[The UI element that manipulates the current images]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["register_action"] = {
["__text"] = [==[Add a new button and call a callback when it is clicked]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The label to display on the button]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The function to call when the button is pressed]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the button that was pressed]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The images to act on when the button was clicked]==],
["__attributes"] = {
["reported_type"] = [==[table oftypes.dt_lua_image_t]==],
},
},
},
},
},
["3"] = {
["__text"] = [==[The tooltip to use on the new button]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[string]==],
},
},
},
},
},
},
["modulegroups"] = {
["__text"] = [==[The icons describing the different iop groups]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["module_toolbox"] = {
["__text"] = [==[The tools on the bottom line of the UI (overexposure)]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["session"] = {
["__text"] = [==[The session UI when tethering]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["histogram"] = {
["__text"] = [==[The histogram widget]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["export"] = {
["__text"] = [==[The export menu]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["history"] = {
["__text"] = [==[The history manipulation menu]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["colorpicker"] = {
["__text"] = [==[The colorpicker menu]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["navigation"] = {
["__text"] = [==[The full image preview to allow navigation]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["masks"] = {
["__text"] = [==[The masks window]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["view_toolbox"] = {
["__text"] = [==[]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["live_view"] = {
["__text"] = [==[The liveview window]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["map_settings"] = {
["__text"] = [==[The map setting window]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["camera"] = {
["__text"] = [==[The camera selection UI]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["location"] = {
["__text"] = [==[The location ui]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["backgroundjobs"] = {
["__text"] = [==[The window displaying the currently running jobs]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
["print_settings"] = {
["__text"] = [==[The settings window in the print view]==],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [==[dt_singleton]==],
},
},
},
},
["guides"] = {
["__text"] = [==[Guide lines to overlay over an image in crop and rotate.
All guides are clipped to the drawing area.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
["register_guide"] = {
["__text"] = [==[Register a new guide.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the guide to show in the GUI.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The function to call to draw the guide lines. The drawn lines will be stroked by darktable.
THIS IS RUNNING IN THE GUI THREAD AND HAS TO BE FAST!]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The cairo object used for drawing.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[A wrapper around a cairo drawing context.
You probably shouldn't use this after the callback that got it passed returned.
For more details of the member functions have a look at the cairo documentation for <ulink url="http://www.cairographics.org/manual/cairo-cairo-t.html">the drawing context</ulink>, <ulink url="http://www.cairographics.org/manual/cairo-Transformations.html">transformations</ulink> and <ulink url="http://www.cairographics.org/manual/cairo-Paths.html">paths</ulink>.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["save"] = {
["__text"] = [==[Save the state of the drawing context.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["restore"] = {
["__text"] = [==[Restore a previously saved state.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["move_to"] = {
["__text"] = [==[Begin a new sub-path.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x coordinate of the new position.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y coordinate of the new position.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["line_to"] = {
["__text"] = [==[Add a line to the path.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x coordinate of the end of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y coordinate of the end of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["rectangle"] = {
["__text"] = [==[Add a closed sub-path rectangle.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x coordinate of the top left corner of the rectangle.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y coordinate of the top left corner of the rectangle.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["4"] = {
["__text"] = [==[The width of the rectangle.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["5"] = {
["__text"] = [==[The height of the rectangle.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["arc"] = {
["__text"] = [==[Add a circular arc.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x position of the center of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y position of the center of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["4"] = {
["__text"] = [==[The radius of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["5"] = {
["__text"] = [==[The start angle, in radians.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["6"] = {
["__text"] = [==[The end angle, in radians.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["arc_negative"] = {
["__text"] = [==[Add a circular arc. It only differs in the direction from types.dt_lua_cairo_t.arc.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x position of the center of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y position of the center of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["4"] = {
["__text"] = [==[The radius of the arc.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["5"] = {
["__text"] = [==[The start angle, in radians.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["6"] = {
["__text"] = [==[The end angle, in radians.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["rotate"] = {
["__text"] = [==[Add a rotation to the transformation matrix.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The angle (in radians) by which the user-space axes will be rotated.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["scale"] = {
["__text"] = [==[Add a scaling to the transformation matrix.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The scale factor for the x dimension.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The scale factor for the y dimension.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["translate"] = {
["__text"] = [==[Add a translation to the transformation matrix.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[Amount to translate in the x direction]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[Amount to translate in the y direction]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["new_sub_path"] = {
["__text"] = [==[Begin a new sub-path.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["draw_line"] = {
["__text"] = [==[Helper function to draw a line with a given start and end.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The context to modify.]==],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [==[The x coordinate of the start of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y coordinate of the start of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["4"] = {
["__text"] = [==[The x coordinate of the end of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["5"] = {
["__text"] = [==[The y coordinate of the end of the new line.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
},
},
},
["2"] = {
["__text"] = [==[The x coordinate of the top left corner of the drawing area.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["3"] = {
["__text"] = [==[The y coordinate of the top left corner of the drawing area.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["4"] = {
["__text"] = [==[The width of the drawing area.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["5"] = {
["__text"] = [==[The height of the drawing area.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
["6"] = {
["__text"] = [==[The current zoom_scale. Only needed when setting the line thickness.]==],
["__attributes"] = {
["reported_type"] = [==[float]==],
},
},
},
},
},
["3"] = {
["__text"] = [==[A function returning a widget to show when the guide is selected. It takes no arguments.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[function]==],
},
},
},
},
},
},
["tags"] = {
["__text"] = [==[Allows access to all existing tags.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["#"] = {
["__text"] = [==[Each existing tag has a numeric entry in the tags table - use ipairs to iterate over them.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["create"] = {
["__text"] = [==[Creates a new tag and return it. If the tag exists return the existing tag.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the new tag.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["find"] = {
["__text"] = [==[Returns the tag object or nil if the tag doesn't exist.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The tag object or nil.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The name of the tag to find.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["delete"]]=],
["attach"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]]=],
["detach"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]]=],
["get_tags"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["get_tags"]]=],
},
["configuration"] = {
["__text"] = [==[This table regroups values that describe details of the configuration of darktable.]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
["version"] = {
["__text"] = [==[The version number of darktable.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["has_gui"] = {
["__text"] = [==[True if darktable has a GUI (launched through the main darktable command, not darktable-cli).]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["verbose"] = {
["__text"] = [==[True if the Lua logdomain is enabled.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["tmp_dir"] = {
["__text"] = [==[The name of the directory where darktable will store temporary files.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["config_dir"] = {
["__text"] = [==[The name of the directory where darktable will find its global configuration objects (modules).]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["cache_dir"] = {
["__text"] = [==[The name of the directory where darktable will store its mipmaps.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["api_version_major"] = {
["__text"] = [==[The major version number of the lua API.]==],
["__attributes"] = {
["reported_type"] = [==[number]==],
},
},
["api_version_minor"] = {
["__text"] = [==[The minor version number of the lua API.]==],
["__attributes"] = {
["reported_type"] = [==[number]==],
},
},
["api_version_patch"] = {
["__text"] = [==[The patch version number of the lua API.]==],
["__attributes"] = {
["reported_type"] = [==[number]==],
},
},
["api_version_suffix"] = {
["__text"] = [==[The version suffix of the lua API.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["api_version_string"] = {
["__text"] = [==[The version description of the lua API. This is a string compatible with the semantic versioning convention]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["check_version"] = {
["__text"] = [==[Check that a module is compatible with the running version of darktable
Add the following line at the top of your module : <code>darktable.configuration.check(...,{M,m,p},{M2,m2,p2})</code>To document that your module has been tested with API version M.m.p and M2.m2.p2.
This will raise an error if the user is running a released version of DT and a warning if he is running a development version
(the ... here will automatically expand to your module name if used at the top of your script]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the module to report on error]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[Tables of API versions that are known to work with the script]==],
["__attributes"] = {
["reported_type"] = [==[table...]==],
},
},
},
},
},
},
["preferences"] = {
["__text"] = [==[Lua allows you to manipulate preferences. Lua has its own namespace for preferences and you can't access nor write normal darktable preferences.
Preference handling functions take a _script_ parameter. This is a string used to avoid name collision in preferences (i.e namespace). Set it to something unique, usually the name of the script handling the preference.
Preference handling functions can't guess the type of a parameter. You must pass the type of the preference you are handling. 
Note that the directory, enum and file type preferences are stored internally as string. The user can only select valid values, but a lua script can set it to any string]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
["register"] = {
["__text"] = [==[Creates a new preference entry in the Lua tab of the preference screen. If this function is not called the preference can't be set by the user (you can still read and write invisible preferences).]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[Invisible prefix to guarantee unicity of preferences.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[A unique name used with the script part to identify the preference.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The type of the preference - one of the string values described above.]==],
["__attributes"] = {
["reported_type"] = {
["__text"] = [==[The type of value to save in a preference]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[string]==],
["2"] = [==[bool]==],
["3"] = [==[integer]==],
["4"] = [==[float]==],
["5"] = [==[file]==],
["6"] = [==[directory]==],
["7"] = [==[enum]==],
},
},
},
},
},
["4"] = {
["__text"] = [==[The label displayed in the preference screen.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["5"] = {
["__text"] = [==[The tooltip to display in the preference menu.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["6"] = {
["__text"] = [==[Default value to use when not set explicitly or by the user.
For the enum type of pref, this is mandatory]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[depends on type]==],
},
},
["7"] = {
["__text"] = [==[Minimum value (integer and float preferences only).]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[int or float]==],
},
},
["8"] = {
["__text"] = [==[Maximum value (integer and float preferences only).]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[int or float]==],
},
},
["9"] = {
["__text"] = [==[Step of the spinner (float preferences only).]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[float]==],
},
},
["10"] = {
["__text"] = [==[Other allowed values (enum preferences only)]==],
["__attributes"] = {
["reported_type"] = [==[string...]==],
},
},
},
},
},
["read"] = {
["__text"] = [==[Reads a value from a Lua preference.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The value of the preference.]==],
["__attributes"] = {
["reported_type"] = [==[depends on type]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[Invisible prefix to guarantee unicity of preferences.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The name of the preference displayed in the preference screen.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The type of the preference.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
},
},
},
},
},
["write"] = {
["__text"] = [==[Writes a value to a Lua preference.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[Invisible prefix to guarantee unicity of preferences.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The name of the preference displayed in the preference screen.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The type of the preference.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
},
},
["4"] = {
["__text"] = [==[The value to set the preference to.]==],
["__attributes"] = {
["reported_type"] = [==[depends on type]==],
},
},
},
},
},
},
["styles"] = {
["__text"] = [==[This pseudo table allows you to access and manipulate styles.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["#"] = {
["__text"] = [==[Each existing style has a numeric index; you can iterate them using ipairs.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["create"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"]]=],
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["delete"]]=],
["duplicate"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"]]=],
["apply"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]]=],
["import"] = {
["__text"] = [==[Import a style from an external .dtstyle file]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The file to import]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["export"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"]]=],
},
["database"] = {
["__text"] = [==[Allows to access the database of images. Note that duplicate images (images with the same RAW but different XMP) will appear multiple times with different duplicate indexes. Also note that all images are here. This table is not influenced by any GUI filtering (collections, stars etc...).]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["#"] = {
["__text"] = [==[Each image in the database appears with a numerical index; you can interate them using ipairs.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["duplicate"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"]]=],
["import"] = {
["__text"] = [==[Imports new images into the database.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The created image if an image is imported or the toplevel film object if a film was imported.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The filename or directory to import images from.

NOTE: If the images are set to be imported recursively in preferences only the toplevel film is returned (the one whose path was given as a parameter).

NOTE2: If the parameter is a directory the call is non-blocking; the film object will not have the newly imported images yet. Use a post-import-film filtering on that film to react when images are actually imported.


]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["move_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]]=],
["copy_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]]=],
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]]=],
},
["collection"] = {
["__text"] = [==[Allows to access the currently worked on images, i.e the ones selected by the collection lib. Filtering (rating etc) does not change that collection.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["#"] = {
["__text"] = [==[Each image in the collection appears with a numerical index; you can interate them using ipairs.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
["control"] = {
["__text"] = [==[This table contain function to manipulate the control flow of lua programs. It provides ways to do background jobs and other related functions]==],
["__attributes"] = {
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [==[dt_singleton]==],
},
["ending"] = {
["__text"] = [==[TRUE when darktable is terminating
Use this variable to detect when you should finish long running jobs]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
},
},
["dispatch"] = {
["__text"] = [==[Runs a function in the background. This function will be run at a later point, after luarc has finished running. If you do a loop in such a function, please check darktable.control.ending in your loop to finish the function when DT exits]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The call to dispatch]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
},
},
["2"] = {
["__text"] = [==[extra parameters to pass to the function]==],
["__attributes"] = {
["reported_type"] = [==[anything]==],
},
},
},
},
},
},
["gettext"] = {
["__text"] = [==[This table contains functions related to translating lua scripts]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
["gettext"] = {
["__text"] = [==[Translate a string using the darktable textdomain]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The translated string]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The string to translate]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["dgettext"] = {
["__text"] = [==[Translate a string using the specified textdomain]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The translated string]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The domain to use for that translation]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The string to translate]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["ngettext"] = {
["__text"] = [==[Translate a string depending on the number of objects using the darktable textdomain]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The translated string]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The string to translate]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The string to translate in plural form]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The number of objetc]==],
["__attributes"] = {
["reported_type"] = [==[int]==],
},
},
},
},
},
["dngettext"] = {
["__text"] = [==[Translate a string depending on the number of objects using the specified textdomain]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[The translated string]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The domain to use for that translation]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The string to translate]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[The string to translate in plural form]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["4"] = {
["__text"] = [==[The number of objetc]==],
["__attributes"] = {
["reported_type"] = [==[int]==],
},
},
},
},
},
["bindtextdomain"] = {
["__text"] = [==[Tell gettext where to find the .mo file translating messages for a particular domain]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The domain to use for that translation]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The base directory to look for the file. The file should be placed in <em>dirname</em>/<em>locale name</em>/LC_MESSAGES/<em>domain</em>.mo]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
},
["debug"] = {
["__text"] = [==[This section must be activated separately by calling

require "darktable.debug"
]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
["dump"] = {
["__text"] = [==[This will return a string describing everything Lua knows about an object, used to know what an object is.

This function is recursion-safe and can be used to dump _G if needed.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[A string containing a text description of the object - can be very long.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The object to dump.]==],
["__attributes"] = {
["reported_type"] = [==[anything]==],
},
},
["2"] = {
["__text"] = [==[A name to use for the object.]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[string]==],
},
},
["3"] = {
["__text"] = [==[A table of object,string pairs. Any object in that table will not be dumped, the string will be printed instead.
defaults to darktable.debug.known if not set]==],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [==[table]==],
},
},
},
},
},
["debug"] = {
["__text"] = [==[Initialized to false; set it to true to also dump information about metatables.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
["max_depth"] = {
["__text"] = [==[Initialized to 10; The maximum depth to recursively dump content.]==],
["__attributes"] = {
["reported_type"] = [==[number]==],
},
},
["known"] = {
["__text"] = [==[A table containing the default value of darktable.debug.dump.known]==],
["__attributes"] = {
["reported_type"] = [==[table]==],
},
},
["type"] = {
["__text"] = [==[Similar to the system function type() but it will return the real type instead of "userdata" for darktable specific objects.]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[A string describing the type of the object.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The object whos type must be reported.]==],
["__attributes"] = {
["reported_type"] = [==[anything]==],
},
},
},
},
},
},
},
["types"] = {
["__text"] = [==[This section documents types that are specific to darktable's Lua API.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["dt_lua_image_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_imageio_module_format_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_imageio_module_format_data_png"] = {
["__text"] = [==[Type object describing parameters to export to png.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["bpp"] = {
["__text"] = [==[The bpp parameter to use when exporting.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_tiff"] = {
["__text"] = [==[Type object describing parameters to export to tiff.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["bpp"] = {
["__text"] = [==[The bpp parameter to use when exporting.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_exr"] = {
["__text"] = [==[Type object describing parameters to export to exr.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["compression"] = {
["__text"] = [==[The compression parameter to use when exporting.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_copy"] = {
["__text"] = [==[Type object describing parameters to export to copy.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_format_data_pfm"] = {
["__text"] = [==[Type object describing parameters to export to pfm.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_format_data_jpeg"] = {
["__text"] = [==[Type object describing parameters to export to jpeg.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["quality"] = {
["__text"] = [==[The quality to use at export time.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_ppm"] = {
["__text"] = [==[Type object describing parameters to export to ppm.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_format_data_webp"] = {
["__text"] = [==[Type object describing parameters to export to webp.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["quality"] = {
["__text"] = [==[The quality to use at export time.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["comp_type"] = {
["__text"] = [==[The overall quality to use; can be one of "webp_lossy" or "webp_lossless".]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[Type of compression for webp]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[webp_lossy]==],
["2"] = [==[webp_lossless]==],
},
},
},
["write"] = true,
},
},
["hint"] = {
["__text"] = [==[A hint on the overall content of the image.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[a hint on the way to encode a webp image]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[hint_default]==],
["2"] = [==[hint_picture]==],
["3"] = [==[hint_photo]==],
["4"] = [==[hint_graphic]==],
},
},
},
["write"] = true,
},
},
},
["dt_imageio_module_format_data_j2k"] = {
["__text"] = [==[Type object describing parameters to export to jpeg2000.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["quality"] = {
["__text"] = [==[The quality to use at export time.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["bpp"] = {
["__text"] = [==[The bpp parameter to use when exporting.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["format"] = {
["__text"] = [==[The format to use.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[J2K format type]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[j2k]==],
["2"] = [==[jp2]==],
},
},
},
["write"] = true,
},
},
["preset"] = {
["__text"] = [==[The preset to use.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[J2K preset type]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[off]==],
["2"] = [==[cinema2k_24]==],
["3"] = [==[cinema2k_48]==],
["4"] = [==[cinema4k_24]==],
},
},
},
["write"] = true,
},
},
},
["dt_imageio_module_format_data_pdf"] = {
["__text"] = [==[Type object describing parameters to export to pdf.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["dpi"] = {
["__text"] = [==[The dot per inch value to use at export]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["icc"] = {
["__text"] = [==[Should the images be tagged with their embedded profile]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["border"] = {
["__text"] = [==[Empty space around the PDF images]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["orientation"] = {
["__text"] = [==[Orientation of the pages in the document]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["title"] = {
["__text"] = [==[The title for the document
  types.dt_imageio_module_format_data_pdf.rotate:set_text([[Should the images be rotated to match the PDF orientation]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["mode"] = {
["__text"] = [==[The image mode to use at export time]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["size"] = {
["__text"] = [==[The paper size to use]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["compression"] = {
["__text"] = [==[Compression mode to use for images]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["pages"] = {
["__text"] = [==[The page type to use]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["rotate"] = {
["__text"] = [==[Should the images be rotated in the resulting PDF]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
},
["_pdf_mode_t"] = {
["__text"] = [==[The export mode to use for PDF document]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[normal]==],
["2"] = [==[draft]==],
["3"] = [==[debug]==],
},
},
},
["_pdf_pages_t"] = {
["__text"] = [==[The different page types for PDF export]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[all]==],
["2"] = [==[single]==],
["3"] = [==[contact]==],
},
},
},
["dt_pdf_stream_encoder_t"] = {
["__text"] = [==[The compression mode for PDF document]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[uncompressed]==],
["2"] = [==[deflate]==],
},
},
},
["dt_imageio_module_storage_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["dt_imageio_module_storage_data_email"] = {
["__text"] = [==[An object containing parameters to export to email.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_storage_data_flickr"] = {
["__text"] = [==[An object containing parameters to export to flickr.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_storage_data_facebook"] = {
["__text"] = [==[An object containing parameters to export to facebook.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_storage_data_latex"] = {
["__text"] = [==[An object containing parameters to export to latex.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["filename"] = {
["__text"] = [==[The filename to export to.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["title"] = {
["__text"] = [==[The title to use for export.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["dt_imageio_module_storage_data_picasa"] = {
["__text"] = [==[An object containing parameters to export to picasa.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
},
["dt_imageio_module_storage_data_gallery"] = {
["__text"] = [==[An object containing parameters to export to gallery.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["filename"] = {
["__text"] = [==[The filename to export to.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["title"] = {
["__text"] = [==[The title to use for export.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["dt_imageio_module_storage_data_disk"] = {
["__text"] = [==[An object containing parameters to export to disk.]==],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["filename"] = {
["__text"] = [==[The filename to export to.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["dt_lua_film_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_style_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
["dt_style_item_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["#"].__attributes["reported_type"]]=],
["dt_lua_tag_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["dt_lua_lib_t"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["dt_lua_view_t"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_lua_backgroundjob_t"] = {} --[=[API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]]=],
["dt_lua_snapshot_t"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]]=],
["hint_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_webp"]["hint"].__attributes["reported_type"]]=],
["dt_ui_container_t"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]["container"].__attributes["reported_type"]]=],
["snapshot_direction_t"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["direction"].__attributes["reported_type"]]=],
["dt_imageio_j2k_format_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_j2k"]["format"].__attributes["reported_type"]]=],
["dt_imageio_j2k_preset_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_j2k"]["preset"].__attributes["reported_type"]]=],
["yield_type"] = {
["__text"] = [==[What type of event to wait for]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[WAIT_MS]==],
["2"] = [==[FILE_READABLE]==],
["3"] = [==[RUN_COMMAND]==],
},
},
},
["comp_type_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_webp"]["comp_type"].__attributes["reported_type"]]=],
["lua_pref_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
["dt_imageio_exr_compression_t"] = {
["__text"] = [==[The type of compression to use for the EXR image]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[off]==],
["2"] = [==[rle]==],
["3"] = [==[zips]==],
["4"] = [==[zip]==],
["5"] = [==[piz]==],
["6"] = [==[pxr24]==],
["7"] = [==[b44]==],
["8"] = [==[b44a]==],
},
},
},
["dt_lib_collect_params_rule_t"] = {
["__text"] = [==[A single rule for filtering a collection]==],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [==[dt_type]==],
},
["mode"] = {
["__text"] = [==[How this rule is applied after the previous one. Unused for the first rule]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[The logical operators to apply between rules]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[DT_LIB_COLLECT_MODE_AND]==],
["2"] = [==[DT_LIB_COLLECT_MODE_OR]==],
["3"] = [==[DT_LIB_COLLECT_MODE_AND_NOT]==],
},
},
},
["write"] = true,
},
},
["data"] = {
["__text"] = [==[The text segment of the rule. Exact content depends on the type of rule]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["item"] = {
["__text"] = [==[The item on which this rule filter. i.e the type of the rule]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [==[The different elements on which a collection can be filtered]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[DT_COLLECTION_PROP_FILMROLL]==],
["2"] = [==[DT_COLLECTION_PROP_FOLDERS]==],
["3"] = [==[DT_COLLECTION_PROP_CAMERA]==],
["4"] = [==[DT_COLLECTION_PROP_TAG]==],
["5"] = [==[DT_COLLECTION_PROP_DAY]==],
["6"] = [==[DT_COLLECTION_PROP_TIME]==],
["7"] = [==[DT_COLLECTION_PROP_HISTORY]==],
["8"] = [==[DT_COLLECTION_PROP_COLORLABEL]==],
["9"] = [==[DT_COLLECTION_PROP_TITLE]==],
["10"] = [==[DT_COLLECTION_PROP_DESCRIPTION]==],
["11"] = [==[DT_COLLECTION_PROP_CREATOR]==],
["12"] = [==[DT_COLLECTION_PROP_PUBLISHER]==],
["13"] = [==[DT_COLLECTION_PROP_RIGHTS]==],
["14"] = [==[DT_COLLECTION_PROP_LENS]==],
["15"] = [==[DT_COLLECTION_PROP_FOCAL_LENGTH]==],
["16"] = [==[DT_COLLECTION_PROP_ISO]==],
["17"] = [==[DT_COLLECTION_PROP_APERTURE]==],
["18"] = [==[DT_COLLECTION_PROP_FILENAME]==],
["19"] = [==[DT_COLLECTION_PROP_GEOTAGGING]==],
},
},
},
["write"] = true,
},
},
},
["dt_lib_collect_mode_t"] = {} --[=[API["types"]["dt_lib_collect_params_rule_t"]["mode"].__attributes["reported_type"]]=],
["dt_collection_properties_t"] = {} --[=[API["types"]["dt_lib_collect_params_rule_t"]["item"].__attributes["reported_type"]]=],
["dt_lua_orientation_t"] = {
["__text"] = [==[A possible orientation for a widget]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[horizontal]==],
["2"] = [==[vertical]==],
},
},
},
["dt_lua_align_t"] = {
["__text"] = [==[The alignment of a label]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[fill]==],
["2"] = [==[start]==],
["3"] = [==[end]==],
["4"] = [==[center]==],
["5"] = [==[baseline]==],
},
},
},
["dt_lua_ellipsize_mode_t"] = {
["__text"] = [==[The ellipsize mode of a label]==],
["__attributes"] = {
["reported_type"] = [==[enum]==],
["values"] = {
["1"] = [==[none]==],
["2"] = [==[start]==],
["3"] = [==[middle]==],
["4"] = [==[end]==],
},
},
},
["dt_lua_cairo_t"] = {} --[=[API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["lua_widget"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["lua_container"] = {
["__text"] = [==[A widget containing other widgets]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["#"] = {
["__text"] = [==[The widgets contained by the box
You can append widgets by adding them at the end of the list
You can remove widgets by setting them to nil]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["lua_check_button"] = {
["__text"] = [==[A checkable button with a label next to it]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["label"] = {
["__text"] = [==[The label displayed next to the button]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["value"] = {
["__text"] = [==[If the widget is checked or not]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["clicked_callback"] = {
["__text"] = [==[A function to call on button click]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget that triggered the callback]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["write"] = true,
},
},
},
["lua_label"] = {
["__text"] = [==[A label containing some text]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["label"] = {
["__text"] = [==[The label displayed]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["selectable"] = {
["__text"] = [==[True if the label content should be selectable]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["halign"] = {
["__text"] = [==[The horizontal alignment of the label]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["types"]["dt_lua_align_t"]]=],
["write"] = true,
},
},
["ellipsize"] = {
["__text"] = [==[The ellipsize mode of the label]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["types"]["dt_lua_ellipsize_mode_t"]]=],
["write"] = true,
},
},
},
["lua_button"] = {
["__text"] = [==[A clickable button]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["label"] = {
["__text"] = [==[The label displayed on the button]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["clicked_callback"] = {
["__text"] = [==[A function to call on button click]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget that triggered the callback]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["write"] = true,
},
},
},
["lua_box"] = {
["__text"] = [==[A container for widget in a horizontal or vertical list]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["types"]["lua_container"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["orientation"] = {
["__text"] = [==[The orientation of the box.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["types"]["dt_lua_orientation_t"]]=],
["write"] = true,
},
},
},
["lua_entry"] = {
["__text"] = [==[A widget in which the user can input text]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["text"] = {
["__text"] = [==[The content of the entry]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["placeholder"] = {
["__text"] = [==[The text to display when the entry is empty]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["is_password"] = {
["__text"] = [==[True if the text content should be hidden]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["editable"] = {
["__text"] = [==[False if the entry should be read-only]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
},
["lua_separator"] = {
["__text"] = [==[A widget providing a separation in the UI.]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["orientation"] = {
["__text"] = [==[The orientation of the separator.]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["lua_combobox"] = {
["__text"] = [==[A widget with multiple text entries in a menu
This widget can be set as editable at construction time.
If it is editable the user can type a value and is not constrained by the values in the menu]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["value"] = {
["__text"] = [==[The text content of the selected entry, can be nil
You can set it to a number to select the corresponding entry from the menu
If the combo box is editable, you can set it to any string
You can set it to nil to deselect all entries]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["#"] = {
["__text"] = [==[The various menu entries.
You can add new entries by writing to the first element beyond the end
You can removes entries by setting them to nil]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
},
},
["changed_callback"] = {
["__text"] = [==[A function to call when the value field changes (character entered or value selected)]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget that triggered the callback]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["write"] = true,
},
},
["editable"] = {
["__text"] = [==[True is the user is allowed to type a string in the combobox]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
["label"] = {
["__text"] = [==[The label displayed on the combobox]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
["lua_file_chooser_button"] = {
["__text"] = [==[A button that allows the user to select an existing file]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["title"] = {
["__text"] = [==[The title of the window when choosing a file]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["value"] = {
["__text"] = [==[The currently selected file]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
["changed_callback"] = {
["__text"] = [==[A function to call when the value field changes (character entered or value selected)]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The widget that triggered the callback]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
},
},
},
["write"] = true,
},
},
["is_directory"] = {
["__text"] = [==[True if the file chooser button only allows directories to be selecte]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[boolean]==],
["write"] = true,
},
},
},
["lua_stack"] = {
["__text"] = [==[A container that will only show one of its child at a time]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["types"]["lua_container"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["active"] = {
["__text"] = [==[The currently selected child, can be nil if the container has no child, can be set to one of the child widget or to an index in the child table]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[types.lua_widget or nil]==],
["write"] = true,
},
},
},
["lua_slider"] = {
["__text"] = [==[A slider that can be set by the user]==],
["__attributes"] = {
["has_ipairs"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]]=],
["reported_type"] = [==[dt_type]==],
},
["__call"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]]=],
["soft_min"] = {
["__text"] = [==[The soft minimum value for the slider, the slider can't go beyond this point]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["soft_max"] = {
["__text"] = [==[The soft maximum value for the slider, the slider can't go beyond this point]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["hard_min"] = {
["__text"] = [==[The hard minimum value for the slider, the user can't manually enter a value beyond this point]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["hard_max"] = {
["__text"] = [==[The hard maximum value for the slider, the user can't manually enter a value beyond this point]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["value"] = {
["__text"] = [==[The current value of the slider]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[number]==],
["write"] = true,
},
},
["label"] = {
["__text"] = [==[The label next to the slider]==],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [==[string]==],
["write"] = true,
},
},
},
},
["events"] = {
["__text"] = [==[This section documents events that can be used to trigger Lua callbacks.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["intermediate-export-image"] = {
["__text"] = [==[This event is called each time an image is exported, once for each image after the image has been processed to an image format but before the storage has moved the image to its final destination.]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The image object that has been exported.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [==[The name of the file that is the result of the image being processed.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["4"] = {
["__text"] = [==[The format used to export the image.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["5"] = {
["__text"] = [==[The storage used to export the image (can be nil).]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["post-import-image"] = {
["__text"] = [==[This event is triggered whenever a new image is imported into the database.

	This event can be registered multiple times, all callbacks will be called.]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The image object that has been exported.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["shortcut"] = {
["__text"] = [==[This event registers a new keyboard shortcut. The shortcut isn't bound to any key until the users does so in the preference panel.

	The event is triggered whenever the shortcut is triggered.


	This event can only be registered once per value of shortcut.
	]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The tooltip string that was given at registration time.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
["signature"] = {
["1"] = {
["__text"] = [==[The string that will be displayed on the shortcut preference panel describing the shortcut.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
},
},
},
},
["post-import-film"] = {
["__text"] = [==[This event is triggered when an film import is finished (all post-import-image callbacks have already been triggered). This event can be registered multiple times.
	]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The new film that has been added. If multiple films were added recursively only the top level film is reported.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["view-changed"] = {
["__text"] = [==[This event is triggered after the user changed the active view]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The view that we just left]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [==[The view we are now in]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["global_toolbox-grouping_toggle"] = {
["__text"] = [==[This event is triggered after the user toggled the grouping button.]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[the new grouping status.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["global_toolbox-overlay_toggle"] = {
["__text"] = [==[This event is triggered after the user toggled the overlay button.]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[the new overlay status.]==],
["__attributes"] = {
["reported_type"] = [==[boolean]==],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["mouse-over-image-changed"] = {
["__text"] = [==[This event is triggered whenever the image under the mouse changes]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The new image under the mous, can be nil if there is no image under the mouse]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["exit"] = {
["__text"] = [==[This event is triggered when darktable exits, it allows lua scripts to do cleanup jobs]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["pre-import"] = {
["__text"] = [==[This event is trigger before any import action]==],
["__attributes"] = {
["reported_type"] = [==[event]==],
},
["callback"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["signature"] = {
["1"] = {
["__text"] = [==[The name of the event that triggered the callback.]==],
["__attributes"] = {
["reported_type"] = [==[string]==],
},
},
["2"] = {
["__text"] = [==[The files that will be imported. Modifying this table will change the list of files that will be imported"]==],
["__attributes"] = {
["reported_type"] = [==[table of string]==],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [==[This event has no extra registration parameters.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
},
["attributes"] = {
["__text"] = [==[This section documents various attributes used throughout the documentation.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["write"] = {
["__text"] = [==[This object is a variable that can be written to.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
["has_tostring"] = {
["__text"] = [==[This object has a specific reimplementation of the "tostring" method that allows pretty-printing it.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
["implicit_yield"] = {
["__text"] = [==[This call will release the Lua lock while executing, thus allowing other Lua callbacks to run.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
["parent"] = {
["__text"] = [==[This object inherits some methods from another object. You can call the methods from the parent on the child object]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
},
},
["system"] = {
["__text"] = [==[This section documents changes to system functions.]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["coroutine"] = {
["__text"] = [==[]==],
["__attributes"] = {
["reported_type"] = [==[documentation node]==],
},
["yield"] = {
["__text"] = [==[Lua functions can yield at any point. The parameters and return types depend on why we want to yield.
A callback that is yielding allows other Lua code to run.

* WAIT_MS: one extra parameter; the execution will pause for that many milliseconds; yield returns nothing;
* FILE_READABLE: an opened file from a call to the OS library; will return when the file is readable; returns nothing;
* RUN_COMMAND: a command to be run by "sh -c"; will return when the command terminates; returns the return code of the execution.
]==],
["__attributes"] = {
["reported_type"] = [==[function]==],
["ret_val"] = {
["__text"] = [==[Nothing for "WAIT_MS" and "FILE_READABLE"; the returned code of the command for "RUN_COMMAND".]==],
["__attributes"] = {
["reported_type"] = [==[variable]==],
},
},
["signature"] = {
["1"] = {
["__text"] = [==[The type of yield.]==],
["__attributes"] = {
["reported_type"] = {} --[=[API["types"]["yield_type"]]=],
},
},
["2"] = {
["__text"] = [==[An extra parameter: integer for "WAIT_MS", open file for "FILE_READABLE", string for "RUN_COMMAND".]==],
["__attributes"] = {
["reported_type"] = [==[variable]==],
},
},
},
},
},
},
},
}
API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_lib"].__attributes["signature"]["8"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_lib"].__attributes["signature"]["8"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["current_view"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["map"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["darkroom"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["lighttable"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["tethering"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["slideshow"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["print"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_lua_view_t"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["view-changed"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["view-changed"]["callback"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["5"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["6"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["new_format"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_png"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_tiff"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_exr"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_copy"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_pfm"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_jpeg"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_ppm"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_webp"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_j2k"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_imageio_module_format_data_pdf"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["intermediate-export-image"]["callback"].__attributes["signature"]["4"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["tags"]["get_tags"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["get_tags"]
API["darktable"]["database"]["duplicate"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"]
API["darktable"]["tags"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["delete"]
API["system"]["coroutine"]["yield"].__attributes["signature"]["1"].__attributes["reported_type"] = API["types"]["yield_type"]
API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]["select"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["snapshots"]["selected"].__attributes["reported_type"] = API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]
API["types"]["dt_lua_snapshot_t"] = API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]
API["darktable"]["gui"]["create_job"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]
API["types"]["dt_lua_backgroundjob_t"] = API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["film"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["films"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["films"]["new"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_lua_film_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["post-import-film"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["preferences"]["read"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["darktable"]["preferences"]["write"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["types"]["lua_pref_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["types"]["lua_label"]["ellipsize"].__attributes["reported_type"] = API["types"]["dt_lua_ellipsize_mode_t"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["save"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["restore"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["move_to"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["line_to"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["rectangle"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["arc"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["arc_negative"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["rotate"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["scale"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["translate"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["new_sub_path"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]["draw_line"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_lua_cairo_t"] = API["darktable"]["guides"]["register_guide"].__attributes["signature"]["2"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["lua_box"].__attributes["parent"] = API["types"]["lua_container"]
API["types"]["lua_stack"].__attributes["parent"] = API["types"]["lua_container"]
API["types"]["lua_label"]["halign"].__attributes["reported_type"] = API["types"]["dt_lua_align_t"]
API["types"]["lua_box"]["orientation"].__attributes["reported_type"] = API["types"]["dt_lua_orientation_t"]
API["types"]["dt_collection_properties_t"] = API["types"]["dt_lib_collect_params_rule_t"]["item"].__attributes["reported_type"]
API["types"]["dt_lib_collect_mode_t"] = API["types"]["dt_lib_collect_params_rule_t"]["mode"].__attributes["reported_type"]
API["types"]["lua_container"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_check_button"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_label"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_button"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_box"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_entry"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_separator"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_combobox"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_file_chooser_button"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_stack"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["lua_slider"]["__call"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"]
API["types"]["dt_imageio_j2k_preset_t"] = API["types"]["dt_imageio_module_format_data_j2k"]["preset"].__attributes["reported_type"]
API["types"]["dt_imageio_j2k_format_t"] = API["types"]["dt_imageio_module_format_data_j2k"]["format"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["attach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]
API["darktable"]["tags"]["attach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]
API["darktable"]["styles"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["delete"]
API["types"]["comp_type_t"] = API["types"]["dt_imageio_module_format_data_webp"]["comp_type"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["styles"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["types"]["dt_style_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["copy"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]
API["darktable"]["database"]["copy_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]
API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["reset_callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]["__call"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["darktable"]["register_lib"].__attributes["signature"]["6"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["darktable"]["new_widget"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["import"]["register_widget"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_widget"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_container"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_container"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_check_button"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_check_button"]["clicked_callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_label"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_button"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_button"]["clicked_callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_entry"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_separator"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_combobox"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_combobox"]["changed_callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_file_chooser_button"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_file_chooser_button"]["changed_callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["types"]["lua_slider"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["7"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["4"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["5"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["6"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["new_storage"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_email"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_flickr"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_facebook"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_latex"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_picasa"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_gallery"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_module_storage_data_disk"].__attributes["parent"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["events"]["intermediate-export-image"]["callback"].__attributes["signature"]["5"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]["reset"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_lib"].__attributes["signature"]["8"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["collect"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["import"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["styles"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["metadata_view"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["metadata"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["hinter"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["modulelist"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["filmstrip"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["viewswitcher"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["darktable_label"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["tagging"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["geotagging"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["recentcollect"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["global_toolbox"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["filter"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["ratings"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["select"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["colorlabels"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["lighttable_mode"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["copy_history"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["image"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["modulegroups"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["module_toolbox"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["session"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["histogram"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["export"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["history"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["colorpicker"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["navigation"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["masks"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["view_toolbox"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["live_view"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["map_settings"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["camera"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["location"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["backgroundjobs"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["print_settings"].__attributes["parent"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_lua_lib_t"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["styles"]["export"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"]
API["types"]["hint_t"] = API["types"]["dt_imageio_module_format_data_webp"]["hint"].__attributes["reported_type"]
API["darktable"]["database"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]
API["darktable"]["films"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["blue"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["green"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["yellow"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["purple"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["tags"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["tags"]["find"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_lua_tag_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_style_item_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["#"].__attributes["reported_type"]
API["types"]["dt_ui_container_t"] = API["darktable"]["register_lib"].__attributes["signature"]["7"].__attributes["signature"]["1"].__attributes["reported_type"]["container"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["detach_tag"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]
API["darktable"]["tags"]["detach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["apply_style"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]
API["darktable"]["styles"]["apply"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]
API["darktable"]["styles"]["duplicate"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["get_tags"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["reset"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["group_with"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["group_with"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["make_group_leader"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["get_group_members"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["group_leader"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["drop_cache"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["hovered"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["gui"]["create_job"].__attributes["signature"]["3"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["database"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["database"]["import"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["collection"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_lua_image_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["intermediate-export-image"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["post-import-image"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["mouse-over-image-changed"]["callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["styles"]["create"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"]
API["types"]["snapshot_direction_t"] = API["darktable"]["gui"]["libs"]["snapshots"]["direction"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["move_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]
API["darktable"]["database"]["move_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]
return API
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
