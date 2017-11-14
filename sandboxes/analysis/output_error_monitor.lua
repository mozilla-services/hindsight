-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Monitors Parquet output plugin for errors, alerting when they pass a given threshold.

```lua
filename = 'parquet_output_error_monitor.lua'
message_matcher = 'Type == "hindsight.plugins"'
ticker_interval = 60
preserve_data = false

alert = {
  disabled = false,
  prefix = true,
  throttle = 1440, -- once a day
  modules = {
    email = {recipients = {"trink@mozilla.com"}},
  },

  thresholds = {
    "output.s3_parquet" = {
      percent = 1 -- alert if more than 1% of outputs cause an error
      number = 100 -- alert if there are more than 100 errors reported at once
    }
  }
}
```
--]]
local alert      = require "heka.alert"
local thresholds = read_config("alert").thresholds

for k, t in pairs(thresholds) do
    if t.number then
        assert(type(t.number) == "number" and t.number > 0,
               string.format("alert: \"%s\".number must contain a number of message failures", k))
    end
    if t.percent then
        assert(type(t.percent) == "number" and t.percent > 0 and t.percent <= 100,
               string.format("alert: \"%s\".percent must contain a percentage of message failures (1-100)", k))
    end
end

-- INJECT_MESSAGE_COUNT = 0
-- INJECT_MESSAGE_BYTES = 1
PROCESS_MESSAGE_COUNT = 2
PROCESS_MESSAGE_FAILURES = 3
-- CURRENT_MEMORY = 4
-- MAX_MEMORY = 5
-- MAX_OUTPUT = 6
-- MAX_INSTRUCTIONS = 7
-- MESSAGE_MATCHER_AVG = 8
-- MESSAGE_MATCHER_SD = 9
-- PROCESS_MESSAGE_AVG = 10
-- PROCESS_MESSAGE_SD = 11
-- TIMER_EVENT_AVG = 12
-- TIMER_EVENT_SD = 13



function process_message()
    for k, t in pairs(thresholds) do
        local ff = string.format("Fields[%s]", k)
        local count = read_message(ff, nil, PROCESS_MESSAGE_COUNT)
        if count < t["minimum_sample"] then
            return 0
        end
        local fails = read_message(ff, nil, PROCESS_MESSAGE_FAILURES)
        local fail_percent = fails / count * 100
        if t["percent"] and fail_percent > t["percent"] then
            alert.send(k, "percent", string.format("%g%% of recent messages failed", fail_percent))
        end
    end
    return 0
end

function timer_event()
end
