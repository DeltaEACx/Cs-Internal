#include "features/visuals/engine.hpp"
#include "../../third_party/spectral/bridge/host.hpp"
extern "C" std::uintptr_t SpectralHostResolve(const char* expression, std::size_t length)
{
    return memory::resolve_cached(std::string_view(expression, length));
}
