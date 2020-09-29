-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at https://mozilla.org/MPL/2.0/.

function process_message()
    assert(read_config("cfg_name") == "input")
    assert(read_config("filename") == "input.lua")
    assert(read_config("output_limit") == 1024 * 64)
    assert(read_config("memory_limit") ==  1024 * 1024 * 8)
    assert(read_config("instruction_limit") == 1000000)
    assert(read_config("ticker_interval") == 0)
    assert(read_config("preserve_data") == false)
    assert(read_config("message_matcher") == nil)
    assert(read_config("thread") == nil)
    assert(read_config("async_buffer_size") == nil)

    return 0
end

function timer_event()
end

