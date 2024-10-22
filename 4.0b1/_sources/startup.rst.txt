Starting Panda Server
=====================

The Panda socket server is normally automatically started at boot time or when
the ``zpkg-daemon`` script is run.  The server is started and stopped by the
script ``etc/panda-server`` installed in ``/opt/etc/init.d``.

The server can optionally be started from the command line, in which case the
following arguments are supported:

``-h``
    This option shows the help text for server, listing all the available
    command line options.

``-p`` port
    This specifies the socket port to be used for configuration commands.  The
    default value is 8888.

``-d`` port
    This specifies the socket port to be used for data capture.  The default
    value is 8889.

``-R``
    This can be specified to allow socket reuse via the ``SO_REUSEADDR`` socket
    option.

``-c`` config-dir
    This specifies the directory where the ``config``, ``registers``, and
    ``description`` files will be loaded from.  This argument must be specified.

``-f`` persistence-file
    This specifies where the persistence state will be loaded from on startup
    and saved during operation.  See the ``-t`` option below for notes on how
    this file is updated.  If this is not specified then the persistence state
    will not be saved.

``-t`` [poll] [":" [holdoff] [":" backoff]]
    This option sets three parameters (in seconds) controlling the pacing of
    writes to the persistence file.  The behaviour of the system is as follows:
    every `poll` seconds the internal state of the server is checked for
    configuration changes.  If a configuration change is checked then there is a
    pause of a further `holdoff` seconds before the updated state is written.
    Finally, there is a pause of `backoff` seconds before polling for internal
    changes resumes.

    Default values are: `poll` = 2, `holdoff` = 10, `backoff` = 60.  The
    somewhat complex syntax show above allows all or any of these values to be
    set with a single ``-t`` option.  For example, ``-t:20`` specifies `holdoff`
    = 20, other values unchaged.

    The intention of this timed behaviour is to reduce file write impact while
    still keeping on top of changes.  With default settings all parameters will
    be written to the persistence file within 72 seconds.

``-D``
    This option requests that the server is run as a daemon.  This is the normal
    mode of operation when running as a server, but is generally omitted for
    debug.

``-p`` pid-file
    If specified the given file is written with the process ID of the server,
    and will be deleted on exit.

``-T``
    This mode is used for config file validation only: the server exits
    immediately after loading configuration files.

``-M`` MAC-list
    If specified then the given file is used to initialise up to four MAC
    address registers.  The file consists of any number of comment
    lines (comment lines start with ``#`` in the first column) together with
    four MAC address lines, each of which is either blank (newline ``\n`` only)
    or is a six octet MAC address written as 2 digit hex numbers separated by
    colons.

``-X`` port
    If specified the server will attempt to connect to an extension server
    running locally and serving on the specified port.
