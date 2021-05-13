Data Capture
============

Capture Configuration
---------------------

Both ``pos_out`` and ``ext_out`` fields can be configured for data capture
through the data capture port by setting the appropriate value in the
``CAPTURE`` attribute.  The possible capture settings depend on the field type
as follows:

``pos_out``
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

``ext_out``
    =============== ========================================================
    Value           Description
    =============== ========================================================
    No              This field will not be captured.
    Value           This field will be captured.
    =============== ========================================================


Data Capture Port
-----------------

The default server port for the data interface is port 8889.  The initial
exchange is in ASCII with newline separated lines, subsequent data communication
is as selected in the initial connection.

Data capture proceeds as follows:

1.  Connection to the data server port, default 8889.

2.  Send capture options string followed by newline.  The newline character is
    mandatory.

3.  The server will respond with ``OK`` unless there was an error parsing the
    capture options, or if the ``NO_STATUS`` option was specified.  If there was
    an error then the server responds with ``ERR`` followed by an error message
    and the connection is closed.

4.  The server will now ignore all further input from the client, and the
    connection will pause until data capture is started via the ``*PCAP.ARM=``
    command.

5.  At the beginning of a round of data capture or "experiment", a header
    detailing the data to be sent and data format is sent in ASCII followed by
    an empty line.  If ``NO_HEADER`` was selected then the header and blank line
    are omitted.

6.  Captured data is sent in the requested format until the experiment is
    complete (either internally disarmed or disarmed via the ``*PCAP.DISARM=``
    command), or there is a communication problem.

7.  At the end of the experiment a completion code is sent as a single line in
    ASCII starting with ``END``, unless ``NO_STATUS`` was specified.

8.  Unless ``ONE_SHOT`` was specified the server will pause until the next
    experiment (step 4).


Capture Options
~~~~~~~~~~~~~~~

A line of capture options *must* be sent after initial connection before any
data will be sent.  This is a list of any of the following options separated by
whitespace ending with a newline character.

=========== ================================================================ = =
ASCII       Specifies that data is to be sent as ASCII numbers.              1 D
BASE64      Binary data will be sent as a stream of base64 strings.          1
FRAMED      Binary data is sent as a sequence of sized frames.               1
UNFRAMED    Binary data is sent as a raw stream of bytes.                    1 R
SCALED      All scalable data is scaled and sent as doubles.                 2 D
UNSCALED    Averages are calculated but all values are sent as integers.     2 R
RAW         The captured binary data is sent without processing.             2
NO_HEADER   The data header is omitted.                                        R
NO_STATUS   The connection and end of experiment status strings are            R
            omitted.
ONE_SHOT    Only one experiment will be transmitted.                           R
XML         The header will be sent in XML format.
BARE        Selects ``UNFRAMED UNSCALED NO_HEADER NO_STATUS ONE_SHOT``
DEFAULT     Default options.                                                   D
=========== ================================================================ = =

Key:
    :D: Default option if no other option specified.
    :R: Options selected in response to ``BARE`` option.
    :1: Data transmission formats, one of these will be selected.
    :2: Data processing formats, one of these will be selected.


Data Transport Formatting
~~~~~~~~~~~~~~~~~~~~~~~~~

Note that all binary data is sent with the lowest order byte first.

``ASCII``
    Each value is formatted as an ASCII number, and transmitted with one line
    per captured sample.

``BASE64``
    The stream of binary data is converted to base64 strings and transmitted as
    a series of lines until the experiment is complete.  Each base64 string is
    preceded by a single space, so the end of the stream is easy to identify.

``FRAMED``
    In ``FRAMED`` mode the captured binary data is sent in blocks of
    unpredictable size.  Each block is preceded by 8 bytes.  The first four
    bytes are ``BIN`` followed by space, the remainind four bytes are the length
    of the data block in bytes *including* the 8 byte header.

``UNFRAMED``
    In ``UNFRAMED`` mode the captured binary data is sent as is.  In this mode
    it is difficult or impossible to reliably detect the end of the data stream,
    so normally this is best combined with ``NO_STATUS`` and ``ONE_SHOT``.


Data Header
~~~~~~~~~~~

At the beginning of each experiment the following information is sent:

=============== ================================================================
missed          Number of samples missed by late data port connection.
process         Data processing option: Scaled, Unscaled, or Raw.
format          Data delivery formatting: ASCII, Base64, Framed, or Unframed.
sample_bytes    Number of bytes in one sample unless ``format`` is ``ASCII``.
fields          Information about each captured field.
=============== ================================================================

For each field the following information is sent:

=============== ============================================================== =
name            Name of captured field.
type            Data type of transmitted field after data processing.
capture         Value of ``CAPTURE`` field used to enable this field.
scale           Scaling factor if scaled field.                                S
offset          Offset if scaled field.                                        S
units           Units string if scaled field.                                  S
=============== ============================================================== =

Key:
    :S: Only present if scaled field

If the ``XML`` option is selected the header is structured as a single
``header`` element containing ``data`` and ``fields`` elements.

The ``type`` field can be one of the following strings:

=========== ======= ============================================================
String      Bytes   Description
=========== ======= ============================================================
int32       4       Used for scalable values sent in unscaled modes.
uint32      4       Used for bit masks.
int64       8       Used for raw ADC mean and unscaled 48-bit encoder data.
double      8       Used for all scaled values when ``SCALED`` selected.
=========== ======= ============================================================


Experiment Completion
~~~~~~~~~~~~~~~~~~~~~

At the end of each capture experiment a single line is sent, eg::

    END 10 Ok

This specifies the number of samples sent and gives a completion code, which can
be one of the following values:

=================== ============================================================
Ok                  Experiment completed without intervention.
Disarmed            Experiment manually completed by ``*PCAP.DISARM=`` command.
Early disconnect    Client disconnect detected.
Data overrun        Client not taking data quickly or network congestion,
                    internal buffer overflow.
Framing error       Triggers too fast for configured data capture.
Driver data overrun Probable CPU overload on PandA, should not occur.
DMA data error      Data capture too fast for memory bandwidth.
=================== ============================================================

High performance mode
~~~~~~~~~~~~~~~~~~~~~

To get the highest performance, use ``FRAMED RAW`` mode. This activates a
special passthrough mode which avoids copying memory as much as possible. In
tests it has been capable of sustaining 60MBytes/s when panda-webcontrol is not
installed. The downside to this mode is that if capture fails for any reason,
then the last Framed block of data that the server sent should be discarded as
it will have been corrupted while being sent.


Examples
~~~~~~~~

Some examples of data capture for different options follow:

Default::

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

``BASE64``::

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

``XML``::

    <header>
    <data missed="0" process="Scaled" format="ASCII" />
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
