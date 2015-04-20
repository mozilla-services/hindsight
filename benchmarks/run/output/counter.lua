local cnt = 0

function process_message()
    --error("boom")
    cnt = cnt + 1
    return 0
end

function timer_event()
        print("count", cnt)
end
