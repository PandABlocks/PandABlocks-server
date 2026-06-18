# Start the PandA server

The PandA socket server normally starts automatically at boot time or when the
`zpkg-daemon` script is run. It is started and stopped by the script
`etc/panda-server` installed at `/opt/etc/init.d`.

The server can also be started from the command line with the following options.

`-h`
: Display help text listing all available command-line options.

`-p` *port*
: TCP port for configuration commands. Default: `8888`.

`-d` *port*
: TCP port for data capture. Default: `8889`.

`-R`
: Allow socket reuse via the `SO_REUSEADDR` socket option.

`-c` *config-dir*
: Directory from which the `config`, `registers`, and `description` files are
  loaded. **This argument must be specified.**

`-f` *persistence-file*
: File from which persistence state is loaded on startup and to which it is saved
  during operation. If not specified, persistence state is not saved.

`-t` *[poll][:holdoff[:backoff]]*
: Three parameters (in seconds) controlling writes to the persistence file.

  Every *poll* seconds the server checks for configuration changes. If a change
  is found, it waits a further *holdoff* seconds before writing. It then waits
  *backoff* seconds before resuming polling.

  Defaults: `poll` = 2, `holdoff` = 10, `backoff` = 60. With these defaults all
  parameters reach the persistence file within 72 seconds.

  The colon syntax lets you set any subset: for example `-t:20` sets *holdoff* = 20
  while leaving *poll* and *backoff* unchanged.

`-D`
: Run the server as a daemon. This is the normal production mode; omit for
  debugging.

`-P` *pid-file*
: Write the server process ID to the given file; the file is deleted on exit.

`-T`
: Config-file validation mode: the server exits immediately after loading
  configuration files without accepting connections.

`-M` *MAC-list*
: Initialise up to four MAC address registers from the given file. The file may
  contain comment lines (starting with `#`) and up to four MAC address lines,
  each either blank or a six-octet address in `XX:XX:XX:XX:XX:XX` format.

`-X` *port*
: Connect to an extension server running locally on the specified port. See
  [](/reference/extension.md) for details.

`-r` *rootfs-version*
: Specify the rootfs version string reported by the `*IDN?` command.
