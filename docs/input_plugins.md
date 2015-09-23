## Input Plugins

### Required Lua Functions (called by Hindsight)

#### process_message

Entry point for message creation.

*Arguments*
* offset (string or number) - value of the last offset value passed into `inject_message`

*Return*
* status_code (number)
  - success (less than or equal to zero)
  - fatal error (greater than zero)
* status_message (optional: string) logged when the status code is less than zero

### Available C Functions (called from the plugin)

#### read_config

Provides access to the sandbox configuration variables.

*Arguments*
* key (string) - configuration key name

*Return*
* value (string, number, bool, table)

#### decode_message

Converts a Heka protobuf encoded message string into a Lua table.

*Arguments*
* heka_pb (string) - Heka protobuf binary string

*Return*
* msg ([Heka message table (array fields)](heka_message_table.md#array-based-message-fields))

#### inject_message

Sends a Heka protocol buffer message into Hindsight.

*Arguments*
* msg ([Heka message table](heka_message_table.md), [Heka stream reader](heka_stream_reader.md) or Heka protobuf string)
* offset (optional: string, number) - checkpoint offset to be returned in the `process_message` call

*Return*
* none - throws an error on invalid input

### Modes of Operation

#### Run Once
* Set the `ticker_interval` to zero and return from `process_message` when you are done.
* The `instruction_limit` configuration can be set if desired.

##### Example startup ping
```lua
-- cfg
-- send a simple 'hello' messages every time Hindsight is started
filename = "hello.lua"
ticker_interval = 0
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

function process_message()
    inject_message({Type = "hello"})
    return 0
end

```

#### Polling

* Set the `ticker_interval` greater than zero and non fatally (<=0) return from `process_message`,
  when the ticker interval expires `process_message` will be called again.
* The `instruction_limit` configuration can be set if desired.

##### Example startup ping
```lua
-- cfg
filename = "uptime.lua"
ticker_interval = 60
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"

local msg = {
Type        = "uptime",
Payload     = "",
}

function process_message()
    local fh = io.popen("uptime")
    if fh then
        msg.Payload = fh:read("*a")
        fh:close()
    else
        return -1, "popen failed"
    end
    if msg.Payload then inject_message(msg) end
    return 0
end

```

#### Continuous

* Don't return from `process_message`.
* The `instruction_limit` configuration **MUST** be set to zero.

##### Example of a Heka protobuf stdin reader

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"
require "heka_stream_reader"

local hsr = heka_stream_reader.new("stdin")

function process_message()
    local found, read, need = false, 0, 8192
    while true do
        local buf = io.stdin:read(need)
        if not buf then break end

        repeat
            found, read, need = hsr:find_message(buf)
            if found then
                inject_message(hsr)
            end
            buf = nil
        until not found
    end
    return 0
end

```
