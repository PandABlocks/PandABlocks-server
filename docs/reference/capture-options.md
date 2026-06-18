# Capture options

This page is the canonical reference for the two distinct sets of "capture
options" exposed by the server:

- **Field capture options** — the per-field `CAPTURE` enumeration that selects
  *what* is captured for a `pos_out` field.
- **Connection capture options** — the option line sent when connecting to the
  data port that selects *how* the captured stream is transmitted.

:::{note}
The authoritative, live list for your firmware is always whatever
`*CAPTURE.OPTIONS?` returns from the running server — availability of some
options (`StdDev`, and therefore `Mean StdDev`) depends on the FPGA
configuration. Live confirmation tracked in [PandABlocks/PandABlocks-server#78](https://github.com/PandABlocks/PandABlocks-server/issues/78).
:::

## Field capture options

`*CAPTURE.OPTIONS?` lists the individual capture options available for
`pos_out` fields:

| Option | Description |
|---|---|
| Value | The value at the time of trigger is captured. |
| Diff | The difference of values is captured. |
| Sum | The sum of all valid values is captured (64-bit; may be scaled if `PCAP.SHIFT_SUM` is set). |
| Mean | The average of all valid values is captured. |
| Min | The minimum of all valid values is captured. |
| Max | The maximum of all valid values is captured. |
| StdDev | The standard deviation of valid values is captured. Only listed if supported by the FPGA configuration. |

A field's `CAPTURE` attribute can be set to `No` (capture disabled), to a
single option, or to a space-separated combination of options (e.g.
`Min Max Mean`).

`*CAPTURE.ENUMS?` returns a curated enumeration of these selections — the same
list as calling `*ENUMS.`*block*`.`*field*`.CAPTURE?` on any `pos_out` field:
`No`, `Value`, `Diff`, `Sum`, `Mean`, `Min`, `Max`, `Min Max`, `Min Max Mean`,
`StdDev`, `Mean StdDev`.

These options are set per field via the field's `CAPTURE` attribute — see
[](/reference/fields.md) for the `pos_out` and `ext_out` capture settings, and
[](/reference/capture.md) for how capture is configured and armed.

## Connection capture options

A line of capture options *must* be sent after the initial connection to the
data port before any data is sent. It is a list of any of the following options
separated by whitespace, ending with a newline character.

| Option | Description | | |
|---|---|---|---|
| ASCII | Data is sent as ASCII numbers. | 1 | D |
| BASE64 | Binary data is sent as a stream of base-64 strings. | 1 | |
| FRAMED | Binary data is sent as a sequence of sized frames. | 1 | |
| UNFRAMED | Binary data is sent as a raw stream of bytes. | 1 | R |
| SCALED | All scalable data is scaled and sent as doubles. | 2 | D |
| RAW | The captured binary data is sent without processing. | 2 | |
| NO_HEADER | The data header is omitted. | | R |
| NO_STATUS | The connection and end-of-experiment status strings are omitted. | | R |
| ONE_SHOT | Only one experiment will be transmitted. | | R |
| XML | The header will be sent in XML format. | | |
| BARE | Selects `UNFRAMED RAW NO_HEADER NO_STATUS ONE_SHOT`. | | |
| DEFAULT | Default options. | | D |

Key:

- **D** — Default option if no other option is specified.
- **R** — Option selected in response to the `BARE` option.
- **1** — Data transmission formats; one of these will be selected.
- **2** — Data processing formats; one of these will be selected.

For how these formats appear on the wire (framing, base-64 layout, header
contents), see [](/reference/capture.md).
