#!/usr/bin/env python3
"""Builds weather.papp with the SDK of an ESP32_P4_Launcher checkout:
    python build.py [path to ESP32_P4_Launcher]
The path defaults to $P4_LAUNCHER, then ../ESP32_P4_Launcher. Output: build/weather.papp (+ icon.png)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
launcher = (sys.argv[1] if len(sys.argv) > 1 else
            os.environ.get("P4_LAUNCHER", os.path.join(HERE, "..", "ESP32_P4_Launcher")))
tools = os.path.join(os.path.abspath(launcher), "papp-sdk", "tools")
if not os.path.isfile(os.path.join(tools, "papp_build.py")):
    sys.exit(f"no launcher SDK at {launcher} (pass the path to an ESP32_P4_Launcher checkout)")
sys.path.insert(0, tools)
import papp_build  # noqa: E402

papp_build.build("weather", [os.path.join(HERE, "main.c")], includes=[HERE], libc=True,
                 out_dir=os.path.join(HERE, "build"))
