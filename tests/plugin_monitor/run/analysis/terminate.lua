
function process_message()
    return 0
end

local cnt = 0
function timer_event()
    cnt = cnt + 1
    if cnt == 6 then error"boom" end
end
