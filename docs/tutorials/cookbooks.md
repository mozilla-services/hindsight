# Common Configuration Cookbooks

## Inputs

See: [Using Decoders with Input Plugins](using_decoders_with_input_plugins.md)

### Syslog
Covered in [Using Decoders with Input Plugins](using_decoders_with_input_plugins.md)

### Auditd Log
```
filename = "tail.lua"
ticker_interval = 1

follow = "name"
input_filename = "/var/log/audit.log"
decoder_module = "lpeg.logfmt"
send_decode_failures = true
```

### Nginx Access Log
```lua
filename        = "tail.lua"
ticker_interval = 1

follow          = "name"
input_filename  = "/var/log/nginx/access.log"

log_format = '$remote_addr - $remote_user [$time_local] "$request" $status $body_bytes_sent "$http_referer" "$http_user_agent"',
decoder_module  = { { {"common_log_format#build_nginx_grammar", log_format}, nil}}
send_decode_failures = true
```

### Nginx Error Log
```lua
filename        = "tail.lua"
ticker_interval = 1

follow          = "name"
input_filename  = "/var/log/nginx/error.log"
decoder_module  = "lpeg.common_log_format#nginx_error_grammar"
send_decode_failures = true
```

### MySQL Slow Query Log
```lua
filename        = "tail.lua"
ticker_interval = 1

follow          = "name"
input_filename  = "/var/log/mysql/slow-query.log"
delimiter       = "^# User@Host:"
decoder_module  = "lpeg.mysql#slow_query_grammar"
send_decode_failures = true
```

### Apache Access Log
```lua
filename        = "tail.lua"
ticker_interval = 1

follow          = "name"
input_filename  = "/var/log/apache/access.log"
log_format      = '%h %l %u %t "%r" %>s %b "%{Referer}i" "%{User-Agent}i"'
decoder_module  = { { {"common_log_format#build_apache_grammar", log_format}, nil}}
send_decode_failures = true
```

### Pfsense 2.2+
```lua
filename            = "udp.lua"
instruction_limit   = 0

address = "*"
port = 4514 -- since pfsense sends non-standard syslog message assign them to a different port

decoder_module = "decoders.syslog"
send_decode_failures = true

log_format = 'nginx: $remote_addr - $remote_user [$time_local] "$request" $status $body_bytes_sent "$http_referer" "$http_user_agent"'

decoders_syslog = {
  template = "<%PRI%>%TIMESTAMP% %syslogtag:1:32%%msg:::sp-if-no-1st-sp%%msg%",
  sub_decoders = {
    filterlog = "lpeg.bsd.filterlog",
    ["hostname.example.com"] = {
      { {"common_log_format#build_nginx_grammar", log_format}, nil}
    }
  }
}
```
