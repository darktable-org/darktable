-- Tiny example Lua script for the `camera-image-downloaded` event.
-- Save this in your luarc and require it, or copy its body into luarc.

local dt = require "darktable"

local function log_camera_download(event, camera_model, camera_port, in_path, in_filename, filename)
  dt.print(string.format(
    "camera-image-downloaded: model=%s port=%s source=%s/%s local=%s",
    camera_model,
    camera_port,
    in_path,
    in_filename,
    filename
  ))

  -- Drop in AI pipeline call here if you want to enqueue a follow-up job.
end

dt.register_event("camera-image-downloaded", log_camera_download)
