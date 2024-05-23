Command Interface
=================

The default server port for the command interface is port 8888.  All commands
and responses are in ASCII with lines separated with newline characters (ASCII
character 0x0A).

All commands can be grouped into three forms (query, assignment, table
assignment) and two targets (system and fields).  There exactly four possible
response formats (ok, ok with value, error, multiple value).  This section
describes this command interface.

The three basic command forms are:

=========== ======================= ============================================
Name        Format                  Description
=========== ======================= ============================================
Query       target\ ``?``           Interrogates *target* for current value, can
                                    return error, single value or a list of
                                    multiple values.
Assignment  target\ ``=``\ value    Updates *target* with given value, can
                                    return error or success.
Table       target\ ``<``\ format   Command may be followed by lines of text,
                                    *must* be terminated by blank line.
=========== ======================= ============================================

The four basic command responses are:

=========== ======================= ============================================
Name        Format                  Description
=========== ======================= ============================================
Success     ``OK``                  Returned assignment and table commands to
                                    report successful update.
Value       ``OK =``\ value         Successful return of single value from
                                    query command.
Error       ``ERR`` error           Error string returned on any command failure
Multi value | ``!`` value           Any number of values can be returned, each
            | ``.``                 preceded by ``!``, and finally ``.`` by
                                    itself indicates end of input.
=========== ======================= ============================================

Command forms and their possible responses:

=========== ====================================================================
Form        Responses
=========== ====================================================================
Query       Error, Value, Multi value
Assignment  Error, Success
Table       Error, Success
=========== ====================================================================

Each individual query target will either return a single value or multi-value,
as documented below.

Finally, there are two basic types of target: configuration commands and system
commands.


Configuration Commands
----------------------

The entire hardware interface to PandA is structured into "blocks" and "fields",
and each field may have a number of "attributes" depending on the field type.
This structure is reflected in the form of configuration commands which are
tabulated below:


+-------------------------------+----------------------------------------------+
| Command Syntax                | Description                                  |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Return current value of field.               |
| ``?``                         |                                              |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Assign value to field.                       |
| ``=``\ value                  |                                              |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Write table data to field.                   |
| ``<``\ [``<``][``B``]         |                                              |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Return current value of field attribute.     |
| ``.``\ attr\ ``?``            |                                              |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Assign value to field attribute.             |
| ``.``\ attr\ ``=``\ value     |                                              |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.*?``        | Returns list of fields.                      |
+-------------------------------+----------------------------------------------+
| block[number]\ ``.``\ field\  | Returns list of field attributes.            |
| ``.*?``                       |                                              |
+-------------------------------+----------------------------------------------+

In all of these commands the number after the block is optional if there is only
one instance of that block, and is ignored for the two ``.*?`` commands.  See
the description of the ``.TABLE`` fields for an explanation of the optional
format characters in the table write command.


System Commands
---------------

All system commands are prefixed with a leading ``*`` character.  The simplest
command is ``*IDN?`` which returns a system identification string::

    < *IDN?
    > OK =PandA SW: 330bd94-dirty FPGA: 0.1.9 d1275f61 00000000

The available system commands are tabulated here and listed in more detail
below:

