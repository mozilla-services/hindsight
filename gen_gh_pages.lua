-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "os"
require "io"
require "string"


local function output_menu(output_dir, version)
    local fh = assert(io.open(string.format("%s/SUMMARY.md", output_dir), "w"))
    fh:write(string.format("* [Hindsight (%s)](README.md)\n\n", version))
    fh:write([[
* [Architecture](architecture.md)
* [Performance](performance.md)
* [Configuration](configuration.md)
* [Command Line Version](hindsight_cli.md)
* [Execution Profiling](hindsight_timer_report.md)
* [Heka Sandbox Documentation](http://mozilla-services.github.io/lua_sandbox/heka/index.html)
* [Sandbox Extensions Documentation](http://mozilla-services.github.io/lua_sandbox_extensions/index.html)

### Tutorials

* [Using Decoders with Input Plugins](tutorials/using_decoders_with_input_plugins.md)
]])
    fh:close()
end


local args = {...}
local function main()
    local output_dir = string.format("%s/gb-source", arg[3])
    local rv = os.execute(string.format("rsync -rav docs/ %s/", output_dir))
    if rv ~= 0 then error"rsync setup" end
    local fh = assert(io.open(string.format("%s/book.json", output_dir), "w"))
    fh:write([[{"plugins" : ["collapsible-menu", "navigator"]}]])
    fh:close()

    os.execute(string.format("cd %s;gitbook install", output_dir))
    os.execute(string.format("mv %s/index.md %s/README.md", output_dir, output_dir))
    output_menu(output_dir, args[1])
    os.execute(string.format("gitbook build %s", output_dir))
    local rv = os.execute(string.format("rsync -rav %s/_book/ %s/", output_dir, "gh-pages/"))
    if rv ~= 0 then error"rsync publish" end
end

main()
