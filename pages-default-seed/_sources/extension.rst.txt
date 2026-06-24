..  _extension:

Extension Server
================

The extension server is used to implement custom fields.  The extension server
runs alongsize the Panda socket server and supports the loading of Python
modules with a lightweight remote procedure calling interface from the socket
server.

As described in :ref:`config`, individual `read`, `write`, `param` fields can be
specified to take their values from and write to the extension register.  At the
same time, these extension read and write operations can be linked to hardware
registers.

The extension server is started alongside the socket server, and on PandA looks
in ``/opt/share/panda-fpga/extensions`` for extension module files.  During
development any Python module can be specified as a container for extension
sub-modules.

Connections from the socket server to the extension server are established while
reading the `registers` file.  Firstly, if a block has an extension module
specified then this module is loaded from the extensions directory and is
associated with the block.  Next, each extension field is processing by calling
the appropriate :func:`parse_read` or :func:`parse_write` methods.

Depending on the register configuration any number of hardware block registers
can be read or written during processing of an extension field.


Extension Modules
-----------------

Each extension module must export a class constructor named `Extension`.  This
takes one argument and must support two methods `parse_read` and `parse_write`

..  class:: Extension(count)

    This class must be defined by each extension module.  The class will be
    instantiated in response to loading a block register definition of the
    form::

        block-name [ "S" ] block-register extension-module

    The parameter `count` is set to the number of instances specified for the
    block in the `config` file.  This class must provide the following methods,
    as appropriate, to support read and write register fields:

    ..  method:: parse_read(field_spec)

        This is called in response to a `read-extension` line in the register
        file of the form::

            [ read-reg ]* "X" field-spec

        The `field-spec` is passed as a string to :func:`parse_read`, and this
        method must return a callable of the following form:

        ..  function:: value = \
                read_register(block_num, read_reg1, ..., read_regN)

            The first argument `block_num` is the number of the block instance
            being called (starting from 0), and is guaranteed to be less than
            `count` as passed to the :class:`Extension` constructor.

            The remaining `read_reg1` ... `read_regN` argument must match the
            number of arguments specified in the `read-reg` block of the
            register file.  These will be populated by reading the corresponding
            block hardware registers before this function is called.

            The value returned must be a single integer, this is the value
            returned when reading this field.

    ..  method:: parse_write(field_spec)

        This is called in response to a `write-extension` line in the register
        file of the form::

            [ read-reg ]* [ "W" [ write-reg ]* ] "X" field-spec

        As for :func:`parse_read`, `field-spec` is passed as the argument, and a
        callable must be returned, of the following form:

        ..  function:: (write_reg1, ..., write_regM) = \
                write_register(block_num, value, read_reg1, ..., read_regN)

            The first argument `block_num` is the number of the block instance
            being called (starting from 0), and is guaranteed to be less than
            `count` as passed to the :class:`Extension` constructor.

            The second argument `value` is the value written to this field.

            The remaining `read_reg1` ... `read_regN` argument must match the
            number of arguments specified in the `read-reg` block of the
            register file.  These will be populated by reading the corresponding
            block hardware registers before this function is called.

            The value returned must be a tuple of integers matching the
            `write-reg` block of the register file.  The returned values will be
            written to the specified hardware registers after processing this
            function.  This defines the action of writing this field.


Injected Values
---------------

Every extension module will have two support values injected into the module
when the module is loaded into the server.  These are available to help with the
implementation of extensions.

..  class:: ServerError(Exception)

    Read and write methods should use this exception to report errors.
    Exceptions of this type are treated specially and are reported as normal
    read or write errors.

..  class:: ExtensionHelper

    This can be used inside an extension module to create extension support for
    individual fields.  Pass a block constructor (that must take one argument,
    the block index) which implements `set_` and `get_` methods as appropriate,
    and this helper will implement the approprate Extension support.

    Use this inside the extension module thus::

        class MyBlock:
            def __init__(self, n):
                ...

            def get_field(self, *regs):
                ...
                return value

            def set_field(self, value, *regs):
                ...
                return new_regs

        def Extension(count):
          return ExtensionHelper(MyBlock, count)
