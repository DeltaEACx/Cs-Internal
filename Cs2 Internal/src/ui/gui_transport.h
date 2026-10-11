#pragma once

#include <functional>
#include <string>

namespace gui_transport {
using Handler = std::function<std::string(const std::string&)>;

// The handler runs on the pipe worker. It must return within two seconds and
// must not call Start/Stop; game-owned state belongs on the render thread.
bool Start(Handler handler);
void Stop();
}
