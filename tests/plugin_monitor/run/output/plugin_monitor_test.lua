function process_message()
    local n = tonumber(read_message("Payload"))
    if n < 0 then
        return -1
    end
    return 0
end

function timer_event ()
end
