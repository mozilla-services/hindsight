-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
# Hindsight plugins.tsv reader

Reads in the plugins.tsv file to enable plugin monitors to be written.

## Sample Configuration
```lua
filename = "plugin_stats.lua"
ticker_interval = 60
preserve_data = false
```
--]]
require "io"
local l = require "lpeg"
l.locale(l)

local eol = l.P"\n"
local tab = l.P"\t"
local header = (1 - eol)^1 * eol
local plugin = l.C((1 - tab)^1)
local number = l.digit^1 / tonumber
local row = l.Cg(plugin * l.Ct((tab * number)^1)) * eol
local grammar = header * l.Cf(l.Ct"" * row^0, rawset) * -1
local msg = {
    Type = "hindsight.plugins",
    Payload = "",
    Fields = {delta = nil}
}
local prev = {}
local delta = nil
local output_path = read_config("output_path") -- provided by Hindsight
function process_message()
    local fh = io.open(output_path .. "/plugins.tsv")
    if not fh then return 0 end -- stats file not available yet

    local s = fh:read("*a")
    fh:close()
    if not s then return 0 end

    local cur = grammar:match(s)
    if not cur then return -1 end
    for k, a in pairs(cur) do
        local old = prev[k]
        if old then
            for i, cv in ipairs(a) do
                local ov = old[i] or 0 -- allow for new appended columns
                delta[i] = cv - ov
            end
        else
            delta = a
        end
        msg.Fields.name = k
        msg.Fields.delta = delta
        inject_message(msg)
    end
    prev = cur
    return 0
end
