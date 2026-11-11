// The runtime's life cycle and the guarded entry points for the host's threads. The game DLL (Device/UnityBridge.cpp)
// and the test host (tests/host) both drive Aether through these.
#pragma once
#include "Core/Common.h"
#include "Device/Host.h"

namespace aether::session {

// Main thread. Starts the log, settings, worker and IPC server. False if it was already started.
bool Start(const std::wstring& dataDir, const std::wstring& configDir);
// Main thread. Stops IPC and the worker. Device objects must have been released on the render thread before
// (ReleaseOnRenderThread); what is still there is abandoned unreleased.
void Stop();
bool Started();
// True after an unexpected fault inside Aether's frame code: nothing is rendered any more this session.
bool Dead();

// Host render thread. All guarded: a C++ exception or a structured exception inside stops Aether, not the game.
void FrameBegin(const HostGfx& host);
void FrameEnd(const HostGfx& host);
// Host render thread, with the host's pending command lists flushed: frees Aether's device objects once the GPU is idle.
void ReleaseOnRenderThread(const HostGfx& host, uint32_t waitMs);
// Any thread: waits until ReleaseOnRenderThread has run since Start (false on timeout).
bool WaitReleased(uint32_t timeoutMs);

// Main thread, every frame.
void MainThreadUpdate();

} // namespace aether::session
