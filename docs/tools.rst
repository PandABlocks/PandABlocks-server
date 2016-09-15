Useful Tools
============

There are a number of useful tools in the ``python`` directory.

``sim_server``
    This is run as part of the top level ``simserver`` script to provide
    emulation of the Panda hardware.  The version of this tool provided with the
    server is *very* basic, for a more functional emulation use the
    corresponding tool in the PandaFPGA project.

``tcp_client`` [server [port]]
    This tool connects to the Panda server configuration port and helps with
    sending and receiving configuration commands.

``save-state`` server file
    This saves the entire configuration state for the given Panda to the given
    file.

``load-state`` server file
    This writes the given configuration file to the Panda.
