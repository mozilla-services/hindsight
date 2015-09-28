-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Reads a Heka protobuf stream from the stdin file handle

-- .cfg
filename = "heka_stdin.cfg"

--]]

require "io"
require "heka_stream_reader"

local hsr = heka_stream_reader.new(read_config("cfg_name"))

function process_message()
    local found, read, consumed
    repeat
        repeat
            found, consumed, read = hsr:find_message(io.stdin)
            if found then
                inject_message(hsr)
            end
        until not found
    until read == 0
    return 0
end
