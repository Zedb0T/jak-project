SM64-Jak  -  Jak and Daxter in Super Mario 64
================================================

Jak - the real Jak, running on his original PS2 game engine - playable
inside Super Mario 64. The full OpenGOAL (Jak PC port) runtime runs inside
the SM64 process and drives Jak's movement, physics, animation and sounds,
while SM64 provides the world.

NO GAME DATA IS INCLUDED IN THIS DOWNLOAD.
You must own both games. Setup builds everything locally from YOUR copies:
  - Super Mario 64 (US) ROM         -> baserom.us.z64 (~8 MB)
  - Jak and Daxter (NTSC-U) ISO

FIRST-TIME SETUP (one time, ~15-25 minutes):
  1. Run "Setup.bat". It will:
     - find your SM64 ROM automatically if another OpenGOAL Mario mod
       already stored one (%APPDATA%\OpenGOAL\mario), else ask for it
       and store it there for reuse
     - install the MSYS2 compiler environment if needed (via winget)
     - build the SM64-Jak game from source with your ROM
     - reuse your OpenGOAL Launcher's Jak 1 data automatically if
       installed, else ask for your Jak 1 ISO and build the data from it
  2. Run "Play SM64-Jak.bat"

REQUIREMENTS:
  - Windows 10/11 64-bit
  - GPU with OpenGL 4.3 support (for the Jak world view)
  - ~6 GB free disk space during setup
  - Internet connection for the first-time compiler install

CONTROLS:
  Standard SM64 controls -- Jak replaces Mario. Jak can run, jump, spin,
  punch, dive, ground pound, swim and dive underwater.
  Mario still handles: cannons, wing cap, shell, poles.

SPECIAL KEYS:
  Left Alt ........ ImGui menu: Jak-world picture-in-picture toggle,
                    debug fly (hold R2), level warp, controller swap
  Hold W .......... send controller input to the Jak world (navigate
                    Jak's own pause/debug menus in the PiP)
  Hold X + D-pad Right ... in-game Jak menu (fallbacks + warps)

CONTROLLERS:
  Plug in before launching if possible. To pick a specific pad, use
  Left Alt -> Controllers in game.

TROUBLESHOOTING:
  - Setup fails at [3/4]: make sure MSYS2 finished installing, then
    re-run Setup.bat (it resumes where it left off).
  - Black picture-in-picture: your GPU/driver must support OpenGL 4.3.
  - Wrong controller: Left Alt -> Controllers, click the right one.

CREDITS:
  OpenGOAL (open-goal/jak-project, ISC license), sm64ex (sm64pc).
  Jak and Daxter is property of Sony Interactive Entertainment /
  Naughty Dog. Super Mario 64 is property of Nintendo.
