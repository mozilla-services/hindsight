## Performance

It is very preliminary but I setup a test using some Nginx logs. The logs were split into three files each containing 
1,002,646 log lines. The test consists of parsing the Nginx logs, converting them to a Heka protobuf stream, and writing 
them back to disk.  The configurations were setup to be as equivalent as posssible (single output, similar flush 
intervals, and individual readers for each file in the concurrent test). The configs can be found here: 
https://github.com/trink/hindsight/tree/master/benchmarks.  

### Test Hardware

* Lenovo x230 Thinkpad
* Memory 8GB
* Processor Intel Core i7-3520M CPU @ 2.90GHz x 4
* Disk SSD

### Test 1 - Processing a single log file

#### Hindsight (0.8)

* RSS: 3276
* VIRT: 253148
* SHR: 1140
* Processing time: 9.88 seconds
* Log Lines/sec: 101482

#### Heka (0.9)

* RSS: 39764
* VIRT: 662204
* SHR: 5240
* Processing time: 63 seconds
* Log Lines/sec: 15,915

#### Summary

* Approximately 12x less resident memory
* Approximately 2.5 less virtual memory
* Over 6x the throughput
* Hindsight (kill -9) Caused some corruption in the stream but no messages were lost and 3 were duplicated.
    * Error unmarshalling message at offset: 275181251 error: proto: field/encoding mismatch: wrong type for field
    * Corruption detected at offset: 275181251 bytes: 317
    * Processed: 1002650, matched: 1002649 messages

* Heka (kill -9) record count: 1,002,595 so at least 51 records were lost.

### Test 2 - Processing all three log files concurrently

#### Hindsight (0.8)

* RSS: 5488
* VIRT: 327872
* SHR: 1128
* Processing time: 17.84 seconds
* Log Lines/sec: 168,606

#### Heka (0.9)

* RSS: 42060
* VIRT: 801432
* SHR: 5256
* Processing time: 198 second
* Log Lines/sec: 15,192  # actually slower than processing them sequentially

#### Summary

* Approximately 7.7x less resident memory
* Approximately 2.4x less virtual memory
* Over 11x the throughput
