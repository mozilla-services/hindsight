-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"
require "string"

--[[
Outputs a Heka protobuf stream rolling the log file every time it reaches the
output_size.

-- .cfg

filename        = "heka_log_rolling.lua"
message_matcher = "TRUE"
ticker_interval = 0
preserve_data   = true

--location where the payload is written
output_dir      = "/tmp"
output_size     = 1024 * 1024 * 1024

--]]

file_num = 0

local output_dir    = read_config("output_dir") or "/tmp"
local output_prefix = read_config("cfg_name")
local output_size   = read_config("output_size") or 1e9
local fh

function process_message()
    if not fh then
        local fn = string.format("%s/%s.%d.log", output_dir, output_prefix, file_num)
        fh, err = io.open(fn, "a")
        if err then return -1, err end
    end

    local msg = read_message("framed")
    fh:write(msg)

    if fh:seek() >= output_size  then
        fh:close()
        fh = nil
        file_num = file_num + 1
    end
    return 0
end

function timer_event(ns)
    -- no op
end
