-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

--[[
# Plugin Statistic and Termination Monitor

This plugin has configuration support to monitor process_message failure rates
and plugin termination.

## Sample Configuration
```lua
filename = 'plugin_monitor.lua'
message_matcher = 'Type =~ "^hindsight%.plugin%."'
preserve_data = false
process_message_inject_limit = 1

alert = {
  disabled = false,
  prefix = true,
  throttle = 90,
  modules = {
    email = {recipients = {"trink@mozilla.com"}},
  },

  thresholds = {
    ["analysis.example"] = { -- plugin name
        process_message_failures = { -- alert specific configuration
          -- alert when percentage of recent failed messages exceeds this
          percent = 5,

          -- but only if this many messages have been processed since the last tick
          minimum_sample = 100
        },
        terminated = true -- alert if terminated
    },
    ["*"] = {
        terminated = true
    },
  }
}

```
--]]
require "string"

local alert = require "heka.alert"
local function verify_thresholds()
    for plugin, tcfg in pairs(alert.thresholds) do
        for at, acfg in pairs(tcfg) do
            if at == "process_message_failures" then
                assert(type(acfg.minimum_sample) == "number" and acfg.minimum_sample >= 0,
                       string.format("%s#%s 'minimum_sample' must be a non-negative number", plugin, at))
                assert(type(acfg.percent) == "number" and acfg.percent > 0 and  acfg.percent <= 100,
                       string.format("%s#%s 'percent' must contain a percentage of message failures (1-100)", plugin, at))
            elseif at == "terminated" then
                assert(type(acfg) == "boolean",
                       string.format("%s#%s terminated must be a boolean", plugin, at))
            else
                error(string.format("%s#%s invalid alert type", plugin, at))
            end
        end
    end
end
verify_thresholds()

-- INJECT_MESSAGE_COUNT = 0     -- probably wouldn't need as there is a lot more flexibility in a cbuf analysis plugin
-- INJECT_MESSAGE_BYTES = 1     -- same as above
PROCESS_MESSAGE_COUNT = 2
PROCESS_MESSAGE_FAILURES = 3
-- CURRENT_MEMORY = 4           -- probably wouldn't use as it can be highly variable
-- MAX_MEMORY = 5               -- wouldn't need to monitor unless it is configured to unlimited
-- MAX_OUTPUT = 6               -- wouldn't need to monitor unless it is configured to unlimited
-- MAX_INSTRUCTIONS = 7         -- wouldn't need to monitor unless it is configured to unlimited
-- MESSAGE_MATCHER_AVG = 8      -- e.g. this and everything below for slow plugin alerting but would mine utlizaton.tsv instead
-- MESSAGE_MATCHER_SD = 9
-- PROCESS_MESSAGE_AVG = 10
-- PROCESS_MESSAGE_SD = 11
-- TIMER_EVENT_AVG = 12
-- TIMER_EVENT_SD = 13

local function handle_stats(plugin, th)
    local pmf = th.process_message_failures
    if not pmf then return end

    local count = read_message("Fields[deltas]", 0, PROCESS_MESSAGE_COUNT)
    if count >= pmf.minimum_sample then
        local fails = read_message("Fields[deltas]", 0, PROCESS_MESSAGE_FAILURES)
        local fail_percent = fails / count * 100
        if fail_percent > pmf.percent then
            alert.send(plugin, "process_message_failures",
                       string.format("%g%% of recent messages failed", fail_percent))
        end
    end
end


function process_message()
    local mt = read_message("Type")
    local plugin = read_message("Fields[name]")

    local th = alert.get_threshold(plugin)
    if not th then return 0 end

    if mt == "hindsight.plugin.stats" then
        handle_stats(plugin, th)
    elseif mt == "hindsight.plugin.terminated" and th.terminated then
        alert.send(plugin, "terminated", read_message("Payload"), 0) -- no throttling
    end
    return 0
end


function timer_event()
end
