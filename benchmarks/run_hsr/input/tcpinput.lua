-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "coroutine"
local socket = require "socket"
require "heka_stream_reader"
require "string"
require "table"

local address = read_config("address") or "127.0.0.1"
local port = read_config("port") or 5565
local server = assert(socket.bind(address, port))
server:settimeout(0)
local threads = {}
local sockets = {server}

local function handle_client(client)
    local found, consumed, need = false, 0, 8192 * 4
    local caddr, cport = client:getpeername()
    if not caddr then
        caddr = "unknown"
        cport = 0
    end

    local hsr = heka_stream_reader.new(
        string.format("%s:%d -> %s:%d", caddr, cport, address, port))
    client:settimeout(0)
    while client do
        local buf, err, partial = client:receive(need)
        if partial then buf = partial end
        if not buf then break end

        repeat
            found, consumed, need = hsr:find_message(buf)
            if found then inject_message(hsr) end
            buf = nil
        until not found

        if err == "closed" then break end

        coroutine.yield()
    end
end

function process_message()
    while true do
        local ready = socket.select(sockets, nil, 1)
        if ready then
            for _, s in ipairs(ready) do
                if s == server then
                    local client = s:accept()
                    if client then
                        sockets[#sockets + 1] = client
                        threads[client] = coroutine.create(
                            function() handle_client(client) end)
                    end
                else
                    if threads[s] then
                        local status = coroutine.resume(threads[s])
                        if not status then
                            s:close()
                            for i = #sockets, 2, -1 do
                                if s == sockets[i] then
                                    table.remove(sockets, i)
                                    break
                                end
                            end
                            threads[s] = nil
                        end
                    end
                end
            end
        end
    end
    return 0
end
