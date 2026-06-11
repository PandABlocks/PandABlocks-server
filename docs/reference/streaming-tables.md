# Streaming tables

Tables in PandA operate in two modes: **fixed** (a single write replaces the
whole table) and **streaming** (a sequence of writes fills a DMA-backed ring
buffer that the hardware consumes continuously). Streaming is used for long,
DMA-driven tables — for example the SEQ sequencer table — where the host needs
to supply data faster than a single write can deliver.

## Write operators

The operator appended to the field name on the configuration port determines
which mode is used:

| Operator | Encoding | Meaning                                          |
|----------|----------|--------------------------------------------------|
| `<`      | text     | Fixed table write                                |
| `<<`     | text     | Streaming table write (more data follows)        |
| `<<\|`   | text     | Streaming table write, last chunk                |
| `<B`     | base-64  | Fixed table write                                |
| `<<B`    | base-64  | Streaming table write (more data follows)        |
| `<<\|B`  | base-64  | Streaming table write, last chunk                |

For text-encoded writes the data is a sequence of decimal integers, one per
line, terminated by a blank line. For base-64 writes the payload follows
base-64 encoding rules.

Writing an empty table with `<<` or `<<|` is rejected to prevent accidental
errors while in streaming mode. To reset a table to the `INIT` state, write an
empty fixed table with `<`.

## `MODE` attribute

Each `table`-type field exposes a `MODE` attribute that reflects the table's
current state:

| Value            | Meaning                                                          |
|------------------|------------------------------------------------------------------|
| `INIT`           | No table loaded                                                  |
| `FIXED`          | A fixed table was last written                                   |
| `STREAMING`      | A streaming write is in progress (`<<` was last)                 |
| `STREAMING_LAST` | The final streaming chunk was received (`<<|` was last)          |

Writing an empty table (`<0`) always moves the table to `INIT`. If a streaming
error occurs, `MODE` transitions automatically to report the error condition so
the client can become aware of failures.

### MODE transition table

| Current MODE / command | `<`     | `<<`        | `<<\|`           | `<0`   |
|------------------------|---------|-------------|------------------|--------|
| `INIT`                 | `FIXED` | `STREAMING` | `STREAMING_LAST` | `INIT` |
| `FIXED`                | `FIXED` | `STREAMING` | `STREAMING_LAST` | `INIT` |
| `STREAMING`            | `FIXED` | `STREAMING` | `STREAMING_LAST` | `INIT` |
| `STREAMING_LAST`       | `FIXED` | `STREAMING` | `STREAMING_LAST` | `INIT` |

## Other table attributes

`LENGTH`
: Current number of words in the table (read-only).

`MAX_LENGTH`
: Maximum number of table rows (read-only).

`ROW_WORDS`
: Number of 32-bit words per table row (read-only).

`B` (base-64 read)
: Returns the current table content encoded in base-64. Each line has the
  format `left:right:data` where *left* and *right* are bit-field indices into
  a row and *data* is the base-64 encoded row content.

A `<<` write returns the number of lines accepted. A `<` (fixed) write returns
the total number of lines in the new table.

## Buffer sizing

Long (DMA) tables are allocated as *2^size* words per buffer with *nbuf* double
buffers (as specified in the `registers` file — see {doc}`/reference/config`).
The simulation server allocates `4096 × 2^order` bytes per buffer. When choosing
buffer counts and sizes for a new block, ensure the ring is large enough to
absorb worst-case host latency between refills.
