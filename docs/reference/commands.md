# Command interface

The default server port for the command interface is port 8888. All commands and
responses are in ASCII with lines separated by newline characters (ASCII
character `0x0A`).

All commands can be grouped into three forms (query, assignment, table
assignment) and two targets (system and fields). There are exactly four possible
response formats (ok, ok with value, error, multiple value). This section
describes the command interface.

The three basic command forms are:

| Name | Format | Description |
|---|---|---|
| Query | *target*`?` | Interrogates *target* for the current value; can return an error, a single value, or a list of multiple values. |
| Assignment | *target*`=`*value* | Updates *target* with the given value; can return an error or success. |
| Table | *target*`<`*format* | Command may be followed by lines of text, and *must* be terminated by a blank line. |

The four basic command responses are:

| Name | Format | Description |
|---|---|---|
| Success | `OK` | Returned by assignment and table commands to report a successful update. |
| Value | `OK =`*value* | Successful return of a single value from a query command. |
| Error | `ERR` *error* | Error string returned on any command failure. |
| Multi value | `!`*value* … `.` | Any number of values can be returned, each preceded by `!`, finally `.` by itself indicates end of input. |

Command forms and their possible responses:

| Form | Responses |
|---|---|
| Query | Error, Value, Multi value |
| Assignment | Error, Success |
| Table | Error, Success |

Each individual query target will either return a single value or multi-value,
as documented below.

Finally, there are two basic types of target: configuration commands and system
commands.

## Configuration commands

The entire hardware interface to PandA is structured into "blocks" and "fields",
and each field may have a number of "attributes" depending on the field type.
This structure is reflected in the form of the configuration commands tabulated
below:

| Command syntax | Description |
|---|---|
| *block*[*number*]`.`*field*`?` | Return current value of field. |
| *block*[*number*]`.`*field*`=`*value* | Assign value to field. |
| *block*[*number*]`.`*field*`<`[[`<`][`\|`]][`B`] | Write table data to field. `<` writes a fixed table, `<<` writes a streaming table and `<<\|` writes the last streaming table. |
| *block*[*number*]`.`*field*`.`*attr*`?` | Return current value of field attribute. |
| *block*[*number*]`.`*field*`.`*attr*`=`*value* | Assign value to field attribute. |
| *block*[*number*]`.*?` | Returns list of fields. |
| *block*[*number*]`.`*field*`.*?` | Returns list of field attributes. |

In all of these commands the number after the block is optional if there is only
one instance of that block, and is ignored for the two `.*?` commands. See the
description of the `.TABLE` fields in [](/reference/fields.md) for an explanation
of the optional format characters in the table write command.

## System commands

All system commands are prefixed with a leading `*` character. The simplest
command is `*IDN?`, which returns a system identification string:

```
< *IDN?
> OK =PandA SW: 4.1-1-g2a34851 FPGA: 4.2.0 04e81f37 8a2b0249 rootfs: 2023.2+snapshot-bb27e6aef4dafd501bb5d72c95facd178c92dc48 (langdale)
```

The available system commands are tabulated here and listed in more detail
below:

| Command | Description |
|---|---|
| `*IDN?` | Device identification. |
| `*ECHO `*string*`?` | Echo. |
| `*WHO?` | List connected clients. |
| `*BLOCKS?` | List device blocks. |
| `*DESC.`*block*`.`*field*[`.`*attr*]`?` <br> `*DESC.`*block*`.`*field*`[].`*subfield*`?` | Show description for field, attribute, or table subfield. |
| `*ENUMS.`*block*`.`*field*[`.`*attr*]`?` <br> `*ENUMS.`*block*`.`*field*`[].`*subfield*`?` | List enumerations for field, attribute, or table subfield. |
| `*CHANGES`[`.`*group*]`?` | Report changes to values. *group* can be any of `CONFIG`, `BITS`, `POSN`, `READ`, `ATTR`, `TABLE`, or `METADATA`. |
| `*CHANGES`[`.`*group*]`=`[`E`\|`S`] | Reset reported changes; *group* as above. |
| `*CAPTURE?` | Report fields configured for capture. |
| `*CAPTURE.*?` | List all fields that can be captured. |
| `*CAPTURE.`*name*`?` | Interrogate capture options; *name* can be `OPTIONS` or `ENUMS`. |
| `*CAPTURE=` | Reset data capture. |
| `*POSITIONS?` | Enumerate possible capture positions. |
| `*BITS?` | Enumerate possible bit bus positions. |
| `*VERBOSE=`*value* | Control command logging. |
| `*PCAP.`*field*`?` | Special position capture status fields. *field* can be any of `STATUS`, `CAPTURED`, or `COMPLETION`. |
| `*PCAP.`*field*`=` | Position capture actions. *field* can be either `ARM` or `DISARM`. |
| `*SAVESTATE=` | Triggers immediate save to file of the persistence file state. |
| `*CLOCK_FREQ?` | Returns currently configured system clock frequency. |

