..  _fields:

Blocks, Fields and Attributes
=============================

The set of hardware blocks can be interrogated with the ``*BLOCKS?`` command::

    < *BLOCKS?
    > !TTLIN 6
    > !OUTENC 4
    > !PCAP 1
    > !PCOMP 4
    > !TTLOUT 10
    > !ADC 8
    > !DIV 4
    > !INENC 4
    > !SLOW 1
    > !PGEN 2
    > !LVDSIN 2
    > !POSITIONS 1
    > !POSENC 4
    > !SEQ 4
    > !PULSE 4
    > !SRGATE 4
    > !LUT 8
    > !LVDSOUT 2
    > !COUNTER 8
    > !ADDER 1
    > !CLOCKS 1
    > !BITS 1
    > !QDEC 4
    > .

For each block the number after the block tells us how many instances there are
of the block.  Each block is controlled and interrogated through a number of
fields, and the *block*\ ``.*?`` command can be used to interrogate the list of
fields::

    < TTLIN.*?
    > !VAL 1 bit_out
    > !TERM 0 param enum
    > .

This tells us that block ``TTLIN`` has two fields, ``TTLIN.VAL`` and
``TTLIN.TERM``.  The first field after the field name is a sequence number for
user interface display, and the rest of each response describes the "type" of
the field.  In this case we see that ``TTLIN.VAL`` is a ``bit_out`` field, which
means can be used for bit data capture and can be connected to any ``param
bit_mux`` field as a data source.

Each field has one or more attributes depending on the field type.  The list of
attributes can be interrogated with the *block*\ ``.``\ field\ ``.*?`` command::

    < TTLIN.VAL.*?
    > !CAPTURE_WORD
    > !OFFSET
    > !INFO
    > .
    < TTLIN.TERM.*?
    > !INFO
    > .

All fields have the ``.INFO`` attribute, which just repeats the type information
already reported, eg ``TTLIN1.VAL.INFO?`` returns ``bit_out`` (note that a block
number must be specified when interrogating fields and attributes).


Field Types
-----------

Each field type determines the set of attributes available for the field.  The
types and their attributes are documented below.

=================== ============================================================
Field type          Description
=================== ============================================================
``param`` subtype   Configurable parameter.  The *subtype* determines the
                    precise behaviour and the available attributes.
``read`` subtype    A read only hardware field, used for monitoring status.
                    Again, *subtype* determines available attributes.
``write`` subtype   A write only field, *subtype* determines possible values
                    and attributes.
``time``            Configurable timer parameter.
``bit_out``         Bit output, can be configured as bit input for ``bit_mux``
                    fields.
``pos_out``         Position output, can be configured for data capture and as
                    position input for ``pos_mux`` fields.
``ext_out`` extra   Extended output values, can be configured for data capture,
                    but not available on position bus.
``bit_mux``         Bit input with configurable delay.
``pos_mux``         Position input multiplexer selection.
``table``           Table data with special access methods.
=================== ============================================================

``param`` subtype
    All fields of this type contribute to the ``*CHANGES.PARAM`` change group
    and are used to configure the behaviour of the corresponding block.  Fields
    of this type are used for input configuration and other behavioural
    settings.

``read`` subtype
    All fields of this type contribute to the ``*CHANGES.READ`` change group,
    but are only checked when either the field is read or the change group is
    polled.  Fields of this type are used for monitoring the internal status of
    a block, and they cannot be written to.

``write`` subtype
    Fields of this type can only be written and are used for immediate actions
    on a block.  The ``action`` subtype is used to support actions without any
    parameters, for example the following command forces a soft reset on the
    given pulse block::

        < PULSE1.FORCE_RESET=
        > OK

