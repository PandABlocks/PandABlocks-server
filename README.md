# PandABlocks-server

The PandA socket server provides a bridge between the register interface to the
FPGA firmware controlling the PandA hardware and users and other software. The
interface is designed to be simple and robust.

The PandA firmware is structured into numerous functional blocks, each
configured via a number of fields. This structure is directly reflected in the
functional interface provided by this server: most commands read or write
specific fields.

The server publishes two socket end points: one for configuration control
(simple ASCII commands, ASCII responses) and one for streamed data capture
(no commands, a lightly structured binary stream).

<!-- README only content. Anything below this line will be excluded from the index page -->

## Where to find things

| | |
|---|---|
| Source code | <https://github.com/PandABlocks/PandABlocks-server> |
| Documentation | <https://PandABlocks.github.io/PandABlocks-server> |
| Releases | <https://github.com/PandABlocks/PandABlocks-server/releases> |