`*IDN?`
: Returns the system identification string, for example:

  ```
  OK =PandA SW: 1.1 FPGA: 0.1.9 d1275f61 00000000 rootfs: PandA 1.1
  ```

  The first field after "PandA" is the software version, the second is the FPGA
  version, the third the firmware build number, and the fourth identifies the
  supporting firmware. The final fields (prefixed `rootfs:`) identify the
  underlying system on which the server is running.

  Note that the `rootfs:` identification is new to version 1.1 of PandA.

`*ECHO `*string*`?`
: Returns the string back to the caller. Not terribly useful. Note that the
  echoed string cannot contain any of `?`, `=` or `<`, as this would cause the
  command to be mistaken for another command format. Example usage:

  ```
  < *ECHO This is a test?
  > OK =This is a test
  ```

`*WHO?`
: Returns a list of client connections, for example:

  ```
  < *WHO?
  > !2015-12-04T14:30:40.403Z config 127.0.0.1:34185
  > .
  ```

  The first field is the time the connection was made, the second is either
  `config` or `data` depending on whether the configuration or data port is
  connected, and the third is the remote IP address and socket.

`*BLOCKS?`
: Returns a list of all the top-level blocks in the system. The order in which
  the blocks are returned is somewhat arbitrary. For example (here the list has
  been shortened in the middle):

  ```
  < *BLOCKS?
  > !TTLIN 6
  > !OUTENC 4
  ...
  > !CLOCKS 1
  > !BITS 1
  > !QDEC 4
  > .
  ```

  Block and field commands can be used to interrogate each block. The number
  after each block records the number of instances of each block.

`*DESC.`*block*`?` <br> `*DESC.`*block*`.`*field*`?` <br> `*DESC.`*block*`.`*field*`.`*attr*`?` <br> `*DESC.`*block*`.`*field*`[].`*subfield*`?`
: Returns the description string for the specified block, field, attribute, or
  table subfield, e.g.:

  ```
  < *DESC.TTLIN?
  > OK =TTL input
  < *DESC.TTLIN.TERM?
  > OK =Select TTL input termination
  < *DESC.TTLIN.TERM.INFO?
  > OK =Class information for field
  ```

`*ENUMS.`*block*`.`*field*`?` <br> `*ENUMS.`*block*`.`*field*`.`*attr*`?` <br> `*ENUMS.`*block*`.`*field*`[].`*subfield*`?`
: Returns the list of enumerations for the given field, attribute, or table
  subfield, if appropriate.

`*CHANGES?` <br> `*CHANGES.CONFIG?` <br> `*CHANGES.BITS?` <br> `*CHANGES.POSN?` <br> `*CHANGES.READ?` <br> `*CHANGES.ATTR?` <br> `*CHANGES.TABLE?` <br> `*CHANGES.METADATA?`
: Reports changes to the appropriate group of values. Changes are reported since
  the last request on the connection, and on the first request the current value
  for every field will be reported. `*CHANGES?` reports changes for all groups;
  otherwise one of the following groups can be selected:

  | Group | Description |
  |---|---|
  | CONFIG | Configuration settings |
  | BITS | Bits on the system bus |
  | POSN | Positions |
  | READ | Polled read values |
  | ATTR | Attributes (including capture enable flags) |
  | TABLE | Table changes |
  | METADATA | Metadata field changes |

  For example:

  ```
  < *CHANGES.CONFIG?
  > !TTLIN1.TERM=High-Z
  > !TTLIN2.TERM=50-Ohm
  > !TTLIN3.TERM=High-Z
  ...
  > !QDEC2.B=TTLIN1.VAL
  > !QDEC3.B=TTLIN1.VAL
  > !QDEC4.B=TTLIN1.VAL
  > .
  ```

  Here 804 (at the time of writing) lines have been deleted from the transcript.
  If we repeat the call we see that no further changes have happened until
  something is actually changed:

  ```
  < *CHANGES.CONFIG?
  > .
  < TTLOUT4.VAL=TTLIN3.VAL
  > OK
  < *CHANGES.CONFIG?
  > !TTLOUT4.VAL=TTLIN3.VAL
  > .
  ```

  Note that for tables only the fact that the table has changed is shown; no
  attempt is made to show the current table value:

  ```
  < *CHANGES.TABLE?
  > !PCOMP1.TABLE<
  > !PCOMP2.TABLE<
  > !PCOMP3.TABLE<
  > !PCOMP4.TABLE<
  > !PGEN1.TABLE<
  > !PGEN2.TABLE<
  > !SEQ1.TABLE<
  > !SEQ2.TABLE<
  > !SEQ3.TABLE<
  > !SEQ4.TABLE<
  > .
  ```