``time``
    Fields of this type are used for configuring delays.  They also contribute
    to ``*CHANGES.PARAM``.  The following attributes are supported by fields of
    this type:

    ``UNITS``
        This attribute can be set to any of the strings ``min``, ``s``, ``ms``,
        or ``us``, and is used to interpret how values read and written to the
        field are interpreted.

    ``RAW``
        This attribute can be read or written to report or set the delay in FPGA
        ticks.

    The ``UNITS`` attribute determines how numbers read or written to the field
    are interpreted.  For example::

        < PULSE1.DELAY.UNITS=s
        > OK
        < PULSE1.DELAY=2.5
        > OK
        < PULSE1.DELAY.RAW?
        > OK =312500000
        < PULSE1.DELAY.UNITS=ms
        > OK
        < PULSE1.DELAY?
        > OK =2500

    Note that changing ``UNITS`` doesn't change the delay, only how it is
    reported and interpreted.

``bit_out``
    Fields of this type are used for block outputs which contribute to the
    internal bit system bus, and they contribute to the ``*CHANGES.BITS`` change
    group.  They can be captured via the appropriate ``PCAP.BITS``\ n block as
    reported by the ``CAPTURE_WORD`` attribute.

    The following attributes are supported by fields of this type:

    ``CAPTURE_WORD``
        This identifies which ``pos_out`` value can be used to capture this bit.

    ``OFFSET``
        This is the bit offset into the captured word of this particular bit.

    For example::

        < TTLIN1.VAL.CAPTURE_WORD?
        > OK =PCAP.BITS0
        < TTLIN1.VAL.OFFSET?
        > OK =2

    This tells us that if ``PCAP.BITS0`` is captured then ``TTLIN1.VAL`` can be
    read as bit 2 of this word, counting from the least significant bit.

    The field itself can be read to return the current value of the bit.

``pos_out``
    Fields of this type are used for block outputs which contribute to the
    internal position bus, and they contribute to the ``*CHANGES.POSN`` change
    group.  The following attributes support capture control:

    ``CAPTURE``
        This can be set to manage capture of this field.  One of the following
        enumeration values can be written to this field:

        =============== ========================================================
        Value           Description
        =============== ========================================================
        No              Capture is disabled for this field.
        Value           The value at the time of trigger will be captured.
        Diff            The difference of values is captured.
        Sum             The sum of all valid values is captured.  This is a
                        64-bit value, and may be further scaled if
                        ``PCAP.SHIFT_SUM`` is set.
        Mean            The average of all valid values is captured.
        Min             The minimum of all valid values is captured.
        Max             The maximum of all valid values is captured.
        Min Max         Both minimum and maximum values are captured.
        Min Max Mean    All three values, minimum, maximum, average are
                        captured.
        =============== ========================================================

    The following attributes support formatting of the field when reading it:
    the current value is returned subject to the formatting rules described
    below.

    ``OFFSET``, ``SCALE``
        These numbers can be set to configure the conversion from the underlying
        position to the value captured when scaling is enabled and read from the
        ``SCALED`` attribute.

    ``UNITS``
        This field can be set to any UTF-8 string, and is provided for the
        convenience of the user interface and is returned as part of the data
        capture heading.

    ``SCALED``
        This returns the scaled value computed as

            value * scale + offset

``ext_out`` extra
    Fields of this type represent values that can be captured but which are not
    present on the position bus.  These fields also support one capture control
    field:

    ``CAPTURE``
        As for ``pos_out``, can be set to control capture of this field:

        =============== ========================================================
        Value           Description
        =============== ========================================================
        No              This field will not be captured.
        Value           This field will be captured.
        =============== ========================================================

    The *extra* field determines the detailed behaviour of this field, and will
    be one of the following values:

    =============== ============================================================
    extra value     Description
    =============== ============================================================
    ``timestamp``   Timestamps in clock ticks with optional scaling to seconds
                    on data capture.
    ``samples``     Special internal field for counting captured samples.
    ``bits``        Used to implement bit-bus readout fields.  Fields of this
                    sub-type implement an extra ``BITS`` field.
    =============== ============================================================

    Fields of type ``ext_out bits`` implement an extra attribute:

    ``BITS``
        This returns a list of all bit fields associated with this field.
        Fields of this type can be used to capture a snapshot of the bit bus at
        the trigger time.

