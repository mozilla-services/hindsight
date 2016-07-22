-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"

local clf = require "lpeg.common_log_format"

local msg = {
Timestamp   = nil,
Type        = "logfile",
Hostname    = "trink-x230",
Logger      = "FxaAuthWebserver",
Payload     = nil,
Fields      = nil
}

local grammar = clf.build_nginx_grammar('$remote_addr $msec "$request" $status $body_bytes_sent "$http_referer" "$http_user_agent" "$http_x_forwarded_for"'
)
local cnt = 0;

local fn = read_config("input_file")

function process_message(offset)
    local fh = assert(io.open(fn, "rb"))
    if offset then fh:seek("set", offset) end

    for line in fh:lines() do
        local fields = grammar:match(line)
        if fields then
            msg.Timestamp = fields.time
            fields.time = nil
            msg.Fields = fields
            inject_message(msg, fh:seek())
            cnt = cnt + 1
        end
    end
    fh:close()

    return 0, tostring(cnt)
end
