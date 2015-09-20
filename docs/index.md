# Hindsight

## Overview

Hindsight is a C based data processing infrastructure based on the [lua sandbox]
(https://github.com/mozilla-services/lua_sandbox) project.  I have received several inquiries 
about a lighter weight and faster data pipeline with delivery guarantees to replace
[Heka](https://github.com/mozilla-services/heka).  Hindsight is that light weight skeleton around
the same lua sandbox offering 'at least once' delivery semantics.

So how much lighter and faster? see: [Performance](performance.md)

## Table of Contents

* [Architecture](architecture.md)
* [Configuration](configuration.md)
* [Message Matcher](message_matcher.md)
* [Input Plugins](input_plugins.md)
  * [Heka Stream Reader](heka_stream_reader.md)
* [Analysis Plugins](analysis_plugins.md)
* [Output Plugins](output_plugins.md)
