-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"
require "heka_stream_reader"
require "string"

local fn = read_config("input_file")
local hsr = heka_stream_reader.new(fn)
local cnt = 0

function process_message(offset)
    local fh = assert(io.open(fn, "rb"))
    if offset then fh:seek("set", offset) end

    local found, read, need = false, 0, 8192
    local offset = 0
    while true do
        local buf = fh:read(need)
        if not buf then break end

        repeat
            found, read, need = hsr:find_message(buf)
            if read then offset = offset + read end
            if found then
                cnt = cnt + 1
                inject_message(hsr, offset)
            end
            buf = nil
        until not found
    end
    fh:close()

    return 0, tostring(cnt)
end
