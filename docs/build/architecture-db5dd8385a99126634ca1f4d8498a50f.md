# Server architecture

:::{admonition} Skeleton — depth coming later
:class: note

This page provides an orientation skeleton. Detailed internals (locking
strategy, DMA pipeline, persistence state machine) are tracked in the issue
backlog as **blocked: author**.
:::

The PandABlocks server is a C daemon that presents two TCP sockets and bridges
them to the PandA FPGA hardware (or a software simulation of it).

## Sockets

**Configuration port (default 8888)**
: An ASCII command/response interface used to read and write block fields and
  execute system commands. Described in [](/reference/commands.md).

**Data port (default 8889)**
: A binary streaming interface for captured experiment data. Described in
  [](/reference/capture.md).

## Internal structure

The server is organised around three cooperating layers.

**Configuration thread** (`config_server.c`)
: Accepts connections on the configuration port. Parses incoming ASCII commands,
  dispatches them to the block/field database (`config_command.c`, `fields.c`),
  and formats responses. One thread per client connection.

**Data thread** (`data_server.c`)
: Accepts connections on the data port. Reads captured data from the central
  circular buffer and streams it to connected clients. Handles framing,
  base-64 encoding, and metadata headers.

**Hardware layer** (`hardware.c` / `sim_hardware.c`)
: Abstracts register reads/writes and DMA transfers to the FPGA. In simulation
  mode (`sim_hardware.c`) the hardware layer is replaced by an in-process
  emulator.

## Block model

The server loads its block and field topology from the `config_d` configuration
files at startup (see [](/reference/config.md)). Each block is a named,
potentially multi-instance hardware component (e.g. `PULSE[4]`). Fields within
a block map to registers and expose typed operations (read, write, capture, etc.)
as documented in [](/reference/fields.md).

## Persistence

Field values that survive a server restart are written to a persistence file
(see the `-f` and `-t` options in [](/how-to/startup.md)). The file is updated
on a poll/holdoff/backoff schedule to reduce write pressure.
