# Hindsight

## Overview

Hindsight is a C based data processing infrastructure based on the
[lua sandbox](https://github.com/mozilla-services/lua_sandbox) project. I have
received several inquiries about a lighter weight and faster data pipeline with
delivery guarantees to replace [Heka](https://github.com/mozilla-services/heka).
Hindsight is that light weight skeleton around the same lua sandbox offering 'at
least once' delivery semantics.

So how much lighter and faster? see: [Performance](performance.md)

## Table of Contents

* [Architecture](architecture.md)
* [Configuration](configuration.md)
* [Command Line Version](hindsight_cli.md)
* [Heka Sandbox Documentation](https://mozilla-services.github.io/lua_sandbox/heka/index.html)
* [Sandbox Extensions Documentation](https://mozilla-services.github.io/lua_sandbox_extensions/index.html)

### Differences from Heka

1. [Sandbox API Differences](https://mozilla-services.github.io/lua_sandbox/heka/index.html#sandbox-api-changes-from-the-go-heka-sandbox)
1. The message matcher now uses Lua string match patterns instead of RE2
expressions.
1. Looping messages in Heka, injecting messages back to an earlier point in the
pipeline, especially from an untrusted sandbox (analysis plugin) has always been
a bad idea so it was removed. Most looping requirements can be flattened out:
i.e., alerting can be handled in the output plugins, aggregation/sessionization
can be handled in the inputs. However, inject_message was added to the output
plugin in 0.16.0 so this functionality is now available in a more
controlled/opt-in way when it is needed.
1. Splitters/Decoders/Encoders no longer exist they simply become part of the
input/output plugins respectively. The common functionality should now be broken
out into Lua modules to allow for re-use in different plugins.
1. Checkpoint are all managed by the Hindsight infrastructure (so much of the
burden is removed from the plugin writer, this also alters the plugin API
slightly and should greatly lower the checkpoint related IOPS when compared
to Heka).
