## Output Plugins

### Required Lua Functions (called by Hindsight)

#### process_message

Called when a message matches the `message_matcher` filter.

*Arguments*
* sequence_id (optional: lightuserdata) - pass in when `async_buffer_size` is configured

*Return*
* status_code (number) - see the [Modes of Operation](#modes-of-operation) for a detail explanation
of the return codes
  - fatal error (greater than zero)
  - success (0)
  - non fatal failure (-1)
  - skip (-2)
  - retry (-3)
  - batching (-4)
  - async output (-5)
* status_message (optional: string) logged when the status code is -1

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

Provides access to the Heka message data. See [read_message](analysis_plugins.md#read_message) for details.

#### decode_message

Converts a Heka protobuf encoded message string into a Lua table.

*Arguments*
* heka_pb (string) - Heka protobuf binary string

*Return*
* msg ([Heka message table (array fields)](heka_message_table.md#array-based-message-fields)) or an error is thrown

#### encode_message

Returns a Heka protocol buffer message using the contents of the specified Lua table.
Note: this operation uses the internal output buffer so it is goverened by the `output_limit`
configuration setting.

*Arguments*
* msg ([Heka message table](heka_message_table.md)
* framed (bool default: false) A value of true includes the framing header

*Return*
* heka_pb (string) - Heka protobuf binary string, framed as specified or an error is thrown

#### batch_checkpoint_update

Available only when `async_buffer_size` is not configured, this function advances the output checkpoint
when in batching mode. The standard use case is to call it from `timer_event` after successfully flushing
a batch on timeout/shutdown.

*Arguments*
* none

*Return*
* none

#### async_checkpoint_update

Available only when `async_buffer_size` is configured, this function advances the output checkpoint
and optionally reports the number of failures that occured.

*Arguments*
* sequence_id (lightuserdata) - sequence_id for the message that was just successfully delivered/acknowledged
* failures (optional: integer) - number of failures that occured in the asynchronus processing (added to the failure count)

*Return*
* none (throws an error on invalid arg types)

#### Modes of Operation

##### Lock Step

* `process_message` operates on the message and returns one of the following values:
  * success (0) - the message was successfully processed and the output checkpoint is advanced
  * failure (-1)  - the message was not successfully processed
    * the failure count is incremented
    * any optional error message is written to the log
    * the message is skipped
    * the checkpoint is advanced
  * skip (-2) - the message was intentionally not processed and the checkpoint is advanced
  * retry (-3) - the message was not successfully processed and Hindsight will call `process_message`
  again, with the same message, after a one second delay

##### Example Payload Output

```lua
-- cfg
filename        = "inject_payload.lua"
message_matcher = "Type == 'inject_payload'"
ticker_interval = 0
thread          = 0

--location where the payload is written
output_dir      = "/tmp"
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "io"
require "string"

local output_dir = read_config("output_dir") or "/tmp"

function process_message()
    local pt = read_message("Fields[payload_type]")
    if type(pt) ~= "string" then return -1, "invalid payload_type" end

    local pn = read_message("Fields[payload_name]") or ""
    if type(pn) ~= "string" then return -1, "invalid payload_name" end

    local logger = read_message("Logger") or ""

    pn = string.gsub(pn, "%W", "_")
    pt = string.gsub(pt, "%W", "_")
    logger = string.gsub(logger, "%W", "_")

    local fn = string.format("%s/%s.%s.%s", output_dir, logger, pn, pt)
    local fh, err = io.open(fn, "w")
    if err then return -1, err end

    local payload = read_message("Payload") or ""
    fh:write(payload)
    fh:close()
    return 0
end

function timer_event(ns)
    -- no op
end
```

##### Batching

* `process_message` batches the message/transformation in memory or on disk and returns one of the following values:
  * batching (-4) - the message was successfully added to the batch
  * failure (-1) - the message cannot be batch
    * the failure count is incremented
    * any optional error message is written to the log
    * the message is skipped
  * skip (-2) - the message was intentionally not added to the batch
  * retry (-3) - the message was not successfully added to the batch and Hindsight will call `process_message`
  again, with the same message, after a one second delay
  * success (0) - the batch has been successfully committed and the output checkpoint is advanced to the most recent message

##### Example Postgres Output

```lua
-- cfg
filename = "postgres.lua"
message_matcher = "Type == 'logfile'"
memory_limit = 0
ticker_interval = 60

buffer_max = 1000
db_config = {
    host = "example.com",
    port = 5432,
    name = "dev",
    user = "test",
    _password = "testpw",
}
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "os"
require "string"
require "table"

local driver = require "luasql.postgres"

local ticker_interval = read_config("ticker_interval")
local db_config       = read_config("db_config") or error("db_config must be set")
local buffer_max      = read_config("buffer_max") or 1000
assert(buffer_max > 0, "buffer_max must be greater than zero")

local env = assert(driver.postgres())
local con, err = env:connect(db_config.name, db_config.user, db_config._password, db_config.host, db_config.port)
assert(con, err)

local buffer = {}
local buffer_len = 0
local table_name = "test_table"
local MAX_LENGTH = 65535
local columns = {
--   column name        field name                  field type      field length
    {"msg_Timestamp"    ,"Timestamp"                ,"TIMESTAMP"    ,nil},
    {"payload"          ,"Payload"                  ,"VARCHAR"      ,MAX_LENGTH},
    {"sourceName"       ,"Fields[sourceName]"       ,"VARCHAR"      ,30},
    {"sourceVersion"    ,"Fields[sourceVersion]"    ,"VARCHAR"      ,12},
    {"submissionDate"   ,"Fields[submissionDate]"   ,"DATE"         ,nil},
    {"sampleId"         ,"Fields[sampleId]"         ,"SMALLINT"     ,nil},
}

local function make_create_table()
    local pieces = {"CREATE TABLE IF NOT EXISTS ", table_name, " ("}
    for i, c in ipairs(columns) do
        table.insert(pieces, c[1])
        table.insert(pieces, " ")
        table.insert(pieces, c[3])
        if c[4] ~= nil then
            table.insert(pieces, "(")
            table.insert(pieces, c[4])
            table.insert(pieces, ")")
        end
        if c[4] == MAX_LENGTH then
            table.insert(pieces, " ENCODE LZO")
        end
        if i < #columns then
            table.insert(pieces, ", ")
        end
    end
    table.insert(pieces, ")")
    return table.concat(pieces)
end
assert(con:execute(make_create_table()))

local function bulk_load()
    local cnt, err = con:execute(table.concat(buffer))
    if not err then
        buffer = {}
        buffer_len = 0
    else
        return err
    end
end

local function esc_str(v)
    if v == nil then return "NULL" end

    if type(v) ~= "string" then v = tostring(v) end
    if string.len(v) > MAX_LENGTH then
        v = "TRUNCATED:" .. string.sub(v, 1, MAX_LENGTH - 10)
    end

    local escd = con:escape(v)
    if not escd then return "NULL" end

    return string.format("'%s'", escd)
end

local function esc_num(v)
    if v == nil then return "NULL" end
    if type(v) ~= "number" then return esc_str(v) end
    return tostring(v)
end

local function esc_ts(v)
    if v == nil then return "NULL" end
    if type(v) ~= "number" then return esc_str(v) end
    local seconds = v / 1e9
    return table.concat({"(TIMESTAMP 'epoch' + ", seconds, " * INTERVAL '1 seconds')"})
end

local function make_insert(sep)
    local pieces = {sep, "("}
    for i=1,#columns do
        if i > 1 then
            table.insert(pieces, ",")
        end
        local col = columns[i]
        if col[3] == "TIMESTAMP" then
            table.insert(pieces, esc_ts(read_message(col[2])))
        elseif col[3] == "SMALLINT" then
            table.insert(pieces, esc_num(read_message(col[2])))
        else
            table.insert(pieces, esc_str(read_message(col[2])))
        end
    end
    table.insert(pieces, ")")
    return table.concat(pieces)
end

local last_load = 0
function process_message()
    local sep = ","
    if buffer_len == 0 then
        buffer_len = buffer_len + 1
        buffer[buffer_len] = string.format("INSERT INTO %s VALUES", table_name)
        sep = " "
    end
    buffer_len = buffer_len + 1
    buffer[buffer_len] = make_insert(sep)

    if buffer_len - 1 >= buffer_max then
        local err = bulk_load()
        if err then
            buffer[buffer_len] = nil
            buffer_len = buffer_len - 1
            return -3, err
        else
            last_load = os.time()
            return 0
        end
    end
    return -4
end

function timer_event(ns, shutdown)
    if buffer_len > 1 and (shutdown or last_load + ticker_interval <= ns / 1e9)then
        local err = bulk_load()
        if not err then
            batch_checkpoint_update()
        end
    end
end
```

##### Asynchronous

* `async_buffer_size` **MUST** be set.
* `process_message` is called with a sequence_id parameter and asynchronously sends the message/transformation
to the destination and returns one of the following values:
  * asynchronous (-5) - the message was successfully queued
  * failure (-1) - the message cannot be queue
    * the failure count is incremented
    * any optional error message is written to the log
    * the message is skipped
  * skip (-2) - the message was intentionally not queued
  * retry (-3) - the message was not successfully queued and Hindsight will call `process_message`
  again, with the same message, after a one second delay
* When an asynchronously sent message is acknowledged [async_checkpoint_update](#async_checkpoint_update)
**MUST** be called to advance the checkpoint to that specific message

##### Example Kafka Output

```lua
-- cfg
filename               = "kafka.lua"
message_matcher        = "TRUE"
output_limit           = 8 * 1024 * 1024
brokers                = "localhost:9092"
ticker_interval        = 60
async_buffer_size      = 20000

topic_constant = "test"
producer_conf = {
    ["queue.buffering.max.messages"] = async_buffer_size,
    ["batch.num.messages"] = 200,
    ["message.max.bytes"] = output_limit,
    ["queue.buffering.max.ms"] = 10,
    ["topic.metadata.refresh.interval.ms"] = -1,
}
```

```lua
-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "kafka.producer"
require "kafka.topic"

local brokers = read_config("brokers") or error("brokers must be set")
local topic_constant = read_config("topic_constant")
local topic_variable = read_config("topic_variable") or "Logger"
local producer_conf = read_config("producer_conf")

local producer = kafka.producer.new(brokers, producer_conf)
local topics = {}

function process_message(sequence_id)
    local var = topic_constant
    if not var then
        var = read_message(topic_variable) or "unknown"
    end

    local topic = topics[var]
    if not topic then
        topic = kafka.topic.new(producer, var)
        topics[var] = topic
    end

    producer:poll() -- calls async_checkpoint_update
    local ret = topic:send(0, read_message("raw"), sequence_id)

    if ret ~= 0 then
        if ret == 105 then
            return -3 -- queue full retry
        elseif ret == 90 then
            return -1 -- message too large
        elseif ret == 2 then
            error("unknown topic: " .. var)
        elseif ret == 3 then
            error("unknown partition")
        end
    end
    return -5 -- asynchronous checkpoint management
end

function timer_event(ns)
    producer:poll() -- calls async_checkpoint_update
end
```
