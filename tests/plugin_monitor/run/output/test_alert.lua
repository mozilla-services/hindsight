require "io"
require "string"
i = 1
alerts = {}
function process_message()
    local raw = read_message("raw")
    local msg = decode_message(raw)
    alerts[i] = {msg.Fields[2].value[1], msg.Payload}
    i = i + 1
    return 0
end

ticks = 0
function timer_event(ns)
    local cnt = #alerts
    if ticks == 15 or cnt == 2 then
        if cnt == 2 then
            if not string.match(alerts[1][2], "10%% of recent messages failed") then
                error("failed alert 1: " .. tostring(alerts[1][2]))
            elseif not string.match(alerts[2][2], "terminate.lua:9: boom") then
                error("failed alert 2: " .. tostring(alerts[2][2]))
            end
        else
            error("fail " .. cnt)
        end
        local fh = assert(io.open(read_config("sandbox_load_path") .. "/input/plugin_tsv.off", "w+"))
        fh:close()
    end
    ticks = ticks + 1
end
