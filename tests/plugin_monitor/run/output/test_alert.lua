require "os"
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
    if ticks == 5 then
        if (#alerts == 1 and alerts[1][2] == "10% of recent messages failed") then
            print("success")
        else
            print("fail " .. #alerts)
        end
        os.execute("kill -INT " .. read_config("Pid"))
    end
    ticks = ticks + 1
end
