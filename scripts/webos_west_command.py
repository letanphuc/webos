# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""West commands for building and testing WebOS on physical hardware."""

import subprocess
import time
from pathlib import Path

from west.commands import WestCommand


class WebosWestCommand(WestCommand):
    def __init__(self):
        super().__init__(
            "webos",
            "build, flash, and test WebOS",
            "Build, flash, and run WebOS integration tests on the physical development device.",
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name, help=self.help, description=self.description
        )
        subparsers = parser.add_subparsers(dest="webos_action", required=True)
        test_parser = subparsers.add_parser(
            "test", help="build, flash, and run physical-device integration tests"
        )
        test_parser.add_argument("--gpio-pin", type=int, default=2)
        test_parser.add_argument("--led-pin", type=int, default=48)
        test_parser.add_argument(
            "--startup-timeout",
            type=int,
            default=45,
            help="seconds to wait for the HTTP service after flashing (default: 45)",
        )
        return parser

    def do_run(self, args, unknown_args):
        if unknown_args:
            self.die(f"unknown arguments: {' '.join(unknown_args)}")
        if args.webos_action == "test":
            self._test(args)

    def _test(self, args):
        repo = Path(__file__).resolve().parent.parent
        workspace = repo.parent
        wdb = Path.home() / ".cargo" / "bin" / "wdb"

        if not wdb.is_file():
            self.die(f"wdb not found: {wdb}")

        self.inf("Building and flashing the physical device through wdb")
        self._run(
            [str(wdb), "run", "--timeout", str(args.startup_timeout)], workspace
        )

        self.inf("Waiting for the device HTTP service")
        root_listing = self._wait_for_device(
            wdb, workspace, args.startup_timeout
        )
        self._require(root_listing, "gpio/", "/dev listing")
        self._require(root_listing, "led/", "/dev listing")

        self.inf("Testing devfs directory traversal")
        gpio_dir = f"/dev/gpio/{args.gpio_pin}/"
        led_dir = f"/dev/led/{args.led_pin}/"
        gpio_listing = self._wdb(
            wdb, workspace, "shell", "fs", "ls", gpio_dir
        )
        self._require(gpio_listing, "direction", gpio_dir)
        self._require(gpio_listing, "value", gpio_dir)
        led_listing = self._wdb(wdb, workspace, "shell", "fs", "ls", led_dir)
        self._require(led_listing, "color", led_dir)

        self.inf("Testing GPIO file writes and readback")
        gpio_value = f"/dev/gpio/{args.gpio_pin}/value"
        try:
            self._wdb(
                wdb, workspace, "shell", "fs", "write", gpio_value, "01"
            )
            high = self._wdb(
                wdb, workspace, "shell", "fs", "cat", gpio_value
            )
            self._require(high, "1", f"{gpio_value} high readback")
        finally:
            self._wdb(
                wdb, workspace, "shell", "fs", "write", gpio_value, "00"
            )
        low = self._wdb(wdb, workspace, "shell", "fs", "cat", gpio_value)
        self._require(low, "0", f"{gpio_value} low readback")

        self.inf("Testing RGB LED file writes and readback")
        led_color = f"/dev/led/{args.led_pin}/color"
        try:
            self._wdb(
                wdb, workspace, "rgbled", "--pin", str(args.led_pin), "red"
            )
            red = self._wdb(wdb, workspace, "shell", "fs", "cat", led_color)
            self._require(red, "255,0,0", f"{led_color} red readback")
        finally:
            self._wdb(
                wdb, workspace, "rgbled", "--pin", str(args.led_pin), "off"
            )

        self.inf("Testing WAMR applications")
        self._wdb(wdb, workspace, "app", "run", "webos/sampleapps/hello")
        self._wdb(
            wdb,
            workspace,
            "app",
            "run",
            "webos/sampleapps/led_colors",
            "1",
        )
        off = self._wdb(wdb, workspace, "shell", "fs", "cat", led_color)
        self._require(off, "0,0,0", f"{led_color} final readback")

        self.inf("Physical-device WebOS tests passed")

    def _wait_for_device(self, wdb, workspace, timeout):
        deadline = time.monotonic() + timeout
        command = [str(wdb), "shell", "fs", "ls", "/dev/"]
        last_error = ""

        while time.monotonic() < deadline:
            result = subprocess.run(
                command, cwd=workspace, text=True, capture_output=True
            )
            if result.returncode == 0:
                print(result.stdout, end="")
                return result.stdout
            last_error = result.stderr.strip() or result.stdout.strip()
            time.sleep(2)

        self.die(f"device did not become ready within {timeout}s: {last_error}")

    def _wdb(self, wdb, workspace, *args):
        result = self._run([str(wdb), *args], workspace, capture_output=True)
        print(result.stdout, end="")
        return result.stdout

    def _run(self, command, cwd, capture_output=False):
        self.inf("+", " ".join(str(arg) for arg in command))
        result = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            capture_output=capture_output,
        )
        if result.returncode != 0:
            if capture_output:
                if result.stdout:
                    print(result.stdout, end="")
                if result.stderr:
                    print(result.stderr, end="")
            self.die(f"command failed with status {result.returncode}")
        return result

    def _require(self, output, expected, context):
        if expected not in output:
            self.die(f"expected {expected!r} in {context}: {output!r}")
