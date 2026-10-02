#!/usr/bin/env python3
"""Extract the GLSL sources embedded in src/Shaders.hpp and validate them with glslangValidator."""
import re, subprocess, sys, tempfile, pathlib
src = pathlib.Path(__file__).parent / "src" / "Shaders.hpp"
ok = True
for name, body in re.findall(r'\{"([\w.]+)", R"GLSL\(\n(.*?)\)GLSL"\}', src.read_text(), re.S):
    with tempfile.NamedTemporaryFile("w", suffix=".frag", delete=False) as f:
        f.write(body); path = f.name
    r = subprocess.run(["glslangValidator", "-S", "frag", path], capture_output=True, text=True)
    out = (r.stdout + r.stderr).replace(path, name).strip()
    print(f"{name}: {'OK' if r.returncode == 0 else 'FAILED'}")
    if r.returncode != 0: print(out); ok = False
sys.exit(0 if ok else 1)
