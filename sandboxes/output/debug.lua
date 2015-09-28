-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Outputs a more user friendly version (RST format) of the full Heka message to stdout.

-- .cfg
filename        = "debug.lua"
message_matcher = "TRUE"

--]]

local write  = require "io".write
local floor  = require "math".floor
local date   = require "os".date
local byte   = require "string".byte
local concat = require "table".concat

local function get_uuid(uuid)
    return string.format("%X%X%X%X-%X%X-%X%X-%X%X-%X%X%X%X%X", byte(uuid, 1, 16))
end

local function get_timestamp(ts)
    local time_t = floor(ts / 1e9)
    local ns = ts - time_t * 1e9
    local ds = date("%Y-%m-%d %H:%M:%S", time_t)
    return string.format("%s.%09d +0000 UTC", ds, ns)
end

function process_message()
    local raw = read_message("raw")
    local msg = decode_message(raw)
    write(":Uuid: ", get_uuid(msg.Uuid), "\n")
    write(":Timestamp: ", get_timestamp(msg.Timestamp), "\n")
    write(":Type: ", msg.Type or "<nil>", "\n")
    write(":Logger: ", msg.Logger or "<nil>", "\n")
    write(":Severity: ", msg.Severity or 7, "\n")
    write(":Payload: ", msg.Payload or "<nil>", "\n")
    write(":EnvVersion: ", msg.EnvVersion or "<nil>", "\n")
    write(":Pid: ", msg.Pid or "<nil>", "\n")
    write(":Hostname: ", msg.Hostname or "<nil>", "\n")
    write(":Fields:\n")
    for i, v in ipairs(msg.Fields) do
        write("    | name:", v.name,
              " type:", v.value_type or 0,
              " representation:", v.representation or "<nil>",
              " value:")
        if v.value_type == 4 then
            for j, w in ipairs(v.value) do
                if j ~= 1 then write(",") end
                if w then write("true") else write("false") end
            end
            write("\n")
        else
            write(concat(v.value, ","), "\n")
        end
    end
    write("\n")
    return 0
end

function timer_event(ns)
    -- no op
end
