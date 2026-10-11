#pragma once

namespace gui_embedded {
// Call from an ordinary control thread, never DllMain or a render callback.
bool Start();
bool Stop(unsigned long timeoutMs=5000);
void Toggle();
bool Running();
bool Stopped();
}
