-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Simple message throughput counter/visualization

-- .cfg
filename        = "throughput.lua"
message_matcher = "TRUE"
ticker_interval = 60
output_limit = 1024 * 1024

rows = 86400
sec_per_row = 1

--]]

require "circular_buffer"
require "os"
local time = os.time

local rows        = read_config("rows") or 1440
local sec_per_row = read_config("sec_per_row") or 60

local cb = circular_buffer.new(rows, 1, sec_per_row)
cb:set_header(1, "messages")

function process_message()
    cb:add(time() * 1e9, 1, 1)
    return 0
end

function timer_event(ns)
    inject_payload("cbuf", "counts", cb)
end
