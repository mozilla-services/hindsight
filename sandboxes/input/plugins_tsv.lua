-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
# Hindsight plugins.tsv reader

Reads in the plugins.tsv file to enable plugin monitors to be written.

## Sample Configuration
```lua
filename = "plugin_tsv.lua"
ticker_interval = 60
preserve_data = false
```
--]]
require "io"
require "string"
local l = require "lpeg"
l.locale(l)

local output_path = read_config("output_path")
local sandbox_run_path = read_config("sandbox_run_path")

local eol = l.P"\n"
local tab = l.P"\t"
local header = (1 - eol)^1 * eol
local plugin = l.C((1 - tab)^1)
local number = l.digit^1 / tonumber
local row = l.Cg(plugin * l.Ct((tab * number)^1)) * eol
local grammar = header * l.Cf(l.Ct"" * row^0, rawset) * -1
local split_plugin_name = l.C((1 - l.P".")^1) * l.P"." * l.C(l.P(1)^1)

local terminated_msg = {
    Type = "hindsight.plugin.terminated",
    Payload = "",  -- termination message
    Fields = {
        name = nil -- plugin name
    }
}

local function process_terminated(prev)
    for k, a in pairs(prev) do -- prev contains stopped/deleted/terminated plugins
        local pt, pn = split_plugin_name:match(k)
        if not pt then
            print("internal error, invalid plugin name:", k)
            return
        end
        local path = string.format("%s/%s/%s.err", sandbox_run_path, pt, pn)
        local fh = io.open(path)
        if fh then
            terminated_msg.Payload = fh:read("*a")
            terminated_msg.Fields.name = k;
            fh:close()
            inject_message(terminated_msg)
        end
    end
end

local msg = {
    Type = "hindsight.plugin.stats",
    Fields = {
        name = nil,  -- plugin name
        deltas = nil -- array of plugin stat deltas
    }
}

local prev = {}
local deltas = nil
function process_message()
    local fh = io.open(output_path .. "/plugins.tsv")
    if not fh then return 0 end -- stats file not available yet

    local s = fh:read("*a")
    fh:close()
    if not s then return 0 end

    local cur = grammar:match(s)
    if not cur then return -1 end
    for k, a in pairs(cur) do
        local old = prev[k]
        if old then
            for i, cv in ipairs(a) do
                local ov = old[i] or 0 -- allow for new appended columns
                deltas[i] = cv - ov
            end
            prev[k] = nil
        else
            deltas = a
        end
        msg.Fields.name = k
        msg.Fields.deltas = deltas
        inject_message(msg)
    end
    process_terminated(prev)
    prev = cur
    return 0
end
