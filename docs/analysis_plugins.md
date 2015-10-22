## Analysis Plugins

### Required Lua Functions (called by Hindsight)

#### process_message

Called when a message matches the `message_matcher` filter.

*Arguments*
* none

*Return*
* status_code (number)
  - success (less than or equal to zero)
  - fatal error (greater than zero)
* status_message (optional: string) logged when the status code is less than zero

#### timer_event

Called when the `ticker_interval` timer expires.

*Arguments*
* ns (number) - nanosecond timestamp of the function call (it is actually `time_t * 1e9`
to keep the timestamp units consistent so it will only have a one second resolution)
* shutdown (bool) - true if timer_event is being called due to a shutdown

*Return*
* none

### Available C Functions (called from the plugin)

#### read_config

Provides access to the sandbox configuration variables.

*Arguments*
* key (string) - configuration key name

*Return*
* value (string, number, bool, table)

#### read_message

Provides access to the Heka message data. Note that both fieldIndex and arrayIndex are zero-based
(i.e. the first element is 0) as opposed to Lua's standard indexing, which is one-based.

*Arguments*
* variableName (string)
  * framed (returns the Heka message protobuf string including the framing header)
  * raw (returns the Heka message protobuf string)
  * Uuid
  * Type
  * Logger
  * Payload
  * EnvVersion
  * Hostname
  * Timestamp
  * Severity
  * Pid
  * Fields[*name*]
* fieldIndex (unsigned) - only used in combination with the Fields variableName
        use to retrieve a specific instance of a repeated field *name*; zero indexed
* arrayIndex (unsigned) - only used in combination with the Fields variableName
        use to retrieve a specific element out of a field containing an array; zero indexed

*Return*
* value (number, string, bool, nil depending on the type of variable requested)

#### decode_message

Converts a Heka protobuf encoded message string into a Lua table.

*Arguments*
* heka_pb (string) - Heka protobuf binary string

*Return*
* msg ( [Heka message table (array fields)](heka_message_table.md#array-based-message-fields))

#### inject_message

Creates a new Heka protocol buffer message using the contents of the specified Lua table
(overwriting whatever is in the payload buffer). `Logger`, `Hostname`, and `Pid` are set by
the infrastructure and cannot be overridden.

*Arguments*
* msg ([Heka message table](heka_message_table.md))
* offset (optional: string, number) - checkpoint offset to be returned in the `process_message` call.

*Return*
* none (throws an error if the table does not match the Heka message schema)

#### add_to_payload

Appends the arguments to the payload buffer for incremental construction of the final payload output
(`inject_payload` finalizes the buffer and sends the message to the infrastructure). This function
is a rename of the generic sandbox output function to improve the readability of the plugin code.

*Arguments*
* arg (number, string, bool, nil, supported userdata)

*Return*
* none (throws an error if arg is an unsupported type)

#### inject_payload

This is a wrapper function for `inject_message` that is included for backwards compatibility. The function
creates a new Heka message using the contents of the payload buffer (pre-populated with `add_to_payload`) and combined
with any additional payload_args passed here. The payload buffer is cleared after the injection. The `payload_type`
and `payload_name` arguments are two pieces of optional metadata stored is message fields. The resulting message is
structured like this:
```lua
msg = {
Timestamp   = <current time>
Uuid        = "<auto generated>"
Hostname    = "<Hindsight hostname>"
Logger      = "<plugin name>"
Type        = "inject_payload"
Pid         = <Hindsight PID>
Payload     = "<payload buffer contents>"
Fields      = {
    payload_type = "txt",
    payload_name = "",
}
```

*Arguments*

* payload_type (optional: string, default "txt") - describes the content type of the injected payload data
* payload_name (optional: string,  default "") - names the content to aid in downstream filtering
* arg3 (optional) -ame type restrictions as `add_to_payload`
* ...
* argN

*Return*
* none (throws an error if arg is an unsupported type)

#### Modes of Operation

##### Lock Step
* Receives one call to `process_message`, operates on the message, and returns success (0) or failure (-1)

##### Example simple counter plugin
```lua
-- cfg
filename 		= "counter.lua"
message_matcher = "TRUE"
ticker_interval = 5
thread 			= 0
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

local cnt = 0

function process_message()
    cnt = cnt + 1
    return 0
end

function timer_event()
    inject_payload("txt", "count", cnt)
end
```