+-------------------------------+----------------------------------------------+
| Command                       | Description                                  |
+-------------------------------+----------------------------------------------+
| ``*IDN?``                     | Device identification.                       |
+-------------------------------+----------------------------------------------+
| ``*ECHO string?``             | Echo.                                        |
+-------------------------------+----------------------------------------------+
| ``*WHO?``                     | List connected clients.                      |
+-------------------------------+----------------------------------------------+
| ``*BLOCKS?``                  | List device blocks.                          |
+-------------------------------+----------------------------------------------+
| | ``*DESC.`` block\ ``.``\    | Show description for field, attribute, or    |
|   field[\ ``.``\ attr]\ ``?`` | table subfield.                              |
| | ``*DESC.`` block\ ``.``\    |                                              |
|   field\ ``[].``\ subfield\   |                                              |
|   ``?``                       |                                              |
+-------------------------------+----------------------------------------------+
| | ``*ENUMS.`` block\ ``.``\   | List enumerations for field, attribute, or   |
|   field[\ ``.``\ attr]\ ``?`` | table subfield.                              |
| | ``*ENUMS.`` block\ ``.``\   |                                              |
|   field\ ``[].``\ subfield\   |                                              |
|   ``?``                       |                                              |
+-------------------------------+----------------------------------------------+
| ``*CHANGES``\ [\ ``.``\       | Report changes to values.  *group* can be    |
| group]\ ``?``                 | any of ``CONFIG``, ``BITS``, ``POSN``,       |
|                               | ``READ``, ``ATTR``, or ``TABLE``.            |
+-------------------------------+----------------------------------------------+
| ``*CHANGES``\ [\ ``.``\       | Reset reported changes, *group* as above.    |
| group]\ ``=``\ [\ ``E``\      |                                              |
| | ``S``\ ]                    |                                              |
+-------------------------------+----------------------------------------------+
| ``*CAPTURE?``                 | Report fields configured for capture.        |
+-------------------------------+----------------------------------------------+
| ``*CAPTURE.*?``               | List all fields that can be captured.        |
+-------------------------------+----------------------------------------------+
| ``*CAPTURE.``\ name\ ``?``    | Interrogate capture options, *name* can be   |
|                               | ``OPTIONS`` or ``ENUMS``.                    |
+-------------------------------+----------------------------------------------+
| ``*CAPTURE=``                 | Reset data capture.                          |
+-------------------------------+----------------------------------------------+
| ``*POSITIONS?``               | Enumerate possible capture positions.        |
+-------------------------------+----------------------------------------------+
| ``*BITS?``                    | Enumerate possible bit bus positions.        |
+-------------------------------+----------------------------------------------+
| ``*VERBOSE=``\ value          | Control command logging.                     |
+-------------------------------+----------------------------------------------+
| ``*PCAP.``\ field\ ``?``      | Special position capture status fields.      |
|                               | *field* can be any of ``STATUS``,            |
|                               | ``CAPTURED``, or ``COMPLETION``.             |
+-------------------------------+----------------------------------------------+
| ``*PCAP.``\ field\ ``=``      | Position capture actions.  *field* can be    |
|                               | either ``ARM``, or ``DISARM``.               |
+-------------------------------+----------------------------------------------+
| ``*SAVESTATE=``               | Triggers immediate save to file of the       |
|                               | persistence file state.                      |
+-------------------------------+----------------------------------------------+
| ``*CLOCK_FREQ?``              | Returns currently configured system clock    |
|                               | frequency                                    |
+-------------------------------+----------------------------------------------+

``*IDN?``
    Returns system identification string, for example the following::

        OK =PandA SW: 1.1 FPGA: 0.1.9 d1275f61 00000000 rootfs: PandA 1.1

    The first field after "PandA" is the software version, the second field is
    the FPGA version, the third the firmware build number, and the fourth field
    identifies the supporting firmware.  The final fields (prefixed ``rootfs:``)
    identify the underlying system on which the server is running.

    Note that the ``rootfs:`` identification is new to version 1.1 of PandA.

``*ECHO string?``
    Returns string back to caller.  Not terribly useful.  Note that the echoed
    string cannot contain any of ``?``, ``=`` or ``<`` characters, as this would
    cause the command to be mistaken for another command format!  Example
    usage::

        < *ECHO This is a test?
        > OK =This is a test

``*WHO?``
    Returns list of client connections, for example::

        < *WHO?
        > !2015-12-04T14:30:40.403Z config 127.0.0.1:34185
        > .

    The first field is the time the connection was made, the second field is
    either ``config`` or ``data`` depending on whether the configuration or data
    port is connected, and the third field is the remote IP address and socket.

``*BLOCKS?``
    Returns a list of all the top level blocks in the system.  The order in
    which the blocks is returned is somewhat arbitrary.  For example (here the
    list has been shortened in the middle)::

        < *BLOCKS?
        > !TTLIN 6
        > !OUTENC 4
        ...
        > !CLOCKS 1
        > !BITS 1
        > !QDEC 4
        > .

    Block and field commands can be used to interrogate each block.  The number
    after each block records the number of instances of each block.

| ``*DESC.``\ block\ ``?``
| ``*DESC.``\ block\ ``.``\ field\ ``?``
| ``*DESC.``\ block\ ``.``\ field\ ``.``\ attr\ ``?``
| ``*DESC.``\ block\ ``.``\ field\ ``[].``\ subfield\ ``?``

    Returns description string for specified block, field, attribute, or table
    subfield eg::

        < *DESC.TTLIN?
        > OK =TTL input
        < *DESC.TTLIN.TERM?
        > OK =Select TTL input termination
        < *DESC.TTLIN.TERM.INFO?
        > OK =Class information for field

| ``*ENUMS.``\ block\ ``.``\ field\ ``?``
| ``*ENUMS.``\ block\ ``.``\ field\ ``.``\ attr\ ``?``
| ``*ENUMS.``\ block\ ``.``\ field\ ``[].``\ subfield\ ``?``

    Returns list of enumerations for given field, attribute, or table subfield,
    if appropriate.

