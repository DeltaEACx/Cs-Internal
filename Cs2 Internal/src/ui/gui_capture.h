#pragma once
#include <cstddef>

// Optional render-thread semantic capture. Normal ImGui remains unchanged when
// no sink is installed. Values are read/written only during the widget call.
namespace gui_capture {
struct Sink {
    virtual ~Sink() = default;
    virtual bool Boolean(const char*, bool*) = 0;
    virtual bool Number(const char*, int, void*, int, const void*, const void*, const char*, double) = 0;
    virtual bool Color(const char*, float*, int) = 0;
    virtual bool Text(const char*, char*, std::size_t, bool) = 0;
    virtual bool Action(const char*, bool = false) = 0;
    virtual bool Radio(const char*, bool) = 0;
    virtual void BeginSelect(const char*, const char*) = 0;
    virtual bool Option(const char*, bool, bool) = 0;
    virtual void EndSelect() = 0;
    virtual void Section(const char*) = 0;
    virtual void BeginGroup(const char*, const char*, unsigned = 0) = 0;
    virtual void EndGroup() = 0;
    virtual bool Keybind(const char*, int*) = 0;
    virtual bool Card(const char*, const char*, bool, bool) = 0;
    virtual void ReportError(const char*) = 0;
};
inline thread_local Sink* active = nullptr;
}
