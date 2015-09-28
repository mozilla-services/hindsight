-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Heka compatible TCP output

-- .cfg
filename        = "heka_tcp.lua"
message_matcher = "TRUE"

address = "127.0.0.1"
port    = 5565
timeout = 10

--]]

local socket = require "socket"

local address = read_config("address") or "127.0.0.1"
local port = read_config("port") or 5565
local timeout = read_config("timeout") or 10

local function create_client()
    local c, err = socket.connect(address, port)
    if c then
        c:setoption("tcp-nodelay", true)
        c:setoption("keepalive", true)
        c:settimeout(timeout)
    end
    return c, err
end

local client, err = create_client()

local function send_message(msg, i)
    local len, err, i = client:send(msg, i)
    if not len then
        if err == "timeout" or err == "closed" then
            client:close()
            client = nil
            return -3, err
        end
        return send_message(msg, i)
    end
    return 0
end

function process_message()
    if not client then
        client, err = create_client()
    end
    if not client then return -3, err end -- retry indefinitely
    local ret, err = send_message(read_message("framed"), 1)
    return ret, err
end

function timer_event(ns)
    -- no op
end
