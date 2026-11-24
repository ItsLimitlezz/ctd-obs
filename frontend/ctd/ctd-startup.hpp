/******************************************************************************
    CTD OBS: what happens before the main window (frontend/obs-main.cpp
    run_program, frontend/widgets/OBSBasic.cpp OBSInit).

      splash screen     frontend/ctd/ctd-splash.cpp
      update check      frontend/ctd/ctd-update.cpp (docs/ctd/updates.md)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

enum class CtdSplashStage { Updates, Plugins, Scenes, Starting };

/* Shows the splash screen (UI thread; once). Not shown for automated or tray launches: the caller decides. */
void CtdSplashShow();
/* Moves the splash to a startup stage and repaints it; does nothing when it isn't shown */
void CtdSplashSetStage(CtdSplashStage stage);
/* Hides the splash while a dialog takes over (the update dialog), and brings it back */
void CtdSplashSuspend();
void CtdSplashResume();
/* Closes it for good (the main window is up, or startup stopped). Safe to call more than once. */
void CtdSplashFinish();

/* The launch update check, before the main window loads. Returns false when CTD OBS should quit now because an update
 * is being installed (the installer or the macOS update helper has been started); true to continue starting up. Never
 * blocks longer than a few seconds when GitHub is slow or unreachable. */
bool CtdStartupUpdateCheck();