``bit_mux``
    Bit input selectors for blocks.  Each of these fields can be set to the name
    of a corresponding ``bit_out`` field, for example::

        < TTLOUT1.VAL=TTLIN1.VAL
        > OK

    There are two attributes:

    ``DELAY``
        This can be set to any value between 0 and ``MAX_DELAY`` to delay the
        bit input to the block by the specified number of clock ticks.

    ``MAX_DELAY``
        This returns the maximum delay that can be set for this input.

``pos_mux``
    Position input selectors for blocks.  Each of these fields can be set to the
    name of a corresponding ``pos_out`` field, for example::

        < ADDER1.INPA=ADC2.OUT
        > OK

``table``
    Values of this type are used for long tables of numbers.  This server
    imposes no structure on these values apart from treating them as an array of
    32-bit integers.

    Tables values are written with the special ``<`` syntax:

    =================================== ========================================
    block number\ ``.``\ field\ ``<``   Normal table write, overwrite table
    block number\ ``.``\ field\ ``<<``  Normal table write, append to table
    block number\ ``.``\ field\ ``<B``  Base-64 table write, overwrite table
    block number\ ``.``\ field\ ``<<B`` Base-64 table write, append to table
    =================================== ========================================

    For "normal" table writes the data is sent as a sequence of decimal numbers
    in ASCII, and the whole sequence must be terminate by an empty blank line.
    For base-64 writes the data is sent in base-64 format, for example::

        < SEQ3.TABLE<B
        < TWFuIGlzIGRpc3Rpbmd1aXNoZWQsIG5vdCBvbmx5IGJ5IGhpcyByZWFzb24sIGJ1
        <
        > OK
        < SEQ3.TABLE.LENGTH?
        > OK =12

    Note that when data is sent in base-64 format, each individual line must
    encode a multiple of four bytes, otherwise the write will be rejected.

    The following attributes are provided by this field type:

    ``MAX_LENGTH``
        This is the maximum number of 32-bit words which can be stored in the
        table.

    ``LENGTH``
        This is the current number of words in the table.

    ``B``
        This read-only attribute returns the content of the table in base-64.

    ``FIELDS``
        This returns a list of strings which can be used to interpret the
        content of the table.  Each line returned is of the following format::

            left:right field-name subtype

        Here *left* and *right* are bit field indices into a single table row,
        consisting of a number of 32-bit words concatenated (in little-endian
        order) with bits numbered from 0 in the least significant position up to
        32*\ ``ROW_WORDS``-1, and *left* >= *right*.  The name of the field is
        given by  *field-name*, and *subtype* can be one of ``int``, ``uint``,
        or ``enum``.  If *subtype* is ``enum`` then the list of enums can be
        interrogated through the command ::

            *ENUMS.block.table[].field?

        where *block*, *table*, *field* are appropriate names.

    ``ROW_WORDS``
        Returns the number of 32-bit words in a single row of the table.  This
        can be used to help interpret the ``FIELDS`` result.


Field Sub-Types
---------------

The following field sub-types can be used for ``param``, ``read`` and ``write``
fields.

``uint`` [*max-value*]
    This is the most basic type: the value read or written is an unsigned 32-bit
    number.  There is one fixed attribute:

    ``MAX``
        This returns the maximum value that can be written to this field.

``int``
    Similar to ``uint``, but signed, and there is no upper limit on the value.

``scalar`` *scale* [*offset* [*units*]]
    Floating point values can be read or written, and are converted from and to
    the underlying signed integer type via the equations below:

        | value = scale * raw + offset
        | raw = (value - offset) / scale

    The following attributes are supported:

    ``UNITS``
        Returns the configured units string.

    ``RAW``
        Returns the underlying unconverted integer value.

    ``SCALE``
        Returns the configured scaling factor.

    ``OFFSET``
        Returns the configured scaling offset.

