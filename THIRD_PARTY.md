# Third-party code and licensing

NFSCVITA's own code is licensed under the MIT License ([LICENSE](LICENSE)). The parts below come from other
projects and keep their own terms.

## pefile

`tools/recompiler/disasm/pefile.py` and `tools/recompiler/disasm/ordlookup/`, from the pefile project (Ero Carrera,
version 2018.8.8), MIT License. The license text is in `tools/recompiler/disasm/LICENSE.pefile`.

## Capstone

The recompiler disassembles with Capstone (BSD 3-Clause License), version pinned in
`tools/recompiler/requirements.txt`. It is not part of the repository: pip or `tools/recompiler/bootstrap.py`
installs it into `.pydeps/`.

## LLVM profile runtime

The instrumented build (`tools/build.ps1 -PgoGenerate`) links compiler-rt's profile runtime (LLVM Project, Apache
License v2.0 with LLVM Exceptions) with `pgo/InstrProfilingPlatformVita.c` from Grarak's
[psp2-tooling](https://github.com/Grarak/psp2-tooling) at commit `bed2efdf4e7c3f83e58c00fa4f2da09c6441d053`, under the
same license. Neither is part of the repository: `cmake/pgo-runtime.cmake` downloads them.

## Need for Speed Carbon

Game code, binaries and assets are the property of Electronic Arts Inc. and its licensors and are not part of this
repository. Users supply their own copy of the game; the code generated from its executable (`src/nfsc/generated/`)
is not tracked.
