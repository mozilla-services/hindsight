## Heka Stream Reader Module

Enables parsing of a framed Heka protobuf stream in a Lua sandbox. See: 
[Example of a Heka protobuf reader](input_plugins.md#example-of-a-heka-protobuf-stdin-reader)

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
local found, consumed, need = hsr:find_message(buf)

```

*Arguments*
* buf (string, userdata (FILE*)) - buffer containing a Heka protobuf stream data or a userdate file object
* decode (bool default: true) - true if the framed message should be protobuf decoded

*Return*
* found (bool) - true if a message was found
* consumed (number) - number of bytes consumed so the offset can be tracked for checkpointing purposes
* need/read (number) - number of bytes needed to complete the message or fill the underlying buffer
  or in the case of a file object the number of bytes added to the buffer

#### decode_message

Converts a Heka protobuf encoded message string into a stream reader representation.  Note: this operation
clears the internal stream reader buffer.

*Arguments*
* heka_pb (string) - Heka protobuf binary string

*Return*
* none - throws an error on failure

#### read_message

Provides access to the Heka message data within the reader object. 

```lua
local ts = hsr:read_message("Timestamp")

```
See [read_message](analysis_plugins.md#read_message) for details.
