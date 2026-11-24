#pragma once

#include <windows.h>

#include "common/types.h"

// Console frame pacing (ID3D12Device::SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX /
// GetFrameStatisticsX). The console runtime owns the frame cadence: the title sets an interval
// (time between flips) and a period (how many intervals a frame may take from start to display),
// then waits for "origin" events that tell it when to start each frame. An origin fires at a
// fixed offset into each interval, but only while fewer than `period` frames are in flight, which
// throttles the title to the display.
//
// We keep a virtual display timeline at the title's interval on a scheduler thread.

enum FrameEventType : u32 {
    FRAME_EVENT_ORIGIN = 0,
    FRAME_EVENT_VBLANK = 1,
    FRAME_EVENT_FLIP = 2,
    FRAME_EVENT_COUNT = 3,
};

typedef u64 FrameToken; // 0 = none

struct FrameSignalList { // title events to signal along with a frame event
    u32 count;
    HANDLE* objects;
};

struct FrameWaitList { // extra handles a frame-event wait also returns for
    u32 count;
    HANDLE* objects;
    u32* signaled_index; // optional out
};

#define FRAME_SIGNALED_NONE    0xFFFFFFFFu
#define FRAME_SIGNALED_PRIMARY 0xFFFFFFFEu

HRESULT frame_set_interval(u32 length_us, u32 period, u32 flags);
HRESULT frame_schedule_event(FrameEventType type, u32 offset_us, const FrameSignalList* signals, u32 flags);
HRESULT frame_wait_event(FrameEventType type, u32 timeout_ms, const FrameWaitList* extra, u32 flags,
                         FrameToken* token);

// Called once a presented frame is on screen.
void frame_on_flip(FrameToken token);
