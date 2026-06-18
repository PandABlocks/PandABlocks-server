# Data capture

## Capture configuration

Both `pos_out` and `ext_out` fields can be configured for data capture through
the data capture port by setting the appropriate value in the `CAPTURE`
attribute. The possible capture settings depend on the field type as follows:

`pos_out`
: | Value | Description |
  |---|---|
  | No | Capture is disabled for this field. |
  | Value | The value at the time of trigger will be captured. |
  | Diff | The difference of values is captured. |
  | Sum | The sum of all valid values is captured. This is a 64-bit value, and may be further scaled if `PCAP.SHIFT_SUM` is set. |
  | Mean | The average of all valid values is captured. |
  | Min | The minimum of all valid values is captured. |
  | Max | The maximum of all valid values is captured. |
  | Min Max | Both minimum and maximum values are captured. |
  | Min Max Mean | All three values — minimum, maximum, average — are captured. |
  | StdDev | The standard deviation of valid values is captured. Only available if supported by the FPGA configuration. |
  | Mean StdDev | Both average and standard deviation are captured. Only available if supported by the FPGA configuration. |

`ext_out`
: | Value | Description |
  |---|---|
  | No | This field will not be captured. |
  | Value | This field will be captured. |

See [](/reference/capture-options.md) for the full list of capture options and
how they are selected.

## Data capture port

The default server port for the data interface is port 8889. The initial
exchange is in ASCII with newline-separated lines; subsequent data communication
is as selected in the initial connection.

Data capture proceeds as follows:

1. Connect to the data server port, default 8889.
2. Send the capture options string followed by a newline. The newline character
   is mandatory.
3. The server responds with `OK` unless there was an error parsing the capture
   options, or the `NO_STATUS` option was specified. If there was an error, the
   server responds with `ERR` followed by an error message and the connection is
   closed.
4. The server now ignores all further input from the client, and the connection
   pauses until data capture is started via the `*PCAP.ARM=` command.
5. At the beginning of a round of data capture or "experiment", a header
   detailing the data to be sent and the data format is sent in ASCII followed by
   an empty line. If `NO_HEADER` was selected then the header and blank line are
   omitted.
6. Captured data is sent in the requested format until the experiment is complete
   (either internally disarmed or disarmed via the `*PCAP.DISARM=` command), or
   there is a communication problem.
7. At the end of the experiment a completion code is sent as a single line in
   ASCII starting with `END`, unless `NO_STATUS` was specified.
8. Unless `ONE_SHOT` was specified, the server pauses until the next experiment
   (step 4).

The line of capture options sent in step 2 selects the transmission format,
processing and framing of the data stream — see
[](/reference/capture-options.md).

### Data transport formatting

Note that all binary data is sent with the lowest-order byte first.

`ASCII`
: Each value is formatted as an ASCII number, and transmitted with one line per
  captured sample.

`BASE64`
: The stream of binary data is converted to base-64 strings and transmitted as a
  series of lines until the experiment is complete. Each base-64 string is
  preceded by a single space, so the end of the stream is easy to identify.

`FRAMED`
: The captured binary data is sent in blocks of unpredictable size. Each block is
  preceded by 8 bytes: the first four are `BIN` followed by a space, the
  remaining four are the length of the data block in bytes *including* the 8-byte
  header.

`UNFRAMED`
: The captured binary data is sent as is. In this mode it is difficult or
  impossible to reliably detect the end of the data stream, so this is normally
  best combined with `NO_STATUS` and `ONE_SHOT`.

### Data header

At the beginning of each experiment the following information is sent:

| Field | Description |
|---|---|
| arm_time | System timestamp when the ARM command was sent. |
| start_time | Timestamp of when PCAP became both armed and enabled. Uses a hardware-provided timestamp (e.g. from an event receiver) if available, falling back to the system timestamp. |
| hw_time_offset_ns | Offset in ns from the hardware timestamp to system time at the start of the experiment. Only present when a hardware time source is used. Used to check that hardware and system times have not drifted too far apart. |
| missed | Number of samples missed by a late data-port connection. |
| process | Data processing option: Scaled, Unscaled, or Raw. |
| format | Data delivery formatting: ASCII, Base64, Framed, or Unframed. |
| sample_bytes | Number of bytes in one sample unless `format` is `ASCII`. |
| fields | Information about each captured field. |

