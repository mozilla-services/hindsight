# Using Decoders with Input Plugins

This guide walks through how input plugins and decoders are used in the context
of ingesting syslogs. Each section starts with a description followed by a
configuration, input file and the resulting output. The configurations are
evolved to demonstrate the available functionality.

For all of the examples in this guide the
[file](https://mozilla-services.github.io/lua_sandbox_extensions/heka/sandboxes/heka/input/file.html)
input is used. In a real world environment one would most likely use the
[tail](https://mozilla-services.github.io/lua_sandbox_extensions/lfs/sandboxes/heka/input/tail.html),
[udp](https://mozilla-services.github.io/lua_sandbox_extensions/socket/sandboxes/heka/input/udp.html)
or
[tcp](https://mozilla-services.github.io/lua_sandbox_extensions/socket/sandboxes/heka/input/tcp.html)
input plugins. They can be swapped in at any time as the [syslog
decoder](https://mozilla-services.github.io/lua_sandbox_extensions/syslog/io_modules/decoders/syslog.html)
configurations discussed here are independent from the input plugin being used.

## Definition of Terms

* [Input Plugin](http://mozilla-services.github.io/lua_sandbox/heka/input.html) -
  A piece of Lua code that loads data into Hindsight
* Decoders/Sub-decoders - Zero or more stages during the input process where the
  raw data is transformed into a more useful form
* [Decoder Module](https://mozilla-services.github.io/lua_sandbox_extensions/#Decoder_API_Convention) -
  A Lua module (installed piece of code) used by input plugins to transform the
  raw input data
* Grammar Module - A Lua module used by input plugins to transform the raw input
  data using only an [LPeg](http://www.inf.puc-rio.br/~roberto/lpeg/) grammar
* printf Module - A Lua module with pre-defined
  [printf parsers](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/printf.html#build_grammar)
  in an exported
  [printf_messsages](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/printf.html#load_messages)
  variable
* Ad-hoc Parser - A data parser defined in the configuration instead of a
  decoder/grammar module


----
## Basic Syslog Decoder Module

This is an example of a basic syslog decoder that extracts the header
information (timestamp, hostname, pid, programname) and the message string. The
only thing required to configure the basic syslog decoder is the template
configuration specified in the
[rsyslog.conf](http://rsyslog-5-8-6-doc.neocities.org/rsyslog_conf_templates.html)
file. The template below is the RSYSLOG_TraditionalFileFormat without the new
line at the end since that is consumed by the file input. The Uuid (type 4
random) and Logger (input plugin name) are added to the output by Hindsight.

### Configuration

```lua
filename = "file.lua"
input_filename = "syslog.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",
}
```

### Input
```
Feb 13 14:25:19 ubuntu sshd[7192]: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
```

### Output
```rst
:Uuid: 0c1e9271-6b7c-451e-aa05-63cdaf14cf10
:Timestamp: 2018-02-13T14:25:19.000000000Z
:Type: <nil>
:Logger: input.syslog
:Severity: 7
:Payload: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
:EnvVersion: <nil>
:Pid: 7192
:Hostname: ubuntu
:Fields:
    | name: programname type: 0 representation: <nil> value: sshd
```

----
## Ad-hoc printf Decoder

This is an example of a syslog decoder using an ad-hoc printf parser.  The key
in the sub_decoders hash is the programname from the syslog and its value is an
array containing a
[printf grammar specification](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/printf.html#build_grammar)
with a nil transformation table.

### Configuration
```lua
filename = "file.lua"
input_filename = "adhoc.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    foo = {
      { {"%s:%lu: invalid line", "path", "linenum"}, nil},
    },
  },
}
```

### Input
```
Jan 23 08:50:03 ubuntu foo[1234]: /tmp/input.tsv:23: invalid line
```

### Output
```rst
:Uuid: ff56099b-51d0-4ca5-af70-7509eaa6450a
:Timestamp: 2018-01-23T08:50:03.000000000Z
:Type: <nil>
:Logger: input.adhoc
:Severity: 7
:Payload: /tmp/input.tsv:23: invalid line
:EnvVersion: <nil>
:Pid: 1234
:Hostname: ubuntu
:Fields:
    | name: linenum type: 3 representation: <nil> value: 23
    | name: path type: 0 representation: <nil> value: /tmp/input.tsv
    | name: programname type: 0 representation: <nil> value: foo
```

----
## Pre-defined printf Module Decoder

This is an example of a syslog decoder using a printf parser library. The value
in the printf_messages array is the name of a module exporting another
[printf_messsage](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/printf.html#load_messages)
array that is imported for use here. The key in the sub_decoders hash is the
programname from the syslog and its value is an array containing a sample
message that the user would like parsed.

### Configuration
```lua
filename = "file.lua"
input_filename = "syslog.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  printf_messages = {
   "lpeg.openssh_portable",
  },

  sub_decoders = {
    sshd = {
      "Accepted publickey for foobar from 192.168.1.1 port 4567 ssh2",
    },
  },
}
```

### Input
```
Feb 13 14:25:19 ubuntu sshd[7192]: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
```

### Output
```rst
:Uuid: 3bde3689-26a7-484f-a893-a7d6718be4c6
:Timestamp: 2018-02-13T14:25:19.000000000Z
:Type: <nil>
:Logger: input.printf
:Severity: 7
:Payload: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
:EnvVersion: <nil>
:Pid: 7192
:Hostname: ubuntu
:Fields:
    | name: ssh_remote_port type: 3 representation: <nil> value: 4242
    | name: method type: 0 representation: <nil> value: publickey
    | name: user type: 0 representation: <nil> value: foobar
    | name: ssh_remote_ipaddr type: 0 representation: ipv4 value: 173.239.228.74
    | name: programname type: 0 representation: <nil> value: sshd
    | name: extra type: 0 representation: <nil> value:
    | name: authmsg type: 0 representation: <nil> value: Accepted
```

----
## Pre-defined printf Module Decoder with Transformation

This is an example of a syslog decoder using a printf parser library. The value
in the printf_messages array is the name of a module exporting another
[printf_messsage](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/printf.html#load_messages)
array that is imported for use here. The key in the sub_decoders hash is the
programname from the syslog and its value is an array containing a sample
message that the user would like parsed and a transformation table. The
transformation table is keyed by the field name to transform and the value is
the transformation funtion to apply. The
[maxminddb_heka](https://mozilla-services.github.io/lua_sandbox_extensions/maxminddb/io_modules/maxminddb/heka.html)
table is the configuration to control the transformation.

### Configuration
```lua
filename = "file.lua"
input_filename = "syslog.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  printf_messages = {
   "lpeg.openssh_portable",
  },

  sub_decoders = {
    sshd = {
      {"Accepted publickey for foobar from 10.11.12.13 port 4242 ssh2", {ssh_remote_ipaddr = "maxminddb.heka#add_geoip"}},
    },
  },
}

maxminddb_heka = {
    databases = {
        ["GeoIP2-City.mmdb"] = {
            _city = {"city", "names", "en"},
            _country = {"country", "iso_code"}
        },
    },
}
```

### Input
```
Feb 13 14:25:19 ubuntu sshd[7192]: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
```

### Output
```rst
:Uuid: 5018c7e5-e51d-447f-aa84-a2f3a2d9ad11
:Timestamp: 2018-02-13T14:25:19.000000000Z
:Type: <nil>
:Logger: input.printf_transform
:Severity: 7
:Payload: Accepted publickey for foobar from 173.239.228.74 port 4242 ssh2
:EnvVersion: <nil>
:Pid: 7192
:Hostname: ubuntu
:Fields:
    | name: ssh_remote_port type: 3 representation: <nil> value: 4242
    | name: method type: 0 representation: <nil> value: publickey
    | name: user type: 0 representation: <nil> value: foobar
    | name: extra type: 0 representation: <nil> value:
    | name: authmsg type: 0 representation: <nil> value: Accepted
    | name: ssh_remote_ipaddr_city type: 0 representation: <nil> value: San Jose
    | name: ssh_remote_ipaddr_country type: 0 representation: <nil> value: US
    | name: programname type: 0 representation: <nil> value: sshd
    | name: ssh_remote_ipaddr type: 0 representation: ipv4 value: 173.239.228.74
```

----
## Lua Grammar Module Decoder

This is an example of a syslog decoder using a grammar module. The key in the
sub_decoders hash is the programname from the syslog and its value is the module
name and optional grammar name reference to apply to the matching messages.

### Configuration
```lua
filename = "file.lua"
input_filename = "grammar_module.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    someapp = "lpeg.logfmt", -- shorthand for "lpeg.logfmt#grammar"
  },
}
```

### Input
```
Feb 14 19:20:21 ubuntu someapp[3453]: foo=bar a=14 baz="hello kitty" cool%story=bro f %^asdf ip=173.239.228.74
```

### Output
```rst
Uuid: 3cafda9a-261f-48b5-a288-cc6c0783ba3e
:Timestamp: 2018-02-14T19:20:21.000000000Z
:Type: <nil>
:Logger: input.grammar_module
:Severity: 7
:Payload: <nil>
:EnvVersion: <nil>
:Pid: 3453
:Hostname: ubuntu
:Fields:
    | name: a type: 0 representation: <nil> value: 14
    | name: foo type: 0 representation: <nil> value: bar
    | name: cool%story type: 0 representation: <nil> value: bro
    | name: baz type: 0 representation: <nil> value: hello kitty
    | name: %^asdf type: 4 representation: <nil> value: true
    | name: programname type: 0 representation: <nil> value: someapp
    | name: f type: 4 representation: <nil> value: true
    | name: ip type: 0 representation: <nil> value: 173.239.228.74
```

----
## Lua Grammar Module Decoder with Transformation

This is an example of a syslog decoder using a grammar module. The key in the
sub_decoders hash is the programname from the syslog and its value an array with
a grammar module entry and a transformation table.

### Configuration
```lua
filename = "file.lua"
input_filename = "grammar_module.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    someapp = {
    { {"lpeg.logfmt"}, {ip = "maxminddb.heka#add_geoip"}},
    },
  },
}

maxminddb_heka = {
  databases = {
    ["GeoIP2-City.mmdb"] = {
        _city = {"city", "names", "en"},
        _country = {"country", "iso_code"}
    },
  }
}
```

### Input
```
Feb 14 19:20:21 ubuntu someapp[3453]: foo=bar a=14 baz="hello kitty" cool%story=bro f %^asdf ip=173.239.228.74
```

### Output
```rst
:Uuid: eade849f-e90f-4cc7-a817-265545d4e522
:Timestamp: 2018-02-14T19:20:21.000000000Z
:Type: <nil>
:Logger: input.grammar_transform
:Severity: 7
:Payload: foo=bar a=14 baz="hello kitty" cool%story=bro f %^asdf ip=173.239.228.74
:EnvVersion: <nil>
:Pid: 3453
:Hostname: ubuntu
:Fields:
    | name: a type: 0 representation: <nil> value: 14
    | name: baz type: 0 representation: <nil> value: hello kitty
    | name: ip type: 0 representation: <nil> value: 173.239.228.74
    | name: f type: 4 representation: <nil> value: true
    | name: ip_country type: 0 representation: <nil> value: US
    | name: ip_city type: 0 representation: <nil> value: San Jose
    | name: %^asdf type: 4 representation: <nil> value: true
    | name: programname type: 0 representation: <nil> value: someapp
    | name: cool%story type: 0 representation: <nil> value: bro
    | name: foo type: 0 representation: <nil> value: bar
```

----
## Lua Grammar Module Decoder with Match Arguments

This is an example of a syslog decoder using a grammar module that takes
additional match arguments. The key in the sub_decoders hash is the programname
from the syslog and its value an array with a grammar module entry and nil
transformation table.

### Configuration
```lua
filename = "file.lua"
input_filename = "grammar_module.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    someapp = {
        { {"syslog_docs#demo", "www.example.com"}, nil},
    },
  },
}
```

### Input
```
Feb 14 19:20:21 ubuntu someapp[3453]: foo=bar a=14 baz="hello kitty" cool%story=bro f %^asdf ip=173.239.228.74
```

### Output
```rst
:Uuid: 0cde3ec6-b894-498e-aad6-abf3e4059342
:Timestamp: 2018-02-14T19:20:21.000000000Z
:Type: <nil>
:Logger: input.grammar_args
:Severity: 7
:Payload: foo=bar a=14 baz="hello kitty" cool%story=bro f %^asdf ip=173.239.228.74
:EnvVersion: <nil>
:Pid: 3453
:Hostname: ubuntu
:Fields:
    | name: programname type: 0 representation: <nil> value: someapp
    | name: real_hostname type: 0 representation: <nil> value: www.example.com
```

----
## Lua Grammar Builder Module Decoder

This is an example of a syslog decoder using a grammar module that dynamically
builds the needed grammar. The key in the sub_decoders hash is the programname
from the syslog and its value is an array containing a
[grammar building function](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/modules/lpeg/common_log_format.html#build_nginx_grammar)
and its required argument, the log_format configuration string. The
transformation table is not used in this example.

### Configuration
```lua
filename = "file.lua"
input_filename = "decoder_module.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

log_format = '$remote_addr - $remote_user [$time_local] "$request" $status $body_bytes_sent "$http_referer" "$http_user_agent"'

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    nginx  = {
        { {"lpeg.common_log_format#build_nginx_grammar", log_format}, nil},
    },
  },
}
```

### Input
```
Jan 23 08:50:02 ubuntu nginx[1234]: 127.0.0.1 - - [10/Feb/2014:08:46:41 -0800] "GET / HTTP/1.1" 304 0 "-" "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:26.0) Gecko/20100101 Firefox/26.0"
```

### Output
```rst
:Uuid: 4d2347c9-26b6-4c7d-a703-edbf9335ecb8
:Timestamp: 2018-01-23T08:50:02.000000000Z
:Type: <nil>
:Logger: input.function
:Severity: 7
:Payload: 127.0.0.1 - - [10/Feb/2014:08:46:41 -0800] "GET / HTTP/1.1" 304 0 "-" "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:26.0) Gecko/20100101 Firefox/26.0"
:EnvVersion: <nil>
:Pid: 1234
:Hostname: ubuntu
:Fields:
    | name: remote_user type: 0 representation: <nil> value: -
    | name: http_user_agent type: 0 representation: <nil> value: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:26.0) Gecko/20100101 Firefox/26.0
    | name: body_bytes_sent type: 3 representation: B value: 0
    | name: remote_addr type: 0 representation: ipv4 value: 127.0.0.1
    | name: time type: 3 representation: <nil> value: 1.392050801e+18
    | name: request type: 0 representation: <nil> value: GET / HTTP/1.1
    | name: programname type: 0 representation: <nil> value: nginx
    | name: http_referer type: 0 representation: <nil> value: -
    | name: status type: 3 representation: <nil> value: 304
```

----
## Lua Decoder Module

This is an example of a syslog decoder using a decoder module. The key in the
sub_decoders hash is the programname from the syslog and its value is the module
name to apply to the matching messages. The
[decoders_nginx_access](https://mozilla-services.github.io/lua_sandbox_extensions/lpeg/io_modules/decoders/nginx/access.html)
table is the configuration information for the specified decoder.  In this case
it is the log_format configuration value from the nginx.conf that produced this
syslog entry.


### Configuration
```lua
filename = "file.lua"
input_filename = "decoder_module.log"
send_decode_failures = true
decoder_module = "decoders.syslog"

decoders_syslog = {
  template = "%TIMESTAMP% %HOSTNAME% %syslogtag%%msg:::sp-if-no-1st-sp%%msg:::drop-last-lf%",

  sub_decoders = {
    nginx  = "decoders.nginx.access",
  },
}

decoders_nginx_access = {
  log_format = '$remote_addr - $remote_user [$time_local] "$request" $status $body_bytes_sent "$http_referer" "$http_user_agent"'
}
```

### Input
```
Jan 23 08:50:02 ubuntu nginx[1234]: 127.0.0.1 - - [10/Feb/2014:08:46:41 -0800] "GET / HTTP/1.1" 304 0 "-" "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:26.0) Gecko/20100101 Firefox/26.0"
```

### Output
```rst
:Uuid: 544eef18-d932-4928-a927-3059e95d8d33
:Timestamp: 2014-02-10T16:46:41.000000000Z
:Type: <nil>
:Logger: input.decoder_module
:Severity: 7
:Payload: <nil>
:EnvVersion: <nil>
:Pid: 1234
:Hostname: ubuntu
:Fields:
    | name: body_bytes_sent type: 3 representation: B value: 0
    | name: remote_addr type: 0 representation: ipv4 value: 127.0.0.1
    | name: http_referer type: 0 representation: <nil> value: -
    | name: status type: 3 representation: <nil> value: 304
    | name: request type: 0 representation: <nil> value: GET / HTTP/1.1
    | name: programname type: 0 representation: <nil> value: nginx
    | name: remote_user type: 0 representation: <nil> value: -
    | name: http_user_agent type: 0 representation: <nil> value: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:26.0) Gecko/20100101 Firefox/26.0
```
