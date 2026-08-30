#pragma once

#include <cstdint>

// Windows clipboard -> Scaleform clipboard, so the game's own Ctrl+V pastes text
// copied outside the game. Off until the meter turns it on.
void ClipSyncSetEnabled(bool enabled);

// Game message-pump thread only: pushes the cached text into the GFx clipboard
// if the Windows clipboard has moved since the last push. A no-op otherwise.
void ClipSyncApplyOnGameThread();

// WM_ACTIVATEAPP / WM_SETFOCUS on the subclassed window. Already on the pump
// thread, so it applies directly.
void ClipSyncOnForeground();

void ClipSyncShutdown();
