## Heka Message Table

### Hash Based Message Fields

```lua
{
Uuid        = "data",               -- ignored if not 16 byte raw binary UUID
Logger      = "nginx",              -- ignored by analysis plugins (the plugin name is used instead)
Hostname    = "example.com",        -- ignored by analysis plugins (the Hindsight hostname name is used instead)
Timestamp   = 1e9,
Type        = "TEST",              
Payload     = "Test Payload",
EnvVersion  = "0.8",
Pid         = 1234,                 -- ignored by analysis plugins (the Hindsight PID is used instead)
Severity    = 6,
Fields      = {
            http_status     = 200,  -- encoded as a double
            request_size    = {value=1413, value_type=2, representation="B"} -- encoded as an integer
            }
}
```

### Array Based Message Fields
```lua
{
-- Message headers are the same as above
Fields      = {
            {name="http_status" , value=200}, -- encoded as a double
            {name="request_size", value=1413, value_type=2, representation="B"} -- encoded as an integer
            }
}
```
