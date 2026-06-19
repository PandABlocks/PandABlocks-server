# Config, registers and description files

On startup the PandA server loads its configuration from three files: `config`,
`registers`, and `description`. When running the simulation these are loaded from
the `config_d` directory in the build directory; on PandA they are loaded from
`/opt/share/panda/config_d`.

The syntax of each file mirrors the field definitions described in
[](/reference/fields.md). All three files share a common structure: indentation
indicates hierarchy, `#` starts a comment, block names appear in the first
column, and fields are indented one level.

| File          | Purpose                                                      |
|---------------|--------------------------------------------------------------|
| `config`      | Defines all blocks, their fields, and each field's behaviour |
| `registers`   | Maps each block and field to hardware register offsets       |
| `description` | Optionally provides human-readable descriptions              |

## Configuration file `config`

This file defines all blocks and fields available to this PandA instance and is
processed first.

**Block definition syntax:**

```
block-name [ "[" count "]" ]
    [ field-definition ]*
```

A block definition gives the block a name and optionally a repeat *count* (the
number of hardware instances). It is followed by indented field definitions.

**Field definition syntax:**

```
field-name field-type [ field-type-data ]
```

The *field-type* determines the basic function of the field, what operations are
permitted on it, and how it maps to hardware registers.

### Field types

| Field type                              | Description                                         |
|-----------------------------------------|-----------------------------------------------------|
| `param` *subtype* [`=` *value*]         | Single 32-bit value written to a register; optional initial value |
| `read` *subtype*                        | Read-only register                                  |
| `write` *subtype*                       | Write-only register (triggers an immediate action)  |
| `time`                                  | Like `param` but 64-bit, spanning two registers     |
| `bit_out`                               | Output bit                                          |
| `pos_out` [*scale* [*offset* [*units*]]]| Position bus output; optional default scale/offset/units |
| `ext_out` (`timestamp` \| `samples` \| `bits` *group*) | Extension bus entry needing special treatment |
| `bit_mux` [`=` *value*]                 | Bit bus input selector; optionally defaults to `ONE` (129) |
| `pos_mux`                               | Position bus input selector                         |
| `table` [*row-words*]                   | Long table of numbers with special access methods   |

`ext_out` *ext-extra* values:

| Value           | Description                                              |
|-----------------|----------------------------------------------------------|
| `timestamp`     | Captures a 64-bit timestamp                              |
| `samples`       | Captures the sample count for data capture               |
| `bits` *group*  | Captures 32 bits of the 128-bit bit bus; *group* selects which 32-bit slice |

### Field subtypes

`param`, `read`, and `write` fields require one of the following subtypes:

| Subtype                                 | Description                                         |
|-----------------------------------------|-----------------------------------------------------|
| `uint` [*max-value*]                    | Unsigned integer, optionally bounded                |
| `int`                                   | Signed integer                                      |
| `scalar` *scale* [*offset* [*units*]]   | Fixed-point value with scale and optional offset/units |
| `bit`                                   | Single bit                                          |
| `action`                                | Write-only trigger with no data payload             |
| `lut`                                   | 5-input lookup table function                       |
| `enum`                                  | Enumeration; followed by indented `number string` pairs |
| `position`                              | Position value                                      |
| `time`                                  | Time value                                          |

## Register file `registers`

This file assigns hardware registers to each block and field. Its structure
mirrors `config` but replaces field type specifications with register
assignments.

**Block definition syntax:**

```
block-name { [ "S" ] block-register | "X" } [ extension-module ]
    [ field-definition ]*
```

- Prefix `S` on *block-register* allows the same register to be shared across
  multiple blocks.
- `X` instead of a register number marks a block with no register-mapped fields
  (extension-only fields only).
- An optional *extension-module* name enables the extension register syntax for
  this block.

**Field register syntax by type:**

| Class                  | Register syntax                                       |
|------------------------|-------------------------------------------------------|
| `param`                | *register* \| *write-extension*                       |
| `read`                 | *register* \| *read-extension*                        |
| `write`                | *register* \| *write-extension*                       |
| `time`                 | *low-register* *high-register*                        |
| `bit_out`              | `(`*bit-index*`)`N                                    |
| `pos_out`              | `(`*pos-index*`)`N                                    |
| `ext_out timestamp`    | *ext-index* *ext-index*                               |
| `ext_out` other        | *ext-index*                                           |
| `bit_mux`              | *register*                                            |
| `pos_mux`              | *register*                                            |
| `table` (short)        | `short` *size* *init-reg* *fill-reg* *length-reg*     |
| `table` (long/DMA)     | `long` `2^`*size* *nbuf* *base-reg* *length-reg*      |

The notation `(...)`N means the register number is repeated N times, once per
hardware instance of the block.

### Extension register syntax

When an extension server is enabled (see [](/how-to/startup.md) `-X` option) and
an extension module is associated with a block, `param`, `read`, and `write`
fields may be redirected to it:

```
read-extension  = [ read-reg ]* "X" field-spec
write-extension = [ read-reg ]* [ "W" [ write-reg ]* ] "X" field-spec
```

*field-spec* is passed to the extension module to bind the field. See
[](/reference/extension.md) for details on extension fields.

## Description file `description`

The entire file is optional. Its syntax is:

```
block-name block-description
    field-name field-description
```

Descriptions are newline-terminated UTF-8 strings.
