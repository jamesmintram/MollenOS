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
#pragma once

#include <cstdlib>
#include <queue>
#include <thread>
#include "engine/veightengine.hpp"
#include "utils/log_manager.hpp"
#include "events/event_window.hpp"

class VioarrCompositor {
public:
	static VioarrCompositor& GetInstance() {
		// Guaranteed to be destroyed.
		// Is instantiated on first use
		static VioarrCompositor _Instance;
		return _Instance;
	}
private:
	VioarrCompositor() {}                     // Constructor? (the {} brackets) are needed here.

public:
	VioarrCompositor(VioarrCompositor const&) = delete;
	void operator=(VioarrCompositor const&) = delete;

    // The main program loop
    int                         Run();

    // Queues a new event up
    void                        QueueEvent(CVioarrEvent *Event);

private:
    // Functions
    void                        SpawnMessageHandler();
    CEntity*                    CreateStandardScene();

    // Resources
    CDisplay*                   _Display;
    std::thread*                _MessageThread;
    std::condition_variable     _EventSignal;
    std::queue<CVioarrEvent*>   _EventQueue;
    std::mutex                  _EventMutex;

    // State tracking
    bool                        _IsRunning;
};

// Shorthand for the vioarr
#define sVioarr VioarrCompositor::GetInstance()
