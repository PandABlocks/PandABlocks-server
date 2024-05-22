import subprocess
import sys

from PandABlocks_server import __version__


def test_cli_version():
    cmd = [sys.executable, "-m", "PandABlocks_server", "--version"]
    assert subprocess.check_output(cmd).decode().strip() == __version__
