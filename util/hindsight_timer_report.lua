#!/usr/bin/lua

require "io"
require "math"
require "string"

local function stats()
    local self = {count = 0, mean = 0, sum = 0}

    local count = function()
        return self.count
    end

    local add = function(d)
        local old_mean = self.mean
        local old_sum = self.sum
        self.count = self.count + 1
        if self.count == 1 then
            self.mean = d
        else
            self.mean = old_mean + (d - old_mean) / self.count
            self.sum = old_sum + (d - old_mean) * (d - self.mean)
        end
    end

    local avg = function()
        return self.mean
    end

    local sd = function()
        if self.count < 2 then return 0 end
        return math.sqrt(self.sum / (self.count - 1))
    end

    return {
        count       = count,
        add         = add,
        avg         = avg,
        sd          = sd
    }
end


local timers = {}
local function create_timer(name)
    local t = {start = nil, min = math.huge, max = -math.huge, stats = stats()}
    timers[name] = t
    return t
end


local function update_timer(ns, prefix, name)
    local t = timers[name]
    if not t then
        t = create_timer(name)
    end

    if prefix == "START_TIMER" then
        if t.start then
            io.stderr:write(string.format("%s '%s' with no matching END_TIMER\n", prefix, name))
        else
            t.start = ns
        end
    elseif prefix == "END_TIMER" then
        if not t.start then
            io.stderr:write(string.format("%s '%s' with no matching START_TIMER\n", prefix, name))
        else
            local delta = ns - t.start
            if delta < t.min then t.min = delta end
            if delta > t.max then t.max = delta end
            t.stats.add(delta)
            t.start = nil
        end
    end
end


local fh = assert(io.open(arg[1]))
for line in fh:lines() do
    local ns, prefix, name = line:match("(%d+)\t%[debug%]\t(%u+_TIMER):(.+)")
    if ns then update_timer(ns, prefix, name) end
end

io.stdout:write("Timer\tCount\tMin\tMax\tAvg\tSD\n")
for k,v in pairs(timers) do
    io.stdout:write(string.format("%s\t%d\t%d\t%d\t%g\t%g\n", k, v.stats.count(), v.min, v.max, v.stats.avg(), v.stats.sd()))
end
