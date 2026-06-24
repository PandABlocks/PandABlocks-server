Supporting Documentation
========================

Useful Tools
------------

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


Panda Status LEDs
-----------------

Two LEDs on Panda show a rough indication of the current status of Panda.  There
are two LEDs, STA (status) and DIA (diagnostic); the status LED is green and is
used to indicate normal activity, the diagnostic LED is red and is used to
indicate various fault conditions.

The table below shows the possible LED indicators and their meaning:

=========== ======= ======= ====================================================
Mnemonic    DIA     STA     Meaning
=========== ======= ======= ====================================================
\-          Off     Off     System not running
BOOTING     Off     Blink   Panda booting
SYSTEM_OK   Off     On      Panda running ok
ATTENTION   Blink   Off     User attention required
NW_ERR      Blink   Blink   Network problem detected
\-          Blink   On      (not used, should not occur)
ZPKG_ERR    On      Off     Problem loading installed package
SYSTEM_ERR  On      Blink   System error
\-          On      On      (not used, should not occur)
=========== ======= ======= ====================================================

The detailed meaning of these conditions is described below.

BOOTING
    The system is currently booting.  Unless a new image is being configured
    this should only take a few seconds, but during image installation this can
    take a few minutes.

SYSTEM_OK
    Booting has completed and the system is running normally.

ATTENTION
    User attention is required.  Either a fresh installating is prompting for a
    MAC address, or no system packages have been installed.  Connect a serial
    port in the first case, connect to the administration web page on port 8080
    in the second case.

NW_ERR
    A network error has been detected.

    This function is not currently implemented.

ZPKG_ERR
    An installed package has failed to start.  Try power-cycling Panda first, if
    that fails check the logs and the serial port for relevant diagnostic
    messages.

SYSTEM_ERR
    An internal system error has been detected.

    This function is not currently implemented.
