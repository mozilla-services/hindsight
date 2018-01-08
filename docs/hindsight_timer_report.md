# Hindsight Timer Report

A command line utility that generates a report of timer measurements from the
Hindsight log. The utility extracts matched sets of start/end prefixes and then
computes the count, min, max, average and standard deviation outputting the
results to stdout as a tsv. The prefix consists of
(START_TIMER|END_TIMER):<timer_name> and is output with the print function
(only available when Hindsight has debug logging enabled).

```
usage: hindsight_timer_report <hindsight log file>
```

## Code
```lua
print("START_TIMER:foo")
--- do foo
print("END_TIMER:foo")
```

## Log
```
1515017287796327572 [debug] START_TIMER:foo
1515017287796347586 [debug] END_TIMER:foo
````

## Output
| Timer | Count | Min | Max | Avg | SD
|---|---|---|---|---|---
foo | 1 | 20014 | 20014 | 20014 | 0
