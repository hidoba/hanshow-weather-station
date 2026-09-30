# PlatformIO post-script for the `ota` environment: pass ADMIN_PASSWORD from src/secrets.h to
# espota, so `pio run -e ota -t upload` authenticates like the firmware expects. It has to be a
# post-script because the platform's own build script sets UPLOADERFLAGS after pre-scripts run.
import os
import re
from pathlib import Path

Import("env")  # noqa: F821

secrets = Path(env["PROJECT_DIR"]) / "src" / "secrets.h"  # noqa: F821
m = re.search(r'#define\s+ADMIN_PASSWORD\s+"((?:[^"\\]|\\.)*)"', secrets.read_text()) if secrets.exists() else None
# Undo C string escapes (\" \\ \n \t) to get the password the firmware compares against.
password = re.sub(r"\\(.)", lambda e: {"n": "\n", "t": "\t"}.get(e.group(1), e.group(1)), m.group(1)) if m else ""
if password:
    # The upload command runs through SCons substitution and then a shell, either of which would
    # mangle "$", spaces or quotes in the password. Hand it over in an environment variable
    # instead, referenced in the shell's own syntax: cmd.exe on Windows expands %VAR%, POSIX
    # shells "$VAR" ("$$" is SCons' escape for "$"; the quotes stop word splitting).
    env["ENV"]["ESPOTA_AUTH"] = password  # noqa: F821
    ref = '"%ESPOTA_AUTH%"' if os.name == "nt" else '"$$ESPOTA_AUTH"'
    env.Append(UPLOADERFLAGS=["--auth=" + ref])  # noqa: F821
