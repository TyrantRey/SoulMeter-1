#pragma once

#include <cstdint>

// Wire ops, shared with the meter (Soulworker Packet/HookCommand.h).
#define SMH_CMD_RESTART_MAZE 1
#define SMH_CMD_EXIT_MAZE 2
#define SMH_CMD_CLIPBOARD_PASTE 3

// Internal op, posted by the clipboard watcher. Never accepted off the pipe.
#define SMH_CMD_CLIP_APPLY 0x80

// False until SoulWorker64.dll is loaded and the game window is subclassed.
// Sender resolution is attempted here but is not required: a build where the
// maze senders are gone still gets a window, which is all the other commands
// need.
bool GameCmdInit();
void GameCmdShutdown();

// Re-picks the game window if it was never found or has been recreated. Safe
// from any thread; cheap once armed.
bool GameCmdEnsureArmed();

// netMgr `this`, captured from the send hook rather than a pinned global.
void GameCmdSetNetMgr(void* netMgr);

// Queues `op` onto the game's message-pump thread. Safe from any thread.
void GameCmdPost(uint8_t op, uint32_t arg);

// Same, for internal ops: no wire filter and no debounce. False when there is
// no window yet.
bool GameCmdPostLocal(uint8_t op, uint32_t arg);
