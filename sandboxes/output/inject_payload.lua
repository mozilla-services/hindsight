-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"
require "string"

--[[
Outputs inject_payload messages to the configured directory.
Filename: output_dir/logger.payload_name.payload_type
Contents: message.Payload

-- .cfg
filename        = "inject_payload.lua"
message_matcher = "Type == 'inject_payload'"
ticker_interval = 0

-- location where the payload is written (e.g. make them accessible from a web
-- server for external consumption)
output_dir      = "/var/www/hindsight/payload"

--]]

local output_dir = read_config("output_dir") or "/tmp"

function process_message()
    local pt = read_message("Fields[payload_type]")
    if type(pt) ~= "string" then return -1, "invalid payload_type" end

    local pn = read_message("Fields[payload_name]") or ""
    if type(pn) ~= "string" then return -1, "invalid payload_name" end

    local logger = read_message("Logger") or ""

    pn = string.gsub(pn, "%W", "_")
    pt = string.gsub(pt, "%W", "_")
    logger = string.gsub(logger, "%W", "_")

    local fn = string.format("%s/%s.%s.%s", output_dir, logger, pn, pt)
    local fh, err = io.open(fn, "w")
    if err then return -1, err end

    local payload = read_message("Payload") or ""
    fh:write(payload)
    fh:close()

    return 0
end

function timer_event(ns)
    -- no op
end
