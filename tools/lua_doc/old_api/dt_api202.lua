API = {
["__text"] = [[This documentation is for the *development* version of darktable. for the stable version, please visit the user manual
To access the darktable specific functions you must load the darktable environment:<code>darktable = require "darktable"</code>All functions and data are accessed through the darktable module.
This documentation for API version 2.0.2.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["darktable"] = {
["__text"] = [[The darktable library is the main entry point for all access to the darktable internals.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["print"] = {
["__text"] = [[Will print a string to the darktable control log (the long overlaid window that appears over the main panel).]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The string to display which should be a single line.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["print_error"] = {
["__text"] = [[This function will print its parameter if the Lua logdomain is activated. Start darktable with the "-d lua" command line option to enable the Lua logdomain.]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The string to display.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["register_event"] = {
["__text"] = [[This function registers a callback to be called when a given event happens.
Events are documented in the event section.]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the event to register to.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The function to call on event. The signature of the function depends on the type of event.]],
["__attributes"] = {
["reported_type"] = [[function]],
},
},
["3"] = {
["__text"] = [[Some events need extra parameters at registration time; these must be specified here.]],
["__attributes"] = {
["reported_type"] = [[variable]],
},
},
},
},
},
["register_storage"] = {
["__text"] = [[This function will add a new storage implemented in Lua.
A storage is a module that is responsible for handling images once they have been generated during export. Examples of core storages include filesystem, e-mail, facebook...]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[A Unique name for the plugin.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[A human readable name for the plugin.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[This function is called once for each exported image. Images can be exported in parallel but the calls to this function will be serialized.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The storage object used for the export.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A virtual type representing all storage types.]],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [[dt_type]],
},
["plugin_name"] = {
["__text"] = [[A unique name for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["name"] = {
["__text"] = [[A human readable name for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["width"] = {
["__text"] = [[The currently selected width for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["height"] = {
["__text"] = [[The currently selected height for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["recommended_width"] = {
["__text"] = [[The recommended width for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["recommended_height"] = {
["__text"] = [[The recommended height for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["supports_format"] = {
["__text"] = [[Checks if a format is supported by this storage.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[True if the format is supported by the storage.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The storage type to check against.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The format type to check.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A virtual type representing all format types.]],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [[dt_type]],
},
["plugin_name"] = {
["__text"] = [[A unique name for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["name"] = {
["__text"] = [[A human readable name for the plugin.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["extension"] = {
["__text"] = [[The typical filename extension for that format.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["mime"] = {
["__text"] = [[The mime type associated with the format.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["max_width"] = {
["__text"] = [[The max width allowed for the format (0 = unlimited).]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["max_height"] = {
["__text"] = [[The max height allowed for the format (0 = unlimited).]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["write_image"] = {
["__text"] = [[Exports an image to a file. This is a blocking operation that will not return until the image is exported.]],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[Returns true on success.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The format that will be used to export.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The image object to export.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[Image objects represent an image in the database. This is slightly different from a file on disk since a file can have multiple developments.

	Note that this is the real image object; changing the value of a field will immediately change it in darktable and will be reflected on any copy of that image object you may have kept.]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["attach_tag"] = {
["__text"] = [[Attach a tag to an image; the order of the parameters can be reversed.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The tag to be attached.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A tag that can be attached to an image.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["delete"] = {
["__text"] = [[Deletes the tag object, detaching it from all images.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The tag to be deleted.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["attach"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]]=],
["detach"] = {
["__text"] = [[Detach a tag from an image; the order of the parameters can be reversed.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The tag to be detached.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The image to detach the tag from.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["name"] = {
["__text"] = [[The name of the tag.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["#"] = {
["__text"] = [[The images that have that tag attached to them.]],
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
["__text"] = [[The image to attach the tag to.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["detach_tag"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]]=],
["get_tags"] = {
["__text"] = [[Gets all tags attached to an image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[A table of tags that are attached to the image.]],
["__attributes"] = {
["reported_type"] = [[table of types.dt_lua_tag_t]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The image to get the tags from.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["create_style"] = {
["__text"] = [[Create a new style based on an image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The new style object.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A style that can be applied to an image.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["delete"] = {
["__text"] = [[Deletes an existing style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[the style to delete]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
},
},
},
["duplicate"] = {
["__text"] = [[Create a new style based on an existing style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The new style object.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The style to base the new style on.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The new style's name.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[The new style's description.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["apply"] = {
["__text"] = [[Apply a style to an image. The order of parameters can be inverted.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The style to use.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The image to apply the style to.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["export"] = {
["__text"] = [[Export a style to an external .dtstyle file]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The style to export]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The directory to export to]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[Is overwriting an existing file allowed]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
},
},
},
["name"] = {
["__text"] = [[The name of the style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["description"] = {
["__text"] = [[The description of the style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["#"] = {
["__text"] = [[The different items that make the style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[An element that is part of a style.]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["name"] = {
["__text"] = [[The name of the style item.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["num"] = {
["__text"] = [[The position of the style item within its style.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
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
["__text"] = [[The image to create the style from.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The name to give to the new style.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[The description of the new style.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["apply_style"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]]=],
["duplicate"] = {
["__text"] = [[Creates a duplicate of an image and returns it.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The created image if an image is imported or the toplevel film object if a film was imported.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[the image to duplicate]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["move"] = {
["__text"] = [[Physically moves an image (and all its duplicates) to another film.
This will move the image file, the related XMP and all XMP for the duplicates to the directory of the new film
Note that the parameter order is not relevant.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image to move]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The film to move to]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A film in darktable; this represents a directory containing imported images.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["move_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]]=],
["copy_image"] = {
["__text"] = [[Physically copies an image to another film.
This will copy the image file and the related XMP to the directory of the new film
If there is already a file with the same name as the image file, it will create a duplicate from that file instead
Note that the parameter order is not relevant.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The new image]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The image to copy]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The film to copy to]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["#"] = {
["__text"] = [[The different images within the film.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["id"] = {
["__text"] = [[A unique numeric id used by this film.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["path"] = {
["__text"] = [[The path represented by this film.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["delete"] = {
["__text"] = [[Removes the film from the database.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The film to remove.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[Force removal, even if the film is not empty.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[Boolean]],
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
["__text"] = [[A unique id identifying the image in the database.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
["path"] = {
["__text"] = [[The file the directory containing the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["film"] = {
["__text"] = [[The film object that contains this image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["filename"] = {
["__text"] = [[The filename of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["duplicate_index"] = {
["__text"] = [[If there are multiple images based on a same file, each will have a unique number, starting from 0.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
["publisher"] = {
["__text"] = [[The publisher field of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["title"] = {
["__text"] = [[The title field of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["creator"] = {
["__text"] = [[The creator field of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["rights"] = {
["__text"] = [[The rights field of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["description"] = {
["__text"] = [[The description field for the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["exif_maker"] = {
["__text"] = [[The maker exif data.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["exif_model"] = {
["__text"] = [[The camera model used.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["exif_lens"] = {
["__text"] = [[The id string of the lens used.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["exif_aperture"] = {
["__text"] = [[The aperture saved in the exif data.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["exif_exposure"] = {
["__text"] = [[The exposure time of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["exif_focal_length"] = {
["__text"] = [[The focal length of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["exif_iso"] = {
["__text"] = [[The iso used on the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["exif_datetime_taken"] = {
["__text"] = [[The date and time of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["exif_focus_distance"] = {
["__text"] = [[The distance of the subject.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["exif_crop"] = {
["__text"] = [[The exif crop data.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["latitude"] = {
["__text"] = [[GPS latitude data of the image, nil if not set.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[documentation node]],
["write"] = true,
},
},
["longitude"] = {
["__text"] = [[GPS longitude data of the image, nil if not set.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[documentation node]],
["write"] = true,
},
},
["is_raw"] = {
["__text"] = [[True if the image is a RAW file.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
},
},
["is_ldr"] = {
["__text"] = [[True if the image is a ldr image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
},
},
["is_hdr"] = {
["__text"] = [[True if the image is a hdr image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
},
},
["width"] = {
["__text"] = [[The width of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
["height"] = {
["__text"] = [[The height of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
["rating"] = {
["__text"] = [[The rating of the image (-1 for rejected).]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["red"] = {
["__text"] = [[True if the image has the corresponding colorlabel.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
["blue"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["green"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["yellow"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["purple"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]]=],
["reset"] = {
["__text"] = [[Removes all processing from the image, resetting it back to its original state]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image whose history will be deleted]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["delete"] = {
["__text"] = [[Removes an image from the database]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image to remove]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["group_with"] = {
["__text"] = [[Puts the first image in the same group as the second image. If no second image is provided the image will be in its own group.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image whose group must be changed.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The image we want to group with.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["make_group_leader"] = {
["__text"] = [[Makes the image the leader of its group.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image we want as the leader.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["get_group_members"] = {
["__text"] = [[Returns a table containing all types.dt_lua_image_t of the group. The group leader is both at a numeric key and at the "leader" special key (so you probably want to use ipairs to iterate through that table).]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[A table of image objects containing all images that are in the same group as the image.]],
["__attributes"] = {
["reported_type"] = [[table of types.dt_lua_image_t]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The image whose group we are querying.]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["group_leader"] = {
["__text"] = [[The image which is the leader of the group this image is a member of.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["local_copy"] = {
["__text"] = [[True if the image has a copy in the local cache]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
["drop_cache"] = {
["__text"] = [[drops the cached version of this image.
This function should be called if an image is modified out of darktable to force DT to regenerate the thumbnail
Darktable will regenerate the thumbnail by itself when it is needed]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The image whose cache must be dropped.]],
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
["__text"] = [[The filename to export to.]],
["__attributes"] = {
["reported_type"] = [[string]],
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
["__text"] = [[The exported image object.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [[The format object used for the export.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["4"] = {
["__text"] = [[The name of a temporary file where the processed image is stored.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["5"] = {
["__text"] = [[The number of the image out of the export series.]],
["__attributes"] = {
["reported_type"] = [[integer]],
},
},
["6"] = {
["__text"] = [[The total number of images in the export series.]],
["__attributes"] = {
["reported_type"] = [[integer]],
},
},
["7"] = {
["__text"] = [[True if the export is high quality.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["8"] = {
["__text"] = [[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export serie.]],
["__attributes"] = {
["reported_type"] = [[table]],
},
},
},
},
},
["4"] = {
["__text"] = [[This function is called once all images are processed and all store calls are finished.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The storage object used for the export.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[A table keyed by the exported image objects and valued with the corresponding temporary export filename.]],
["__attributes"] = {
["reported_type"] = [[table]],
},
},
["3"] = {
["__text"] = [[An empty Lua table to store extra data. This table is common to all calls to store and the call to finalize in a given export series.]],
["__attributes"] = {
["reported_type"] = [[table]],
},
},
},
},
},
["5"] = {
["__text"] = [[A function called to check if a given image format is supported by the Lua storage; this is used to build the dropdown format list for the GUI.
Note that the parameters in the format are the ones currently set in the GUI; the user might change them before export.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[True if the corresponding format is supported.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The storage object tested.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The format object to report about.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["6"] = {
["__text"] = [[A function called before storage happens
This function can change the list of exported functions]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The modified table of images to export or nil
If nil (or nothing) is returned, the original list of images will be exported
If a table of images is returned, that table will be used instead. The table can be empty. The images parameter can be modified and returned]],
["__attributes"] = {
["reported_type"] = [[table or nil]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The storage object tested.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The format object to report about.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [[A table containing images to be exported.]],
["__attributes"] = {
["reported_type"] = [[table of types.dt_lua_image_t]],
},
},
["4"] = {
["__text"] = [[True if the export is high quality.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["5"] = {
["__text"] = [[An empty Lua table to take extra data. This table is common to the initialize, store and finalize calls in an export serie.]],
["__attributes"] = {
["reported_type"] = [[table]],
},
},
},
},
},
},
},
},
["films"] = {
["__text"] = [[A table containing all the film objects in the database.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [[dt_singleton]],
},
["#"] = {
["__text"] = [[Each film has a numeric entry in the database.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["new"] = {
["__text"] = [[Creates a new empty film
 see darktable.database.import to import a directory with all its images and to add images to a film]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The newly created film, or the existing film if the directory is already imported]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The directory that the new film will represent. The directory must exist]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]]=],
},
["new_format"] = {
["__text"] = [[Creates a new format object to export images]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The newly created object. Exact type depends on the type passed]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The type of format object to create, one of : 

* copy
* exr
* j2k
* jpeg
* pfm
* png
* ppm
* tiff
* webp
]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["new_storage"] = {
["__text"] = [[Creates a new storage object to export images]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The newly created object. Exact type depends on the type passed]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The type of storage object to create, one of : 

* disk
* email
* facebook
* flickr
* gallery
* latex
* picasa
(Other, lua-defined, storage types may appear.)]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["gui"] = {
["__text"] = [[This subtable contains function and data to manipulate the darktable user interface with Lua.
Most of these function won't do anything if the GUI is not enabled (i.e you are using the command line version darktabl-cli instead of darktable).]],
["__attributes"] = {
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [[dt_singleton]],
},
["action_images"] = {
["__text"] = [[A table of types.dt_lua_image_t on which the user expects UI actions to happen.
It is based on both the hovered image and the selection and is consistent with the way darktable works.
It is recommended to use this table to implement Lua actions rather than darktable.gui.hovered or darktable.gui.selection to be consistent with darktable's GUI.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[table]],
},
},
["hovered"] = {
["__text"] = [[The image under the cursor or nil if no image is hovered.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[documentation node]],
},
},
["selection"] = {
["__text"] = [[Allows to change the set of selected images.]],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[A table containing the selection as it was before the function was called.]],
["__attributes"] = {
["reported_type"] = [[table of types.dt_lua_image_t]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[A table of images which will define the selected images. If this parameter is not given the selection will be untouched. If an empty table is given the selection will be emptied.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[table of types.dt_lua_image_t]],
},
},
},
},
},
["current_view"] = {
["__text"] = [[Allows to change the current view.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[the current view]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A darktable view]],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [[dt_type]],
},
["id"] = {
["__text"] = [[A unique string identifying the view]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["name"] = {
["__text"] = [[The name of the view]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
},
},
},
["signature"] = {
["1"] = {
["__text"] = [[The view to switch to. If empty the current view is unchanged]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
},
},
},
["create_job"] = {
["__text"] = [[Create a new progress_bar displayed in darktable.gui.libs.backgroundjobs]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The newly created job object]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[A lua-managed entry in the backgroundjob lib]],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [[dt_type]],
},
["percent"] = {
["__text"] = [[The value of the progress bar, between 0 and 1. will return nil if there is no progress bar, will raise an error if read or written on an invalid job]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["valid"] = {
["__text"] = [[True if the job is displayed, set it to false to destroy the entry
An invalid job cannot be made valid again]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
},
},
},
["signature"] = {
["1"] = {
["__text"] = [[The text to display in the job entry]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[Should a progress bar be displayed]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[boolean]],
},
},
["3"] = {
["__text"] = [[A function called when the cancel button for that job is pressed
note that the job won't be destroyed automatically. You need to set types.dt_lua_backgroundjob_t.valid to false for that]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The job who is being cancelded]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
},
},
},
},
},
},
["views"] = {
["__text"] = [[The different views in darktable]],
["__attributes"] = {
["has_pairs"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
["map"] = {
["__text"] = [[The map view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
["latitude"] = {
["__text"] = [[The latitude of the center of the map]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["longitude"] = {
["__text"] = [[The longitude of the center of the map]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["zoom"] = {
["__text"] = [[The current zoom level of the map]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
},
["darkroom"] = {
["__text"] = [[The darkroom view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["lighttable"] = {
["__text"] = [[The lighttable view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["tethering"] = {
["__text"] = [[The tethering view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["slideshow"] = {
["__text"] = [[The slideshow view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
},
["libs"] = {
["__text"] = [[This table allows to reference all lib objects
lib are the graphical blocks within each view.
To quickly figure out what lib is what, you can use the following code which will make a given lib blink.
<code>local tested_module="global_toolbox"
dt.gui.libs[tested_module].visible=false
coroutine.yield("wait_ms",2000)
while true do
	dt.gui.libs[tested_module].visible = not dt.gui.libs[tested_module].visible
	coroutine.yield("wait_ms",2000)
end</code>]],
["__attributes"] = {
["has_pairs"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
["snapshots"] = {
["__text"] = [[The UI element that manipulates snapshots in darkroom]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {
["__text"] = [[The type of a UI lib]],
["__attributes"] = {
["has_pairs"] = true,
["reported_type"] = [[dt_type]],
},
["id"] = {
["__text"] = [[A unit string identifying the lib]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["name"] = {
["__text"] = [[The translated title of the UI element]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["version"] = {
["__text"] = [[The version of the internal data of this lib]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
["visible"] = {
["__text"] = [[Allow to make a lib module completely invisible to the user.
Note that if the module is invisible the user will have no way to restore it without lua]],
["__attributes"] = {
["implicit_yield"] = true,
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
["expandable"] = {
["__text"] = [[True if the lib can be expanded/retracted]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
},
},
["expanded"] = {
["__text"] = [[True if the lib is expanded]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
["reset"] = {
["__text"] = [[A function to reset the lib to its default values
This function will do nothing if the lib is not visible or can't be reset]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The lib to reset]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
},
},
},
},
},
["on_screen"] = {
["__text"] = [[True if the lib is currently visible on the screen]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
},
},
},
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
["ratio"] = {
["__text"] = [[The place in the screen where the line separating the snapshot is. Between 0 and 1]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["direction"] = {
["__text"] = [[The direction of the snapshot overlay]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[Which part of the main window is occupied by a snapshot]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[left]],
["2"] = [[right]],
["3"] = [[top]],
["4"] = [[bottom]],
},
},
},
["write"] = true,
},
},
["#"] = {
["__text"] = [[The different snapshots for the image]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[The description of a snapshot in the snapshot lib]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["reported_type"] = [[dt_type]],
},
["filename"] = {
["__text"] = [[The filename of an image containing the snapshot]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
["select"] = {
["__text"] = [[Activates this snapshot on the display. To deactivate all snapshot you need to call this function on the active snapshot]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The snapshot to activate]],
["__attributes"] = {
["is_self"] = true,
["reported_type"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]]=],
},
},
},
},
},
["name"] = {
["__text"] = [[The name of the snapshot, as seen in the UI]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
},
},
},
},
},
["selected"] = {
["__text"] = [[The currently selected snapshot]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[documentation node]],
},
},
["take_snapshot"] = {
["__text"] = [[Take a snapshot of the current image and add it to the UI
The snapshot file will be generated at the next redraw of the main window]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
},
},
},
["max_snapshot"] = {
["__text"] = [[The maximum number of snapshots]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
},
},
},
["styles"] = {
["__text"] = [[The style selection menu]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["metadata_view"] = {
["__text"] = [[The widget displaying metadata about the current image]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["metadata"] = {
["__text"] = [[The widget allowing modification of metadata fields on the current image]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["hinter"] = {
["__text"] = [[The small line of text at the top of the UI showing the number of selected images]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["modulelist"] = {
["__text"] = [[The window allowing to set modules as visible/hidden/favorite]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["filmstrip"] = {
["__text"] = [[The filmstrip at the bottom of some views]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["viewswitcher"] = {
["__text"] = [[The labels allowing to switch view]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["darktable_label"] = {
["__text"] = [[The darktable logo in the upper left corner]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["tagging"] = {
["__text"] = [[The tag manipulation UI]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["geotagging"] = {
["__text"] = [[The geotagging time synchronisation UI]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["recentcollect"] = {
["__text"] = [[The recent collection UI element]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["global_toolbox"] = {
["__text"] = [[The common tools to all view (settings, grouping...)]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
["grouping"] = {
["__text"] = [[The current status of the image grouping option]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
["show_overlays"] = {
["__text"] = [[the current status of the image overlays option]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[boolean]],
["write"] = true,
},
},
},
["filter"] = {
["__text"] = [[The image-filter menus at the top of the UI]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["import"] = {
["__text"] = [[The buttons to start importing images]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["ratings"] = {
["__text"] = [[The starts to set the rating of an image]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["select"] = {
["__text"] = [[The buttons that allow to quickly change the selection]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["collect"] = {
["__text"] = [[The collection UI element that allows to filter images by collection]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["colorlabels"] = {
["__text"] = [[The color buttons that allow to set labels on an image]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["lighttable_mode"] = {
["__text"] = [[The navigation and zoom level UI in lighttable]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["copy_history"] = {
["__text"] = [[The UI element that manipulates history]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["image"] = {
["__text"] = [[The UI element that manipulates the current image]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["modulegroups"] = {
["__text"] = [[The icons describing the different iop groups]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["module_toolbox"] = {
["__text"] = [[The tools on the bottom line of the UI (overexposure)]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["session"] = {
["__text"] = [[The session UI when tethering]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["histogram"] = {
["__text"] = [[The histogram widget]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["export"] = {
["__text"] = [[The export menu]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["history"] = {
["__text"] = [[The history manipulation menu]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["colorpicker"] = {
["__text"] = [[The colorpicker menu]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["navigation"] = {
["__text"] = [[The full image preview to allow navigation]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["masks"] = {
["__text"] = [[The masks window]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["view_toolbox"] = {
["__text"] = [[]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["live_view"] = {
["__text"] = [[The liveview window]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["map_settings"] = {
["__text"] = [[The map setting window]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["camera"] = {
["__text"] = [[The camera selection UI]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["location"] = {
["__text"] = [[The location ui]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
["backgroundjobs"] = {
["__text"] = [[The window displaying the currently running jobs]],
["__attributes"] = {
["has_pairs"] = true,
["has_tostring"] = true,
["is_attribute"] = true,
["is_singleton"] = true,
["parent"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["read"] = true,
["reported_type"] = [[dt_singleton]],
},
},
},
},
["tags"] = {
["__text"] = [[Allows access to all existing tags.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [[dt_singleton]],
},
["#"] = {
["__text"] = [[Each existing tag has a numeric entry in the tags table - use ipairs to iterate over them.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["create"] = {
["__text"] = [[Creates a new tag and return it. If the tag exists return the existing tag.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the new tag.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["find"] = {
["__text"] = [[Returns the tag object or nil if the tag doesn't exist.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The tag object or nil.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The name of the tag to find.]],
["__attributes"] = {
["reported_type"] = [[string]],
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
["__text"] = [[This table regroups values that describe details of the configuration of darktable.]],
["__attributes"] = {
["reported_type"] = [[table]],
},
["version"] = {
["__text"] = [[The version number of darktable.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["has_gui"] = {
["__text"] = [[True if darktable has a GUI (launched through the main darktable command, not darktable-cli).]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["verbose"] = {
["__text"] = [[True if the Lua logdomain is enabled.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["tmp_dir"] = {
["__text"] = [[The name of the directory where darktable will store temporary files.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["config_dir"] = {
["__text"] = [[The name of the directory where darktable will find its global configuration objects (modules).]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["cache_dir"] = {
["__text"] = [[The name of the directory where darktable will store its mipmaps.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["api_version_major"] = {
["__text"] = [[The major version number of the lua API.]],
["__attributes"] = {
["reported_type"] = [[number]],
},
},
["api_version_minor"] = {
["__text"] = [[The minor version number of the lua API.]],
["__attributes"] = {
["reported_type"] = [[number]],
},
},
["api_version_patch"] = {
["__text"] = [[The patch version number of the lua API.]],
["__attributes"] = {
["reported_type"] = [[number]],
},
},
["api_version_suffix"] = {
["__text"] = [[The version suffix of the lua API.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["api_version_string"] = {
["__text"] = [[The version description of the lua API. This is a string compatible with the semantic versioning convention]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["check_version"] = {
["__text"] = [[Check that a module is compatible with the running version of darktable
Add the following line at the top of your module : <code>darktable.configuration.check(...,{M,m,p},{M2,m2,p2})</code>To document that your module has been tested with API version M.m.p and M2.m2.p2.
This will raise an error if the user is running a released version of DT and a warning if he is running a development version
(the ... here will automatically expand to your module name if used at the top of your script]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the module to report on error]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[Tables of API versions that are known to work with the scrip]],
["__attributes"] = {
["reported_type"] = [[table...]],
},
},
},
},
},
},
["preferences"] = {
["__text"] = [[Lua allows you do manipulate preferences. Lua has its own namespace for preferences and you can't access nor write normal darktable preferences.
Preference handling functions take a _script_ parameter. This is a string used to avoid name collision in preferences (i.e namespace). Set it to something unique, usually the name of the script handling the preference.
Preference handling functions can't guess the type of a parameter. You must pass the type of the preference you are handling. 
Note that the directory, enum and file type preferences are stored internally as string. The user can only select valid values, but a lua script can set it to any string]],
["__attributes"] = {
["reported_type"] = [[table]],
},
["register"] = {
["__text"] = [[Creates a new preference entry in the Lua tab of the preference screen. If this function is not called the preference can't be set by the user (you can still read and write invisible preferences).]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[Invisible prefix to guarantee unicity of preferences.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[A unique name used with the script part to identify the preference.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[The type of the preference - one of the string values described above.]],
["__attributes"] = {
["reported_type"] = {
["__text"] = [[The type of value to save in a preference]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[string]],
["2"] = [[bool]],
["3"] = [[integer]],
["4"] = [[float]],
["5"] = [[file]],
["6"] = [[directory]],
["7"] = [[enum]],
},
},
},
},
},
["4"] = {
["__text"] = [[The label displayed in the preference screen.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["5"] = {
["__text"] = [[The tooltip to display in the preference menu.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["6"] = {
["__text"] = [[Default value to use when not set explicitly or by the user.
For the enum type of pref, this is mandatory]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[depends on type]],
},
},
["7"] = {
["__text"] = [[Minimum value (integer and float preferences only).]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[int or float]],
},
},
["8"] = {
["__text"] = [[Maximum value (integer and float preferences only).]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[int or float]],
},
},
["9"] = {
["__text"] = [[Step of the spinner (float preferences only).]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[float]],
},
},
["10"] = {
["__text"] = [[Other allowed values (enum preferences only)]],
["__attributes"] = {
["reported_type"] = [[string...]],
},
},
},
},
},
["read"] = {
["__text"] = [[Reads a value from a Lua preference.]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[The value of the preference.]],
["__attributes"] = {
["reported_type"] = [[depends on type]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[Invisible prefix to guarantee unicity of preferences.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The name of the preference displayed in the preference screen.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[The type of the preference.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
},
},
},
},
},
["write"] = {
["__text"] = [[Writes a value to a Lua preference.]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[Invisible prefix to guarantee unicity of preferences.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The name of the preference displayed in the preference screen.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[The type of the preference.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
},
},
["4"] = {
["__text"] = [[The value to set the preference to.]],
["__attributes"] = {
["reported_type"] = [[depends on type]],
},
},
},
},
},
},
["styles"] = {
["__text"] = [[This pseudo table allows you to access and manipulate styles.]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [[dt_singleton]],
},
["#"] = {
["__text"] = [[Each existing style has a numeric index; you can iterate them using ipairs.]],
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
["__text"] = [[Import a style from an external .dtstyle file]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The file to import]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["export"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"]]=],
},
["database"] = {
["__text"] = [[Allows to access the database of images. Note that duplicate images (images with the same RAW but different XMP) will appear multiple times with different duplicate indexes. Also note that all images are here. This table is not influenced by any GUI filtering (collections, stars etc...).]],
["__attributes"] = {
["has_ipairs"] = true,
["has_length"] = true,
["has_pairs"] = true,
["is_singleton"] = true,
["reported_type"] = [[dt_singleton]],
},
["#"] = {
["__text"] = [[Each image in the database appears with a numerical index; you can interate them using ipairs.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["duplicate"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"]]=],
["import"] = {
["__text"] = [[Imports new images into the database.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The filename or directory to import images from.

NOTE: If the images are set to be imported recursively in preferences only the toplevel film is returned (the one whose path was given as a parameter).

NOTE2: If the parameter is a directory the call is non-blocking; the film object will not have the newly imported images yet. Use a post-import-film filtering on that film to react when images are actually imported.


]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["move_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]]=],
["copy_image"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]]=],
["delete"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]]=],
},
["debug"] = {
["__text"] = [[This section must be activated separately by calling 

require "darktable.debug"
]],
["__attributes"] = {
["reported_type"] = [[table]],
},
["dump"] = {
["__text"] = [[This will return a string describing everything Lua knows about an object, used to know what an object is.

This function is recursion-safe and can be used to dump _G if needed.]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[A string containing a text description of the object - can be very long.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The object to dump.]],
["__attributes"] = {
["reported_type"] = [[anything]],
},
},
["2"] = {
["__text"] = [[A name to use for the object.]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[string]],
},
},
["3"] = {
["__text"] = [[A table of object,string pairs. Any object in that table will not be dumped, the string will be printed instead.
defaults to darktable.debug.known if not set]],
["__attributes"] = {
["optional"] = true,
["reported_type"] = [[table]],
},
},
},
},
},
["debug"] = {
["__text"] = [[Initialized to false; set it to true to also dump information about metatables.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
["max_depth"] = {
["__text"] = [[Initialized to 10; The maximum depth to recursively dump content.]],
["__attributes"] = {
["reported_type"] = [[number]],
},
},
["known"] = {
["__text"] = [[A table containing the default value of darktable.debug.dump.known]],
["__attributes"] = {
["reported_type"] = [[table]],
},
},
["type"] = {
["__text"] = [[Similar to the system function type() but it will return the real type instead of "userdata" for darktable specific objects.]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[A string describing the type of the object.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The object whos type must be reported.]],
["__attributes"] = {
["reported_type"] = [[anything]],
},
},
},
},
},
},
},
["types"] = {
["__text"] = [[This section documents types that are specific to darktable's Lua API.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["dt_lua_image_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_imageio_module_format_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_imageio_module_format_data_png"] = {
["__text"] = [[Type object describing parameters to export to png.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["bpp"] = {
["__text"] = [[The bpp parameter to use when exporting.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_tiff"] = {
["__text"] = [[Type object describing parameters to export to tiff.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["bpp"] = {
["__text"] = [[The bpp parameter to use when exporting.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_exr"] = {
["__text"] = [[Type object describing parameters to export to exr.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["compression"] = {
["__text"] = [[The compression parameter to use when exporting.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_copy"] = {
["__text"] = [[Type object describing parameters to export to copy.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_format_data_pfm"] = {
["__text"] = [[Type object describing parameters to export to pfm.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_format_data_jpeg"] = {
["__text"] = [[Type object describing parameters to export to jpeg.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["quality"] = {
["__text"] = [[The quality to use at export time.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
},
["dt_imageio_module_format_data_ppm"] = {
["__text"] = [[Type object describing parameters to export to ppm.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_format_data_webp"] = {
["__text"] = [[Type object describing parameters to export to webp.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["quality"] = {
["__text"] = [[The quality to use at export time.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["comp_type"] = {
["__text"] = [[The overall quality to use; can be one of "webp_lossy" or "webp_lossless".]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[Type of compression for webp]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[webp_lossy]],
["2"] = [[webp_lossless]],
},
},
},
["write"] = true,
},
},
["hint"] = {
["__text"] = [[A hint on the overall content of the image.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[a hint on the way to encode a webp image]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[hint_default]],
["2"] = [[hint_picture]],
["3"] = [[hint_photo]],
["4"] = [[hint_graphic]],
},
},
},
["write"] = true,
},
},
},
["dt_imageio_module_format_data_j2k"] = {
["__text"] = [[Type object describing parameters to export to jpeg2000.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["quality"] = {
["__text"] = [[The quality to use at export time.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["bpp"] = {
["__text"] = [[The bpp parameter to use when exporting.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[number]],
["write"] = true,
},
},
["format"] = {
["__text"] = [[The format to use.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[J2K format type]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[j2k]],
["2"] = [[jp2]],
},
},
},
["write"] = true,
},
},
["preset"] = {
["__text"] = [[The preset to use.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = {
["__text"] = [[J2K preset type]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[off]],
["2"] = [[cinema2k_24]],
["3"] = [[cinema2k_48]],
["4"] = [[cinema4k_24]],
},
},
},
["write"] = true,
},
},
},
["dt_imageio_module_storage_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["dt_imageio_module_storage_data_email"] = {
["__text"] = [[An object containing parameters to export to email.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_storage_data_flickr"] = {
["__text"] = [[An object containing parameters to export to flickr.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_storage_data_facebook"] = {
["__text"] = [[An object containing parameters to export to facebook.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_storage_data_latex"] = {
["__text"] = [[An object containing parameters to export to latex.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["filename"] = {
["__text"] = [[The filename to export to.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["title"] = {
["__text"] = [[The title to use for export.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
},
["dt_imageio_module_storage_data_picasa"] = {
["__text"] = [[An object containing parameters to export to picasa.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
},
["dt_imageio_module_storage_data_gallery"] = {
["__text"] = [[An object containing parameters to export to gallery.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["filename"] = {
["__text"] = [[The filename to export to.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
["title"] = {
["__text"] = [[The title to use for export.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
},
["dt_imageio_module_storage_data_disk"] = {
["__text"] = [[An object containing parameters to export to disk.]],
["__attributes"] = {
["has_pairs"] = true,
["parent"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["reported_type"] = [[dt_type]],
},
["filename"] = {
["__text"] = [[The filename to export to.]],
["__attributes"] = {
["is_attribute"] = true,
["read"] = true,
["reported_type"] = [[string]],
["write"] = true,
},
},
},
["dt_lua_film_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
["dt_style_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]]=],
["dt_style_item_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["#"].__attributes["reported_type"]]=],
["dt_lua_tag_t"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
["dt_lib_module_t"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]]=],
["dt_view_t"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
["dt_lua_backgroundjob_t"] = {} --[=[API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]]=],
["dt_lua_snapshot_t"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]]=],
["hint_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_webp"]["hint"].__attributes["reported_type"]]=],
["snapshot_direction_t"] = {} --[=[API["darktable"]["gui"]["libs"]["snapshots"]["direction"].__attributes["reported_type"]]=],
["dt_imageio_j2k_format_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_j2k"]["format"].__attributes["reported_type"]]=],
["dt_imageio_j2k_preset_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_j2k"]["preset"].__attributes["reported_type"]]=],
["yield_type"] = {
["__text"] = [[What type of event to wait for]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[WAIT_MS]],
["2"] = [[FILE_READABLE]],
["3"] = [[RUN_COMMAND]],
},
},
},
["comp_type_t"] = {} --[=[API["types"]["dt_imageio_module_format_data_webp"]["comp_type"].__attributes["reported_type"]]=],
["lua_pref_type"] = {} --[=[API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]]=],
["dt_imageio_exr_compression_t"] = {
["__text"] = [[The type of compression to use for the EXR image]],
["__attributes"] = {
["reported_type"] = [[enum]],
["values"] = {
["1"] = [[off]],
["2"] = [[rle]],
["3"] = [[zips]],
["4"] = [[zip]],
["5"] = [[piz]],
["6"] = [[pxr24]],
["7"] = [[b44]],
["8"] = [[b44a]],
},
},
},
},
["events"] = {
["__text"] = [[This section documents events that can be used to trigger Lua callbacks.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["intermediate-export-image"] = {
["__text"] = [[This event is called each time an image is exported, once for each image after the image has been processed to an image format but before the storage has moved the image to its final destination.]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the event that triggered the callback.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The image object that has been exported.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["3"] = {
["__text"] = [[The name of the file that is the result of the image being processed.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["4"] = {
["__text"] = [[The format used to export the image.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
["5"] = {
["__text"] = [[The storage used to export the image (can be nil).]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["post-import-image"] = {
["__text"] = [[This event is triggered whenever a new image is imported into the database.

	This event can be registered multiple times, all callbacks will be called.]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the event that triggered the callback.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The image object that has been exported.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["shortcut"] = {
["__text"] = [[This event registers a new keyboard shortcut. The shortcut isn't bound to any key until the users does so in the preference panel.

	The event is triggered whenever the shortcut is triggered.


	This event can only be registered once per value of shortcut.
	]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the event that triggered the callback.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The tooltip string that was given at registration time.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
["signature"] = {
["1"] = {
["__text"] = [[The string that will be displayed on the shortcut preference panel describing the shortcut.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
},
},
},
},
["post-import-film"] = {
["__text"] = [[This event is triggered when an film import is finished (all post-import-image callbacks have already been triggered). This event can be registered multiple times.
	]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The name of the event that triggered the callback.]],
["__attributes"] = {
["reported_type"] = [[string]],
},
},
["2"] = {
["__text"] = [[The new film that has been added. If multiple films were added recursively only the top level film is reported.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["view-changed"] = {
["__text"] = [[This event is triggered after the user changed the active view]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[The view that we just left]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
["2"] = {
["__text"] = [[The view we are now in]],
["__attributes"] = {
["reported_type"] = {} --[=[API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]]=],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["global_toolbox-grouping_toggle"] = {
["__text"] = [[This event is triggered after the user toggled the grouping button.]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[the new grouping status.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["global_toolbox-overlay_toggle"] = {
["__text"] = [[This event is triggered after the user toggled the overlay button.]],
["__attributes"] = {
["reported_type"] = [[event]],
},
["callback"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[function]],
["signature"] = {
["1"] = {
["__text"] = [[the new overlay status.]],
["__attributes"] = {
["reported_type"] = [[boolean]],
},
},
},
},
},
["extra_registration_parameters"] = {
["__text"] = [[This event has no extra registration parameters.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
},
["attributes"] = {
["__text"] = [[This section documents various attributes used throughout the documentation.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["write"] = {
["__text"] = [[This object is a variable that can be written to.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
["has_tostring"] = {
["__text"] = [[This object has a specific reimplementation of the "tostring" method that allows pretty-printing it.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
["implicit_yield"] = {
["__text"] = [[This call will release the Lua lock while executing, thus allowing other Lua callbacks to run.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
["parent"] = {
["__text"] = [[This object inherits some methods from another object. You can call the methods from the parent on the child object]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
},
},
["system"] = {
["__text"] = [[This section documents changes to system functions.]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["coroutine"] = {
["__text"] = [[]],
["__attributes"] = {
["reported_type"] = [[documentation node]],
},
["yield"] = {
["__text"] = [[Lua functions can yield at any point. The parameters and return types depend on why we want to yield.
A callback that is yielding allows other Lua code to run.

* wait_ms: one extra parameter; the execution will pause for that many milliseconds; yield returns nothing;
* file_readable: an opened file from a call to the OS library; will return when the file is readable; returns nothing;
* run_command: a command to be run by "sh -c"; will return when the command terminates; returns the return code of the execution.
]],
["__attributes"] = {
["reported_type"] = [[function]],
["ret_val"] = {
["__text"] = [[Nothing for "wait_ms" and "file_readable"; the returned code of the command for "run_command".]],
["__attributes"] = {
["reported_type"] = [[variable]],
},
},
["signature"] = {
["1"] = {
["__text"] = [[The type of yield.]],
["__attributes"] = {
["reported_type"] = {} --[=[API["types"]["yield_type"]]=],
},
},
["2"] = {
["__text"] = [[An extra parameter: integer for "wait_ms", open file for "file_readable", string for "run_command".]],
["__attributes"] = {
["reported_type"] = [[variable]],
},
},
},
},
},
},
},
}
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["styles"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
API["types"]["dt_style_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]
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
API["darktable"]["styles"]["export"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["export"]
API["darktable"]["database"]["duplicate"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["duplicate"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["blue"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["green"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["yellow"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["purple"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["red"]
API["darktable"]["styles"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["delete"]
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
API["darktable"]["database"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_lua_image_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["intermediate-export-image"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["post-import-image"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["apply_style"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]
API["darktable"]["styles"]["apply"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["apply"]
API["types"]["comp_type_t"] = API["types"]["dt_imageio_module_format_data_webp"]["comp_type"].__attributes["reported_type"]
API["types"]["hint_t"] = API["types"]["dt_imageio_module_format_data_webp"]["hint"].__attributes["reported_type"]
API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]["reset"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["styles"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["metadata_view"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["metadata"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["hinter"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["modulelist"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["filmstrip"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["viewswitcher"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["darktable_label"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["tagging"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["geotagging"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["recentcollect"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["global_toolbox"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["filter"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["import"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["ratings"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["select"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["collect"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["colorlabels"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["lighttable_mode"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["copy_history"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["image"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["modulegroups"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["module_toolbox"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["session"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["histogram"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["export"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["history"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["colorpicker"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["navigation"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["masks"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["view_toolbox"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["live_view"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["map_settings"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["camera"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["location"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["darktable"]["gui"]["libs"]["backgroundjobs"].__attributes["parent"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["types"]["dt_lib_module_t"] = API["darktable"]["gui"]["libs"]["snapshots"].__attributes["parent"]
API["types"]["snapshot_direction_t"] = API["darktable"]["gui"]["libs"]["snapshots"]["direction"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["film"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["films"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["films"]["new"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["types"]["dt_lua_film_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["events"]["post-import-film"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]
API["darktable"]["styles"]["duplicate"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["duplicate"]
API["darktable"]["films"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]
API["darktable"]["gui"]["current_view"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["map"].__attributes["parent"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["darkroom"].__attributes["parent"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["lighttable"].__attributes["parent"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["tethering"].__attributes["parent"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["gui"]["views"]["slideshow"].__attributes["parent"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["types"]["dt_view_t"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["events"]["view-changed"]["callback"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["events"]["view-changed"]["callback"].__attributes["signature"]["2"].__attributes["reported_type"] = API["darktable"]["gui"]["current_view"].__attributes["ret_val"].__attributes["reported_type"]
API["darktable"]["tags"]["get_tags"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["get_tags"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["move_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]
API["darktable"]["database"]["move_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["detach_tag"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]
API["darktable"]["tags"]["detach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"]
API["darktable"]["styles"]["create"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"]
API["types"]["dt_imageio_j2k_preset_t"] = API["types"]["dt_imageio_module_format_data_j2k"]["preset"].__attributes["reported_type"]
API["types"]["dt_style_item_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["create_style"].__attributes["ret_val"].__attributes["reported_type"]["#"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["delete"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["detach"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["tags"]["#"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["darktable"]["tags"]["find"].__attributes["ret_val"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_lua_tag_t"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]
API["types"]["dt_imageio_j2k_format_t"] = API["types"]["dt_imageio_module_format_data_j2k"]["format"].__attributes["reported_type"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["attach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]
API["darktable"]["tags"]["attach"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"]
API["darktable"]["database"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["delete"]
API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["copy"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]
API["darktable"]["database"]["copy_image"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["move"].__attributes["signature"]["2"].__attributes["reported_type"]["copy_image"]
API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]["select"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]
API["types"]["dt_lua_snapshot_t"] = API["darktable"]["gui"]["libs"]["snapshots"]["#"].__attributes["reported_type"]
API["darktable"]["tags"]["delete"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]["write_image"].__attributes["signature"]["2"].__attributes["reported_type"]["attach_tag"].__attributes["signature"]["1"].__attributes["reported_type"]["delete"]
API["darktable"]["preferences"]["read"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["darktable"]["preferences"]["write"].__attributes["signature"]["3"].__attributes["reported_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["types"]["lua_pref_type"] = API["darktable"]["preferences"]["register"].__attributes["signature"]["3"].__attributes["reported_type"]
API["system"]["coroutine"]["yield"].__attributes["signature"]["1"].__attributes["reported_type"] = API["types"]["yield_type"]
API["darktable"]["gui"]["create_job"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"] = API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]
API["types"]["dt_lua_backgroundjob_t"] = API["darktable"]["gui"]["create_job"].__attributes["ret_val"].__attributes["reported_type"]
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
API["events"]["intermediate-export-image"]["callback"].__attributes["signature"]["4"].__attributes["reported_type"] = API["darktable"]["register_storage"].__attributes["signature"]["3"].__attributes["signature"]["1"].__attributes["reported_type"]["supports_format"].__attributes["signature"]["2"].__attributes["reported_type"]
return API
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
