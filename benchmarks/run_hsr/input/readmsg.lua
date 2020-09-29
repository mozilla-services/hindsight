-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at https://mozilla.org/MPL/2.0/.

require "io"
require "string"

local fn = read_config("input_file")
local hsr = create_stream_reader(fn)
local cnt = 0

function process_message(offset)
    local fh = assert(io.open(fn, "rb"))
    if offset then fh:seek("set", offset) else offset = 0 end

    local found, read, consumed
    repeat
        repeat
            found, consumed, read = hsr:find_message(fh)
            offset = offset + consumed
            if found then
                cnt = cnt + 1
                inject_message(hsr, offset)
            end
        until not found
    until read == 0
    fh:close()
    return 0, tostring(cnt)
end
