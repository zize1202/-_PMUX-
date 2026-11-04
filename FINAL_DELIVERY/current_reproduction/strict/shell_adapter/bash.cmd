@echo off
if not defined PMUX_SH_EXE (
  echo Set PMUX_SH_EXE to the POSIX sh executable. 1>&2
  exit /b 2
)
"%PMUX_SH_EXE%" %*
exit /b %errorlevel%
