-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
# Generates test data for stats_monitor
--]]

local inputs = {
    {Timestamp = 0, Type = "stats_monitor_test_percent", Payload = "0"},
    {Timestamp = 0, Type = "stats_monitor_test_count", Payload = "0"},
    {Timestamp = 0, Type = "stats_monitor_test_success", Payload = "0"},
}



function process_message()
    for x = 0, 89 do
        inputs[1].Payload = tostring(x)
        inject_message(inputs[1])
        inputs[2].Payload = tostring(x)
        inject_message(inputs[2])
    end
    for x = 1, 10 do
        inputs[1].Payload = tostring(-x)
        inject_message(inputs[1])
        inputs[2].Payload = tostring(-x)
        inject_message(inputs[2])
    end
    for x = 0, 100 do
        inputs[3].Payload = tostring(x)
        inject_message(inputs[3])
    end
    return 0
end