| ``*CHANGES?``
| ``*CHANGES.CONFIG?``
| ``*CHANGES.BITS?``
| ``*CHANGES.POSN?``
| ``*CHANGES.READ?``
| ``*CHANGES.ATTR?``
| ``*CHANGES.TABLE?``

    Reports changes to the appropriate group of values.  Changes are reported
    since the last request on the connection, and on the first request the
    current value for every field will be reported.  The ``*CHANGES?`` command
    reports changes for all groups, otherwise one of the following groups can be
    selected:

    ======= ====================================================================
    CONFIG  Configuration settings
    BITS    Bits on the system bus
    POSN    Positions
    READ    Polled read values
    ATTR    Attributes (included capture enable flags)
    TABLE   Table changes
    ======= ====================================================================

    For example::

        < *CHANGES.CONFIG?
        > !TTLIN1.TERM=High-Z
        > !TTLIN2.TERM=50-Ohm
        > !TTLIN3.TERM=High-Z
        ...
        > !QDEC2.B=TTLIN1.VAL
        > !QDEC3.B=TTLIN1.VAL
        > !QDEC4.B=TTLIN1.VAL
        > .

    Here 804 (at the time of writing) lines have been deleted from the
    transcript!  Now if we repeat the call we see that no further changes have
    happened until something is actually changed::

        < *CHANGES.CONFIG?
        > .
        < TTLOUT4.VAL=TTLIN3.VAL
        > OK
        < *CHANGES.CONFIG?
        > !TTLOUT4.VAL=TTLIN3.VAL
        > .

    Note that for tables only the fact that the table has changed is shown, no
    attempt is made to show the current table value::

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

| ``*CHANGES=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.CONFIG=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.BITS=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.POSN=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.READ=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.ATTR=``\ [``E``\ | ``S``\ ]
| ``*CHANGES.TABLE=``\ [``E``\ | ``S``\ ]

    These commands reset the change information for the corresponding group of
    information so that only changes occuring after the reset are reported, or
    so that all changes are reported.  If ``=`` or ``=E`` (for End) is specified
    then only new changes are reported, if ``=S`` (for Start) then change
    reporting is reset to the start as for a new connection.  For example::

        < TTLIN1.TERM=50-Ohm
        > OK
        < *CHANGES=
        > OK
        < *CHANGES.CONFIG?
        > .

``*CAPTURE?``
    This returns a list of all positions and bit masks that will be written to
    the data capture port.  This list is controlled by setting the ``.CAPTURE``
    attribute on the corresponding position fields.

``*CAPTURE.*?``
    This returns a list of all fields that can be configured for capture.  This
    includes all ``pos_out`` and ``ext_out`` fields.

``*CAPTURE.OPTIONS?``
    Lists the available capture options for ``pos_out`` fields.  The available
    options are "Value", "Diff", "Sum", "Mean, "Min", "Max", "StdDev".
    Availability of the last option "StdDev" depends on the FPGA configuration.

``*CAPTURE.ENUMS?``
    Generates a curated list of capture option selections.  This is designed to
    be used for presenting lists of available capture options as an enumeration.
    Returns the same as calling ``*ENUMS.``\ name\ ``.``\ field\ ``.CAPTURE?``
    on any ``pos_out`` field.

``*CAPTURE=``
    This resets all ``.CAPTURE`` flags to zero so that no data will be captured.

``*POSITIONS?``
    This command lists all available position capture fields in order.

``*BITS?``
    This command lists all available bit bus positions, but not including the
    special values ``ZERO`` and ``ONE``.

``*VERBOSE=``\ value
    If ``*VERBOSE=1`` is set then every command will be echoed to the server's
    log.  Set ``*VERBOSE=0`` to restore normal quiet behaviour.

| ``*PCAP.STATUS?``
| ``*PCAP.CAPTURED?``
| ``*PCAP.COMPLETION?``

    Interrogates status of position capture:

    =========== ================================================================
    STATUS      Returns string with three fields: "Busy" or "Idle", followed by
                the number of connected readers, and the number taking data.
    CAPTURED    Returns number of samples captured in the current or most recent
                data capture.
    COMPLETION  Returns completion status from most recent data capture, as
                listed in the table below.
    =========== ================================================================

    The completion codes have the following meaning:

    =================== ========================================================
    Busy                Capture in progress.
    Ok                  Capture completed without error or intervention.
    Disarmed            Capture was manually disarmed by ``*PCAP.DISARM=``
                        command.
    Framing error       Data capture framing error, probably due to incorrectly
                        configured capture.
    DMA data error      Internal data error, should not occur.
    Driver data overrun Data capture too fast, internal buffers overrun.  Can
                        also occur if PandA processor overloaded.
    =================== ========================================================

| ``*PCAP.ARM=``
| ``*PCAP.DISARM=``

    Top level capture control:

    =========== ================================================================
    ARM         Initiates data capture.  Will fail if capture already in
                progress, or no fields configured for capture.
    DISARM      Halts ongoing data capture.
    =========== ================================================================

``*SAVESTATE=``
    Updates the persistence state file (as configured on the command line when
    launched) with the current state.  Returns after a file system ``sync``
    call, so it is safe to power-off the system after this command has
    completed.

``*CLOCK_FREQ?``
    Returns currently configured FPGA clock frequency as used to convert between
    times in natural units and times in clock ticks.
