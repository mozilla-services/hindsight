local cnt = 0

function process_message()
    cnt = cnt + 1
    return 0
end

function timer_event()
    inject_payload("txt", "count", cnt)
end
