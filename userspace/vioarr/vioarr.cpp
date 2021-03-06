/* MollenOS
 *
 * Copyright 2018, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * MollenOS - Vioarr Window Compositor System
 *  - The window compositor system and general window manager for
 *    MollenOS.
 */

#include <os/mollenos.h>
#include <os/process.h>
#include "vioarr.hpp"

#if defined(_VIOARR_OSMESA)
#include "graphics/opengl/osmesa/display_osmesa.hpp"
#define DISPLAY_TYPE() CDisplayOsMesa()
#else
#include "graphics/soft/display_framebuffer.hpp"
#define DISPLAY_TYPE() CDisplayFramebuffer()
#endif

// Run
// The main program loop
int VioarrCompositor::Run()
{
    //std::chrono::time_point<std::chrono::system_clock> LastUpdate;
    _IsRunning = true;

    // Create the display
    sLog.Info("Creating display");
    _Display = new DISPLAY_TYPE();
    if (!_Display->Initialize()) {
        delete _Display;
        return -2;
    }

    // Spawn message handler
    sLog.Info("Spawning message handler");
    SpawnMessageHandler();

    // Initialize V8 Engine
    sLog.Info("Initializing V8");
    sEngine.Initialize(_Display);
    sEngine.SetRootEntity(CreateStandardScene());
    MollenOSEndBoot();

    // Initial render
    sEngine.Update(0);
    sEngine.Render();

    // Spawn the test application
    ProcessSpawn("$bin/wintest.app", NULL, 1);

    // Enter event loop
    //LastUpdate = std::chrono::system_clock::now();
    while (_IsRunning) {
        CVioarrEvent *Event = nullptr;
        {
            std::unique_lock<std::mutex> _eventlock(_EventMutex);
            while (_EventQueue.empty()) _EventSignal.wait(_eventlock);
            Event = _EventQueue.front();
            _EventQueue.pop();
        }
        sLog.Info("processing event");

        // Handle event
        switch (Event->GetType()) {
            case CVioarrEvent::EventWindowCreated: {
                sEngine.GetRootEntity()->AddEntity(((CWindowCreatedEvent*)Event)->GetWindow());
            } break;
            case CVioarrEvent::EventWindowDestroy: {
                sEngine.GetRootEntity()->RemoveEntity(((CWindowDestroyEvent*)Event)->GetWindow());
                delete ((CWindowDestroyEvent*)Event)->GetWindow();
            } break;
            case CVioarrEvent::EventWindowUpdate: {
                ((CWindowUpdateEvent*)Event)->GetWindow()->SwapOnNextUpdate(true);
            } break;
        }
        delete Event;

        // Run updates
        // auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - LastUpdate);
        sEngine.Update(0 /*  milliseconds.count() */);

        // Update screen if there are no more events
        if (_EventQueue.empty()) {
            sEngine.Render();
        }
        // LastUpdate = std::chrono::system_clock::now();
    }
    return 0;
}

// Queues a new event up
void VioarrCompositor::QueueEvent(CVioarrEvent *Event)
{
    std::unique_lock<std::mutex> _eventlock(_EventMutex);
    _EventQueue.push(Event);
    _EventSignal.notify_one();
}
