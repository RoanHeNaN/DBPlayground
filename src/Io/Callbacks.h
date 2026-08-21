//
// Shared callback typedefs for the reactor / IO layer.
//

#pragma once

#include <functional>

namespace dbplay {

// A callback invoked from within an EventLoop thread when an event fires or a
// queued task runs. All reactor callbacks are void() closures; state is captured
// by the closure itself.
using EventCallback = std::function<void()>;

}  // namespace dbplay
