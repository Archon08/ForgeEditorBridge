@echo off
REM Prefer the stack venv's Python (has tree-sitter-cpp, networkx, pyyaml),
REM fall back to system Python if the venv isn't built yet.
if exist "%~dp0.venv\Scripts\python.exe" (
    "%~dp0.venv\Scripts\python.exe" "%~dp0bridge-stack" %*
) else (
    python "%~dp0bridge-stack" %*
)
