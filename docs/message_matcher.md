### Message Matcher Syntax

The message matcher allows plugins to select which messages they want to consume
(see [Heka Message Structure](https://hekad.readthedocs.org/en/latest/message/index.html))

#### Examples

*  Type == "test" && Severity == 6
*  (Severity == 7 || Payload == "Test Payload") && Type == "test"
*  Fields[foo] != "bar"
*  Fields[foo][1][0] == "alternate"
*  Fields[MyBool] == TRUE
*  TRUE
*  Fields[created] =~ "^2015"
*  Fields[widget] != NIL

#### Relational Operators

* == equals
* != not equals
* > greater than
* >= greater than equals
* < less than
* <= less than equals
* =~ Lua pattern match
* !~ Lua negated pattern match

#### Logical Operators

* Parentheses are used for grouping expressions
* && and (higher precedence)
* || or

#### Boolean

* TRUE
* FALSE

#### Constants

* NIL used to test the existence (!=) or non-existence (==) of a field variable

#### Message Variables

All message variables must be on the left hand side of the relational comparison
 
##### String

* Uuid - 16 byte raw binary type 4 UUID (useful for partitioning data)
* Type
* Logger
* Payload
* EnvVersion
* Hostname

##### Numeric

* Timestamp
* Severity
* Pid

##### Fields

* Fields[_field_name_] - shorthand for Field[_field_name_][0][0]
* Fields[_field_name_][_field_index_] - shorthand for Field[_field_name_][_field_index_][0]
* Fields[_field_name_][_field_index_][_array_index_]
* If a field type is mis-match for the relational comparison, false will be returned e.g., Fields[foo] == 6 where "foo" is a string

#### Quoted String

* single or double quoted strings are allowed

#### Lua Pattern Matching Expression

* see [Lua Patterns](http://www.lua.org/manual/5.1/manual.html#5.4.1)
* capture groups will be ignored
