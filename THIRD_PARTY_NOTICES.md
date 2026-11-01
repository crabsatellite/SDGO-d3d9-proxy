# Third-party notices

## Unified Manager runtime

The self-contained Manager bundles Python (PSF license), Pillow (HPND license),
pyodbc (MIT), and, when present in the build, defusedxml (PSF license). Its executable
uses the PyInstaller bootloader (GPL with its distribution exception). Their full
license texts are bundled inside the EXE under `licenses/`. The build uses the
installed package versions; it does not include game resources or owner data.

The Windows/.NET framework used by the internal shop service is supplied by the
operating system and is not redistributed by this project.

## MinHook 1.3.4

Inline function hooking in `core.dll` uses MinHook (https://github.com/TsudaKageyu/minhook),
vendored unmodified in `third_party/minhook`.

* MinHook: Copyright (C) 2009-2017 Tsuda Kageyu. BSD 2-Clause License.
* Hacker Disassembler Engine 32 (part of MinHook): Copyright (c) 2008-2009 Vyacheslav Patkov.
  BSD 2-Clause License.

The full license text is `third_party/minhook/LICENSE.txt` (in the release package:
`licenses/MinHook-LICENSE.txt`).
