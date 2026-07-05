#pragma once

#include <memory>

namespace ashiato::debugger {

struct DebuggerAppState;

class DebuggerApp {
public:
    DebuggerApp();
    ~DebuggerApp();

    DebuggerApp(const DebuggerApp&) = delete;
    DebuggerApp& operator=(const DebuggerApp&) = delete;
    DebuggerApp(DebuggerApp&&) noexcept;
    DebuggerApp& operator=(DebuggerApp&&) noexcept;

    void draw();

private:
    std::unique_ptr<DebuggerAppState> state_;
};

void apply_debugger_style();

}  // namespace ashiato::debugger
