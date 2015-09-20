## Heka Stream Reader Module

Enables parsing of a framed Heka protobuf stream in a Lua sandbox. See: 
[Example of a Heka protobuf reader](input_plugin.md#example-of-a-heka-protobuf-stdin-reader)

### API

#### new

Creates a Heka stream reader.

```lua
local hsr = heka_stream_reader.new("stdin")

```

*Arguments*
* name (string) - name of the stream reader (used in the log)

*Return*
* hsr (userdata) - Heka stream reader or an error is thrown

### API Methods

#### find_message

Locates a Heka message within the stream.

```lua
local found, read, need = hsr:find_message(buf)

```

*Arguments*
* buf (string) - buffer containing a Heka protobuf stream

*Return*
* found (bool) - true if a message was found
* read (number) - number of bytes read so the offset can be tracked for checkpointing purposes
* need (number) - number of bytes needed to complete the message or fill the underlying buffer

#### read_message

Provides access to the Heka message data within the reader object. 

```lua
local ts = hsr:read_message("Timestamp")

```
See [read_message](analysis_plugins.md#read_message) for details.
