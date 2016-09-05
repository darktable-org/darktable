--[[
  This file is part of darktable,
  copyright (c) 2016 Bill Ferguson
  copyright (c) 2016 Tobias Jakobs
  copyright (c) 2014 Jérémy Rosen
  
  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
]]
--[[
  script_manager - a drop in luarc file that provides script management

  script_manager creates two modules, Script Install/Update and Script
  Controller.  Script Install/Update installs the scripts, from the
  specified repository, if they don't exist.  Once the scripts are 
  installed the module provides a way to update the scripts with one
  click.  After the scripts are installed, Script Controller starts.  A
  button for each script is displayed.  Hovering over the button displays 
  the documentation from the script. Clicking the button loads the script
  and runs it.  A preference is updated to record that the script is active.
  Once a script is active, the button changes to Deactivate to turn off the
  script.

  ADDITIONAL SOFTWARE NEEDED FOR THIS SCRIPT
  * git - https://git-scm.com/

  USAGE
  * replace the existing luarc file with this one.  
  * start darktable
  * click install to install the scripts from the specified repository (default:
    https://github.com/darktable-org/lua-scripts.git), if they aren't already.
  * click update to retrieve the latest version of the scripts from the
    specified repository (default: https://github.com/darktable-org/lua-scripts.git)
  * activate the scripts you want 

  CAVEATS
  * deactivate doesn't take effect until darktable is restarted.  There currently isn't
    a way to unload a script from the UI (that I'm aware of).
  * Newer scripts retrieved using update won't take effect until darktable is restarted
    since we can't unload running scripts.  A script that isn't active and gets updated
    will run the newest version if activated after the update.
  * changing the repository, then clicking update will cause the current lua directory
    ($HOME/lua-scripts) to be removed and a new one created by cloning the specified
    repository.

    BUGS, COMMENTS, SUGGESTIONS
    * Send to Bill Ferguson, wpferguson@gmail.com


]]
local dt = require "darktable"

dt.configuration.check_version(...,{3,0,0})

local lua_path = dt.configuration.config_dir .. "/lua"
local lua_script_repo = "https://github.com/darktable-org/lua-scripts.git"
local script_controller_not_installed = true
local script_widgets = {}
local widget_cnt = 1

-- Thanks Tobias Jakobs for the idea and the correction
function checkIfFileExists(filepath)
  local file = io.open(filepath,"r")
  local ret
  if file ~= nil then 
    io.close(file) 
    dt.print_error("true checkIfFileExists: "..filepath)
    ret = true
  else 
    dt.print_error(filepath.." not found")
    ret = false
  end
  return ret
end

-- Thanks Tobias Jakobs
local function checkIfBinExists(bin)
  local handle = io.popen("which "..bin)
  local result = handle:read()
  local ret
  handle:close()
  if (result) then
    dt.print_error("true checkIfBinExists: "..bin)
    ret = true
  else
    dt.print_error(bin.." not found")
    ret = false
  end
  return ret
end

-- Thanks Jérémy Rosen
local function load_scripts()
  local output = io.popen("cd "..dt.configuration.config_dir.."/lua ;find . -name \\*.lua -print | sort")
  for line in output:lines() do
    local req_name = line:sub(3,-5)
    if not string.match(req_name, "example") then  -- skip the example scripts
      if not string.match(req_name, "include_all") then -- skip include all since this replaces it
        if not string.match(req_name, "yield") then -- special case, because everything needs this
        -- check if we know about this script
          if dt.preferences.read("script_controller", req_name, "bool") then
            prequire(req_name)
            if script_controller_not_installed then
              add_script_widget(req_name, true)
            end
          else
            dt.preferences.write("script_controller", req_name, "bool", false)
            if script_controller_not_installed then
              add_script_widget(req_name, false)
            end
          end
        else
          prequire(req_name)
        end
      end
    end
  end
  
  if script_controller_not_installed then
    -- install it
    dt.register_lib(
      "Script Controller",     -- Module name
      "Script Controller",     -- name
      true,                -- expandable
      false,               -- resetable
      {[dt.gui.views.lighttable] = {"DT_UI_CONTAINER_PANEL_LEFT_BOTTOM", 100}},   -- containers
      dt.new_widget("box") -- widget
      {
        orientation = "vertical",
        table.unpack(script_widgets),
      },
      nil,-- view_enter
      nil -- view_leave
    )
    script_controller_not_installed = false
  end
end

-- protected require, load but recover frome errors

function prequire(req_name)
  dt.print_error("Loading " .. req_name)
  local status, lib = pcall(require, req_name)
  if status then
    dt.print_error("Loaded " .. req_name)
  else
    dt.print_error("Error loading " .. req_name)
  end
end

function add_script_widget(req_name, script_state)
  local button_text = ""
  if script_state then
    button_text = "Deactivate " .. req_name
  else
    button_text = "Activate " .. req_name
  end
  script_widgets[widget_cnt] = dt.new_widget("button")
  {
    label = button_text,
    tooltip = get_script_doc(req_name),
    clicked_callback = function (self)
      -- split the label into action and target
      local action, target = string.match(self.label, "(.+) (.+)")
      -- load the script if it's not loaded
      if action == "Activate" then
        dt.preferences.write("script_controller", target, "bool", true)
        dt.print_error("Loading " .. target)
        local status, lib = pcall(require, target)
        if status then
          dt.print("Loaded " .. target)
        else
          dt.print_error("Error loading " .. target)
        end
        self.label = "Deactivate " .. target
      else
        dt.preferences.write("script_controller", target, "bool", false)
        -- ideally we would call a deactivate method provided by the script
        dt.print(target .. " will not be active when darktable is restarted")
        self.label = "Activate " .. target
      end
    end
  }
  widget_cnt = widget_cnt + 1
end

-- get the script documentation, with some assumptions
function get_script_doc(script)
  local description = nil
  f = io.open(lua_path .. "/" .. script .. ".lua")
  if f then
    -- slurp the file
    local content = f:read("*all")
    f:close()
    -- assume that the second block comment is the documentation
    description = string.match(content, "%-%-%[%[.-%]%].-%-%-%[%[(.-)%]%]")
  else
    dt.print_error("Cant read from " .. script)
  end
  if description then
    return description
  else
    return "No documentation available"
  end
end


local script_repo = dt.preferences.read("script_manager", "repository", "string")
if script_repo == "" then
  dt.preferences.write("script_manager", "repository", "string", lua_script_repo)
  script_repo = lua_script_repo
end
dt.print_error("script_repo is set to " .. script_repo)

local action_button_text = "Install Scripts"
if checkIfFileExists(lua_path) then
  action_button_text = "Update Scripts"
end

action_button =  dt.new_widget("button")
{
  label = action_button_text,
  clicked_callback = function (_)
    if action_button.label == "Install Scripts" then
      if checkIfBinExists("git") then
        -- see if $HOME/lua-scripts exists and if it does move it out of the way
        local homedir = os.getenv("HOME")
        if checkIfFileExists(homedir .. "/lua-scripts") then
          os.execute("mv $HOME/lua-scripts $HOME/lua-scripts.save")
        end
        -- fetch it and set it up
        local result = os.execute("cd $HOME;git clone " .. script_repo)
        if not result then
          -- something went wrong
          dt.print("Error fetching scripts from " .. script_repo)
        else
          result = os.execute("ln -s $HOME/lua-scripts " .. lua_path)
          load_scripts()
--          require("contrib/gimp")
          action_button.label = "Update Scripts"
        end
      else
        dt.print("Git not installed.  Install git and restart darktable")
      end
    else -- doing an update
      dt.print_error("Updating scripts")
      if repository.text == script_repo then
        result = os.execute("cd " .. lua_path .. ";git pull")
        if not result then
          dt.print("Failed to update scripts")
        else
  --        package.loaded.gimp = nil
          load_scripts()
  --        require("contrib/gimp")
        end
      else -- repository changed
        script_repo = repository.text
        -- remove the existing repository
        os.execute("rm -f lua_path")
        os.execute("rm -rf $HOME/lua-scripts")
        -- clone from the new reposiitory
        local result = os.execute("cd $HOME;git clone " .. script_repo)
          -- if successful save the repository
        if not result then
          -- something went wrong
          dt.print("Error fetching scripts from " .. script_repo)
        else
          result = os.execute("ln -s $HOME/lua-scripts " .. lua_path)
          load_scripts()
          dt.preferences.write("script_manager", "repository", "string", script_repo)
--          require("contrib/gimp")
          action_button.label = "Update Scripts"
        end
      end
    end
  end
}

repository = dt.new_widget("entry")
{
    text = script_repo,
}


if checkIfFileExists(lua_path) then
  -- find the scripts and read them
  dt.print_error("Found the lua directory, now we need to read the scripts")
  load_scripts()
end

-- create the install/update module
dt.register_lib(
  "Script Install/Update",     -- Module name
  "Script Install/Update",     -- name
  true,                -- expandable
  false,               -- resetable
  {[dt.gui.views.lighttable] = {"DT_UI_CONTAINER_PANEL_LEFT_BOTTOM", 100}},   -- containers
  dt.new_widget("box") -- widget
  {
    orientation = "vertical",
    repository,
    action_button,
  },
  nil,-- view_enter
  nil -- view_leave
)