-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"

local cnt = 0

function process_message()
    --error("boom")
    cnt = cnt + 1
    return 0
end

function timer_event()
    io.write("count", "=", cnt, "\n")
end
