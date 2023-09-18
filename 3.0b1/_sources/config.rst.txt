..  _config:

Configuration Files
===================

On startup the Panda Server loads its configuration from three files:
``config``, ``registers``, ``description``.  These are loaded from the
``config_d`` directory in build directory when running the basic simulation, and
on Panda the configuration files are loaded from ``/opt/share/panda/config_d``.

The syntax of each configuration file is documented here.  The format of each
field definition closely follows the format documented in :ref:`fields`.

The three files have the following distinct functions.

=============== ===============================================================
File            Description
=============== ===============================================================
``config``      Configuration: defines list of blocks, fields in each block,
                and the behaviour of each field.
``registers``   Registers: for each block and field defines the associated
                register offsets.
``description`` Description: optionally defines a description string for each
                block and field.
=============== ===============================================================

All files have a common structure and indentation is used for structure.
Comments begin with ``#``, blocks are identified by their name in the first
column, and fields are listed at the next level of indentation.


Configuration file ``config``
-----------------------------

This file defines all of the blocks and fields available to this instance of
Panda and is processed first.

The syntax of a block definition is::

    block-name [ "[" count "]" ]
        [ field-definition ]*

This means that a block definition consists of a `block-name`, which can be any
word used to name the block, optionally followed by a block `count` enclosed in
square brackets, followed by any number of indented `field-definition`\ s.

The syntax of a field definition is::

    field-name field-type [ field-type-data ]

The `field-name` names the field, the `field-type` is a single word from the
list documented below, and the `field-type-data` depended on the field type as
documented.

The `field type` determines the basic function of the field, what actions can
be performed on the field, and how the field interacts with the hardware.
Typically each field corresponds to a single register or function of the block.

Many field type have an associated `field subtype` which is used to convert
between the values show to the user of the server and the values written to or
read from registers.

Field type
~~~~~~~~~~

The following field types are defined.

============================================================================== =
Field
============================================================================== =
``param`` field-subtype [ ``=`` value ]
``read`` field-subtype
``write`` field-subtype
``time`` [ ``>`` min_value ]
``bit_out``
``pos_out`` [ scale [ offset [ units ]]]
``ext_out`` ( ``timestamp`` | ``samples`` | ``bits`` group )
``bit_mux`` [ ``=`` value ]
``pos_mux``
``table``
============================================================================== =

``param`` field-subtype [ ``=`` value ]
    A ``param`` field is used to define a single 32-bit value written to a
    register.  The `field-subtype` must be specified.  Optionally an initial
    value (only relevant when no state file has been loaded) can be specified.
    In this case the initial value is read as a raw unsigned integer which is
    written directly to the hardware on startup.

``read`` field-subtype
    A ``read`` field is used for read-only registers.  The `field-subtype` must
    be specified.

``write`` field-subtype
    A ``write`` field is used for write-only registers which trigger immediate
    actions on a block.  The `field-subtype` must be specified, and the `action`
    subtype is useful for ``write`` fields which take no data.

``time`` [ ``>`` min_value ]
    Time fields behave like ``param`` fields, but need special treatment because
    the underlying value is 64-bits and so two registers need to be written.

    If desired a minimum valid value can be specified as `min_value`.  This will
    prevent the writing of values less than this value and can be read as the
    ``.MIN`` attribute.

``bit_out``
    This identifies an output bit.

``pos_out`` [ scale [ offset [ units ]]]
    This identifies a position bus output.  Optionally default values for the
    scale, offset, and units fields can be specified in the config file.  Note
    that these are only effective when there is no persistence file to load.

``ext_out`` ext-extra
    This identifies an entry on the extension bus which needs special treatment.
    The `ext-extra` field must be one of the following values:

    =============== ============================================================
    ext-extra       Description
    =============== ============================================================
    ``timestamp``   Capture timestamp as a 64-bit value
    ``samples``     Captures sample count for data capture.
    ``bits`` group  Defines fields which allow the bit bus to be captured.  The
                    group number identifies which 32-bit group of 128 bits is
                    captured.
    =============== ============================================================

| ``bit_mux`` [ ``=`` value ]
| ``pos_mux``

    These two are configuration settings for selecting inputs, and behave like
    ``param`` fields.  As for ``param`` a default value can be assigned to
    ``bit_mux``, but the only useful value is 129 (``ONE``).

``table`` [ row-words ]
    Tables are treated specially.

Field subtype
~~~~~~~~~~~~~

The following field subtype can follow a ``param``, ``read`` or ``write`` field
type:

============================================================================== =
Type
============================================================================== =
``uint`` [ max-value ]
``int``
``scalar`` scale [ offset [ units ]]
``bit``
``action``
``lut``
``enum``
``position``
``time``
============================================================================== =

Note that ``enum`` must be followed by indented lines each consisting of a
number followed by a string: the string is the enumeration value written to
the user, the number is the value written to the register.


Register file ``registers``
---------------------------

This file defines the register assignments for each block and register.  The
body of this file should contain a sequencer of block and field definitions
repeating the ``config`` file, except that the field type specification is
replaced by a type specific register definition.

So a block definition is::

    block-name { [ "S" ] block-register | "X" } [ extension-module ]
        [ field-definition ]*

If the `block-register` number is prefixed with ``S`` then the same block
register number can be shared with multiple blocks: this allows a single
hardware implementation to be presented as multiple software blocks.  If ``X``
is used instead of specifying `block-register` then no fields can use registers,
so must be extension fields with no register linkage.

The register number can be followed by an `extension-module` which is used to
identify this block to the extension server, and will enable use of the
extension register syntax defined below.

A field definition is::

    field-definition = field-name register-specification

where `register-specification` depends on the associated field type as
follows:

======================= ========================================================
Class                   Register syntax
======================= ========================================================
``param``               register | write-extension
``read``                register | read-extension
``write``               register | write-extension
``time``                low-register high-register
``bit_out``             ( bit-index )N
``pos_out``             ( pos-index )N
``ext_out timestamp``   ext-index ext-index
``ext_out`` other       ext-index
``bit_mux``             register
``pos_mux``             register
``table``               ``short`` size init-reg fill-reg length-reg
``table``               ``long`` ``2^``\ size base-reg length-reg
======================= ========================================================

where the syntax ``(...)N`` means that the given register number is repeated N
times where N is the number of instances of the block.  See below for an
explanation of `read-extension` and `write-extension`.

Extension register syntax
~~~~~~~~~~~~~~~~~~~~~~~~~

If the extension server is enabled (with the ``-X`` command line option on the
server) and if an extension module has been loaded as part of the block
specification then ``param``, ``read``, and ``write`` subtypes can all be
redirected to the extension server using the `read-extension` and
`write-extension` syntax::

    read-extension = [ read-reg ]* "X" field-spec
    write-extension = [ read-reg ]* [ "W" [ write-reg ]* ] "X" field-spec

In this syntax `field-spec` is passed through to the associated
`extension-module` to create the binding between this field and the extension
server.  The specified `read-reg`\ s and `write-reg`\ s will be used when
processing this field.

See :ref:`extension` for more details on extension fields.


Description file ``description``
--------------------------------

The entire content of the description file is optional.  The basic syntax is::

    block-name block-description
        [ field ]*

where field is::

    field-name field-description

and the description is any newline terminated string in UTF-8 format.