All timestamps are in
[ISO 8601 UTC format](https://en.wikipedia.org/wiki/ISO_8601) with nanosecond
resolution, i.e. `YYYY-MM-DDTHH:mm:ss.sssssssssZ`.

`start_time` will be a hardware timestamp if a hardware time source which produces
non-zero timestamps is selected; otherwise it will be a system timestamp saved by
the driver.

For each field the following information is sent:

| Field | Description | |
|---|---|---|
| name | Name of captured field. | |
| type | Data type of transmitted field after data processing. | |
| capture | Value of `CAPTURE` field used to enable this field. | |
| scale | Scaling factor if scaled field. | S |
| offset | Offset if scaled field. | S |
| units | Units string if scaled field. | S |

Key: **S** — only present if a scaled field.

If the `XML` option is selected the header is structured as a single `header`
element containing `data` and `fields` elements.

The `type` field can be one of the following strings:

| String | Bytes | Description |
|---|---|---|
| int32 | 4 | Used for scalable values sent in unscaled modes. |
| uint32 | 4 | Used for bit masks. |
| int64 | 8 | Used for raw ADC mean and unscaled 48-bit encoder data. |
| double | 8 | Used for all scaled values when `SCALED` is selected. |

### Experiment completion

At the end of each capture experiment a single line is sent, e.g.:

```
END 10 Ok
```

This specifies the number of samples sent and gives a completion code, which can
be one of the following values:

| Code | Meaning |
|---|---|
| Ok | Experiment completed without intervention. |
| Disarmed | Experiment manually completed by `*PCAP.DISARM=` command. |
| Early disconnect | Client disconnect detected. |
| Data overrun | Client not taking data quickly, or network congestion; internal buffer overflow. |
| Framing error | Triggers too fast for configured data capture. |
| Driver data overrun | Probable CPU overload on PandA; should not occur. |
| DMA data error | Data capture too fast for memory bandwidth. |

## High performance mode

To get the highest performance, use `FRAMED RAW` mode. This activates a special
passthrough mode which avoids copying memory as much as possible. In tests it has
been capable of sustaining 60 MBytes/s when panda-webcontrol is **not** installed. <!-- verify: PandABlocks/PandABlocks-server#79 — confirm figure and conditions -->
The downside to this mode is that if capture fails for any reason, then the last
framed block of data that the server sent should be discarded, as it will have
been corrupted while being sent.

## Examples

Some examples of data capture for different options follow.

Default:

```
arm_time: 2021-05-26T10:34:06.133Z
missed: 0
process: Scaled
format: ASCII
fields:
 PCAP.CAPTURE_TS double Trigger
 COUNTER1.OUT double Triggered scale: 1 offset: 0 units:
 COUNTER2.OUT double Triggered scale: 1 offset: 0 units:
 PGEN1.OUT double Triggered scale: 1 offset: 0 units:

 1e-06 0 0 262143
 3e-06 0 0 262142
 5e-06 0 0 262141
 7e-06 0 0 262140
 9e-06 0 0 262139
END 5 Ok
```

`BASE64`:

```
arm_time: 2021-05-26T10:34:06.133Z
missed: 0
process: Scaled
format: Base64
sample_bytes: 32
fields:
 PCAP.CAPTURE_TS double Trigger
 COUNTER1.OUT double Triggered scale: 1 offset: 0 units:
 COUNTER2.OUT double Triggered scale: 1 offset: 0 units:
 PGEN1.OUT double Triggered scale: 1 offset: 0 units:

 ju21oPfGsD4AAAAAAAAAAAAAAAAAAAAAAAAAAPj/D0FU5BBxcyrJPgAAAAAAAAAAAAAAAAAAAAAA
 AAAA8P8PQfFo44i1+NQ+AAAAAAAAAAAAAAAAAAAAAAAAAADo/w9BuF8+WTFc3T4AAAAAAAAAAAAA
 AAAAAAAAAAAAAOD/D0E/q8yU1t/iPgAAAAAAAAAAAAAAAAAAAAAAAAAA2P8PQQ==
END 5 Ok
```

`XML`:

```
<header>
<data arm_time="2021-05-26T10:35:06.107Z" missed="0" process="Scaled" format="ASCII" />
<fields>
<field name="PCAP.CAPTURE_TS" type="double" capture="Trigger" />
<field name="COUNTER1.OUT" type="double" capture="Triggered" scale="1"
offset="0" units="" />
<field name="COUNTER2.OUT" type="double" capture="Triggered" scale="1"
offset="0" units="" />
<field name="PGEN1.OUT" type="double" capture="Triggered" scale="1" offset="0"
units="" />
</fields>
</header>

 1e-06 0 0 262143
 3e-06 0 0 262142
 5e-06 0 0 262141
 7e-06 0 0 262140
 9e-06 0 0 262139
END 5 Ok
```

## Getting captured data out

This page describes the wire format on the data port. For end-to-end ways of
retrieving captured data — reading the binary stream directly, using the Python
client, or via EPICS/Tango — see the meta-panda how-to
[Integrate with a PandA](xref:meta-panda/how-to/integrate-with-a-panda).

<!-- Stage A xref/intersphinx probe: a real cross-link into a Sphinx repo
     (PandABlocks-client) that must resolve in the BUILT output. -->
As a starting point, captured data can be read out programmatically with the
Python client's
[`BlockingClient`](xref:PandABlocks-client#pandablocks.blocking.BlockingClient).
