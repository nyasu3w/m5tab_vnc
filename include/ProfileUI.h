#pragma once

#include "ConnectionProfile.h"
#include "ProfileStore.h"
#include <M5Unified.h>

// Call synchronously after M5.begin()/display rotation and CardKB setup, before
// starting VNC/input tasks. The caller loads the store first. No networking is
// performed here. Each call requires an explicit touch on Connect, even when
// there is only one profile. CardKB input is used only by the field keyboard.
// On false (Cancel), out and slot are unchanged. On true they contain a copy
// of the validated saved profile and its slot; no store pointer escapes.
bool selectConnectionProfile(ProfileStore& store, ConnectionProfile& out,
                             uint8_t& slot,
                             uint8_t (*readKey)() = nullptr);
