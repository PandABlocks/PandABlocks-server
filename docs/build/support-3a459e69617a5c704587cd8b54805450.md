# Supporting tools and LEDs

## Useful tools

Several supporting tools are found in the `python/` directory of the server source.

`sim_server`
: Runs as part of the top-level `simserver` script to provide emulation of the
  PandA hardware. The version supplied with the server is minimal; for a more
  functional emulation use the corresponding tool in the PandABlocks-FPGA project.

`tcp_client [server [port]]`
: Connects to the PandA server configuration port and helps with sending and
  receiving configuration commands.

`save-state server file`
: Saves the entire configuration state for the given PandA to the given file.

`load-state server file`
: Writes the given configuration file to the PandA.

## PandA status LEDs

Two LEDs on PandA give a rough indication of its current status: **STA** (status,
green) indicates normal activity, and **DIA** (diagnostic, red) indicates fault
conditions.

| Mnemonic   | DIA   | STA   | Meaning                            |
|------------|-------|-------|------------------------------------|
| —          | Off   | Off   | System not running                 |
| BOOTING    | Off   | Blink | PandA booting                      |
| SYSTEM_OK  | Off   | On    | PandA running OK                   |
| ATTENTION  | Blink | Off   | User attention required            |
| NW_ERR     | Blink | Blink | Network problem detected           |
| —          | Blink | On    | (not used, should not occur)       |
| ZPKG_ERR   | On    | Off   | Problem loading installed package  |
| SYSTEM_ERR | On    | Blink | System error                       |
| —          | On    | On    | (not used, should not occur)       |

`BOOTING`
: The system is currently booting. Unless a new image is being configured this
  should only take a few seconds; during image installation it can take a few
  minutes.

`SYSTEM_OK`
: Booting has completed and the system is running normally.

`ATTENTION`
: User attention is required. Either a fresh installation is prompting for a MAC
  address, or no system packages have been installed. Connect a serial port in the
  first case, or connect to the administration web page on port 8080 in the second.

`NW_ERR`
: A network error has been detected. (Not currently implemented.)

`ZPKG_ERR`
: An installed package has failed to start. Try power-cycling PandA first; if that
  fails, check the logs and serial port for relevant diagnostic messages.

`SYSTEM_ERR`
: An internal system error has been detected. (Not currently implemented.)