`*CHANGES=`[`E`\|`S`] <br> `*CHANGES.CONFIG=`[`E`\|`S`] <br> `*CHANGES.BITS=`[`E`\|`S`] <br> `*CHANGES.POSN=`[`E`\|`S`] <br> `*CHANGES.READ=`[`E`\|`S`] <br> `*CHANGES.ATTR=`[`E`\|`S`] <br> `*CHANGES.TABLE=`[`E`\|`S`] <br> `*CHANGES.METADATA=`[`E`\|`S`]
: These commands reset the change information for the corresponding group so that
  only changes occurring after the reset are reported, or so that all changes are
  reported. If `=` or `=E` (for End) is specified then only new changes are
  reported; if `=S` (for Start) then change reporting is reset to the start as
  for a new connection. For example:

  ```
  < TTLIN1.TERM=50-Ohm
  > OK
  < *CHANGES=
  > OK
  < *CHANGES.CONFIG?
  > .
  ```

`*CAPTURE?`
: Returns a list of all positions and bit masks that will be written to the data
  capture port. This list is controlled by setting the `.CAPTURE` attribute on
  the corresponding position fields.

`*CAPTURE.*?`
: Returns a list of all fields that can be configured for capture. This includes
  all `pos_out` and `ext_out` fields.

`*CAPTURE.OPTIONS?`
: Lists the available capture options for `pos_out` fields. See
  [](/reference/capture-options.md) for the full set of options.

`*CAPTURE.ENUMS?`
: Generates a curated list of capture option selections, designed for presenting
  lists of available capture options as an enumeration. Returns the same as
  calling `*ENUMS.`*name*`.`*field*`.CAPTURE?` on any `pos_out` field.

`*CAPTURE=`
: Resets all `.CAPTURE` flags to zero so that no data will be captured.

`*POSITIONS?`
: Lists all available position capture fields in order.

`*BITS?`
: Lists all available bit bus positions, but not including the special values
  `ZERO` and `ONE`.

`*VERBOSE=`*value*
: If `*VERBOSE=1` is set then every command will be echoed to the server's log.
  Set `*VERBOSE=0` to restore normal quiet behaviour.

`*PCAP.STATUS?` <br> `*PCAP.CAPTURED?` <br> `*PCAP.COMPLETION?`
: Interrogates the status of position capture:

  | Field | Description |
  |---|---|
  | STATUS | Returns a string with three fields: "Busy" or "Idle", followed by the number of connected readers, and the number taking data. |
  | CAPTURED | Returns the number of samples captured in the current or most recent data capture. |
  | COMPLETION | Returns the completion status from the most recent data capture, as listed in the table below. |

  The completion codes have the following meaning:

  | Code | Meaning |
  |---|---|
  | Busy | Capture in progress. |
  | Ok | Capture completed without error or intervention. |
  | Disarmed | Capture was manually disarmed by `*PCAP.DISARM=` command. |
  | Framing error | Data capture framing error, probably due to incorrectly configured capture. |
  | DMA data error | Internal data error, should not occur. |
  | Driver data overrun | Data capture too fast, internal buffers overrun. Can also occur if the PandA processor is overloaded. |

`*PCAP.ARM=` <br> `*PCAP.DISARM=`
: Top-level capture control:

  | Field | Description |
  |---|---|
  | ARM | Initiates data capture. Will fail if capture is already in progress, or no fields are configured for capture. |
  | DISARM | Halts ongoing data capture. |

`*SAVESTATE=`
: Updates the persistence state file (as configured on the command line when
  launched) with the current state. Returns after a filesystem `sync` call, so it
  is safe to power off the system after this command has completed.

`*CLOCK_FREQ?`
: Returns the currently configured FPGA clock frequency, as used to convert
  between times in natural units and times in clock ticks.
