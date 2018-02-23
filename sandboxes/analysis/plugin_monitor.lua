-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
Monitors changes in plugin statistics and issues alerts on noteworthy
events.

Currently this watches for the percentage of message-processing failures
exceeding a threshold.

## Sample Configuration
```lua
filename = 'plugin_monitor.lua'
message_matcher = 'Type == "hindsight.plugins" && Fields[name] == "analysis.example"'
preserve_data = false
process_message_inject_limit = 1

alert = {
  disabled = false,
  prefix = false,
  throttle = 1440,
  modules = {
    email = {recipients = {"trink@mozilla.com"}},
  },

  thresholds = {
    process_message_failures = {
      percent = 5, -- alert when percentage of recent failed messages
                   -- exceeds this
      minimum_sample = 100 -- but only if this many messages have been
                           -- processed since the last tick
    },
  }
}

```
--]]
local alert      = require "heka.alert"
local alert_percent = alert.thresholds.process_message_failures.percent
local minimum_sample = alert.thresholds.process_message_failures.minimum_sample
assert(type(minimum_sample) == "number" and minimum_sample >= 0,
               string.format("alert: 'minimum_sample' must be a non-negative number", k))

assert(type(alert_percent) == "number" and alert_percent > 0 and alert_percent <= 100,
       string.format("alert: 'percent' must contain a percentage of message failures (1-100)", k))

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
    local count = read_message("Fields[delta]", 0, PROCESS_MESSAGE_COUNT)
    if count < minimum_sample then
        return 0
    end
    local fails = read_message("Fields[delta]", 0, PROCESS_MESSAGE_FAILURES)
    print("count", count, "fails", fails)
    local fail_percent = fails / count * 100
    if fail_percent > alert_percent then
        alert.send("process_message_failures", "percent", string.format("%g%% of recent messages failed", fail_percent))
    end
    return 0
end

function timer_event()
end