``bit``
    A value which is 0 or 1, there are no extra attributes.

``action``
    A value which cannot be read and always writes as 0.  Only useful for
    ``write`` fields.

``lut``
    This field sub-type is used for the 5-input lookup table function
    calculation field.  This field can be set to any valid logical expression
    generated from inputs ``A`` to ``E`` using the standard operators ``&``,
    ``|``, ``^``, ``~``, ``?:`` from C together with ``=`` for equality and
    ``=>`` for implication (``A=>B`` abbreviates ``~A|B``).  All operations have
    C precedence, ``=`` has the same precedence as ``==`` in C, and ``=>`` has
    precedence between ``|`` and ``?:``.

    The following attribute is supported:

    ``RAW``
        This returns the corresponding lookup table assignment as a 32-bit
        number.

    For example::

        < LUT2.FUNC=A=>B?C:D
        > OK
        < LUT2.FUNC?
        > OK =A=>B?C:D
        < LUT2.FUNC.RAW?
        > OK =0xF0CCF0F0


``enum``
    Enumeration fields define a list of valid strings which can be written to
    the field.  To interrogate the list of valid enumeration values use the
    ``*ENUMS`` command, for example::

        < *ENUMS.TTLIN1.TERM?
        > !High-Z
        > !50-Ohm
        > .

``time``
    Converts between time in specified units and time in FPGA clock ticks.  The
    following attributes are supported:

    ``UNITS``
        This attribute can be set to any of the strings ``min``, ``s``, ``ms``,
        or ``us``, and is used to interpret how values read and written to the
        field are interpreted.

    ``RAW``
        This attribute can be read or written to report or set the delay in FPGA
        ticks.


Summary of Sub-Types
--------------------

=========== =============== ====================================================
Sub-type    Attributes      Description
=========== =============== ====================================================
uint        MAX             Possibly bounded 32-bit unsigned integer value
int                         Unbounded 32-bit signed integer value
scalar      RAW, UNITS,     Scaled signed floating point value
            SCALE, OFFSET
bit                         Bit: 0 or 1
action                      Write only, no value
lut         RAW             5 input lookup table logical formula
enum        LABELS          Enumeration selection
time        RAW, UNITS      Time intervals converted to FPGA ticks
=========== =============== ====================================================


Summary of Attributes
---------------------

=============== =============== ======================================= = = = =
Field (sub)type Attribute       Description                             R W C M
=============== =============== ======================================= = = = =
(all)           INFO            Returns type of field                   R
uint            MAX             Maximum allowed integer value           R
scalar          RAW             Underlying integer value                R W
\               UNITS           Configured units for scalar             R
\               SCALE           Configured scaling factor for scalar    R
\               OFFSET          Configured scaling offset for scalar    R
lut             RAW             Computed Lookup Table 32-bit value      R
time            UNITS           Units and scaling selection for time    R W C
\               RAW             Raw time in FPGA clock cycles           R W
bit_out         CAPTURE_WORD    Capturable word containing this bit     R
\               OFFSET          Offset of this bit in captured word     R
bit_mux         DELAY           Bit input delay in FPGA ticks           R W C
\               MAX_DELAY       Maximum valid delay                     R
pos_out         CAPTURE         Position capture control                R W C
\               OFFSET          Position offset                         R W C
\               SCALE           Position scaling                        R W C
\               UNITS           Position units                          R W C
\               SCALED          Position after applying scaling         R
ext_out bits    BITS            List of bit_out fields                  R     M
table           MAX_LENGTH      Maximum table row count                 R
\               LENGTH          Current table row count                 R
\               B               Table data in base-64                   R     M
\               FIELDS          Table field descriptions                R     M
\               ROW_WORDS       Number of words in a table row          R
=============== =============== ======================================= = = = =

Key:
    :R:     Attribute can be read
    :W:     Attribute can be written
    :C:     Attribute contributes to ``*CHANGES.ATTR`` change set
    :M:     Attribute returns multiple value result.
