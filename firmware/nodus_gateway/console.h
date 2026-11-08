// Serial console (115200 baud, line ending CR or LF) for setup and diagnostics.
// Type "help".
#pragma once

namespace console {
void poll();  // call from loop()
}
