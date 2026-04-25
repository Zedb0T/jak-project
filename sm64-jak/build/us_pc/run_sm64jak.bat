@echo off
set JAK_DATA_PATH=C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project
REM Pick which SDL controller to use. 0 = first, 1 = second, etc.
REM Check controllers.txt next to launch_sm64jak.bat after launching
REM for the detected list and per-controller GUIDs.
set JAK_CONTROLLER_INDEX=0
start sm64.us.f3dex2e.exe
