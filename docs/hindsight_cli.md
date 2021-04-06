# Hindsight CLI

The command line interface version of Hindsight is designed for non-real-time
use (testing and reporting).  The usage is exactly the same as the hindsight
executable but the behavior is different.

```
usage: hindsight_cli <cfg> <log_level>
```

## Behavioral Changes Compared to Hindsight

1. It will initiate a shutdown when all inputs plugins have finished
1. It will not exit until all the data has been process by every plugin
1. The ticker_interval is driven by the timestamp in the data (greatest time
   seen) as opposed to the actual clock time i.e. if an hour of data is
   processed in 45 seconds a 60 second ticker_interval would trigger the
   timer_event function 60 times.
1. A second ctrl-c will abort the execution
1. The return value will be non-zero if the execution failed.  If the failure
   was due to plugin termination the return values will be or'ed together
   e.g., 6 indicates an input and analysis termination.
   * 1 system error
   * 2 input plugin termination
   * 4 analysis plugin termination
   * 8 output plugin termination

## Integration Test Example

[Mozilla Telemetry Integration Tests](https://github.com/mozilla-services/lua_sandbox_extensions/tree/main/moz_telemetry/tests/hindsight)
