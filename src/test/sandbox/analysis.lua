-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at https://mozilla.org/MPL/2.0/.

local function test_array(a)
    assert(type(a) == "table", type(a))
    assert(#a == 4)
    assert(a[1] == "string")
    assert(a[2] == 99)
    assert(a[3] == false)
    assert(a[4] == true)
end


local function test_hash(h)
    assert(type(h) == "table")
    assert(h.string == "string")
    assert(h.number == 99)
    assert(h["false"] == false)
    assert(h["true"] == true)
end


function process_message()
    assert(read_config("cfg_name") == "analysis")
    assert(read_config("filename") == "analysis.lua")
    assert(read_config("output_limit") == 77777)
    assert(read_config("memory_limit") == 88888)
    assert(read_config("instruction_limit") == 99999)
    assert(read_config("ticker_interval") == 17)
    assert(read_config("preserve_data"))
    assert(read_config("message_matcher") == "TRUE")
    assert(read_config("thread") == 1)
    assert(read_config("async_buffer_size") == nil)

    test_array(read_config("array"))
    test_hash(read_config("hash"))

    local nested = read_config("nested")
    assert(type(nested) == "table")
    test_array(nested.array)
    test_hash(nested.hash)

    return 0
end

function timer_event()
end

