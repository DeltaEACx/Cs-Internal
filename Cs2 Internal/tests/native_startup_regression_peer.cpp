#include "core/module_digest.hpp"
bool PeerModuleMatch(std::uintptr_t module,std::string_view digest){return native_startup::MatchesModule(module,digest);}
