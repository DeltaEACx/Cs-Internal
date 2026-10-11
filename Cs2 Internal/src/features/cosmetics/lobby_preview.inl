// Private game-thread helpers. Preview models derive from CBaseAnimGraph, not
// C_EconEntity: only model/mesh operations are valid on these entities.
struct LobbyModelState
{
    Address list = 0, entity = 0;
    int definition = 0;
    std::uint64_t revision = 0;
    ULONGLONG started = 0, lastRequest = 0;
    unsigned requests = 0;
    bool confirmed = false, failed = false;
};
std::unordered_map<std::uint32_t, LobbyModelState> lobbyModels;
ULONGLONG nextLobbyTick = 0;

bool IsLobbyPreview(Address entity)
{
    Address table = 0, locator = 0;
    std::uint32_t signature = 0, offset = 0, type = 0, self = 0;
    if (!clientBase || !Read(entity, table) || table < clientBase + 8 || table >= clientBase + 0x8000000 ||
        !Read(table - 8, locator) || locator < clientBase || locator >= clientBase + 0x8000000 ||
        !Read(locator, signature) || signature != 1 || !Read(locator + 4, offset) || offset != 0 ||
        !Read(locator + 12, type) || type >= 0x8000000 ||
        !Read(locator + 20, self) || locator - self != clientBase)
        return false;
    char name[128]{};
    for (unsigned i = 0; i < sizeof(name); ++i)
    {
        if (!Read(clientBase + type + 16 + i, name[i]))
            return false;
        if (!name[i])
        {
            const std::string_view className(name, i);
            return className == ".?AVC_CSGO_PreviewModel@@" ||
                   className == ".?AVC_CSGO_PreviewModelAlias_csgo_item_previewmodel@@" ||
                   className == ".?AVC_CSGO_TeamPreviewModel@@";
        }
    }
    return false;
}

int PreviewDefinition(const std::string &path)
{
    for (const auto &model : custom_models::kModels)
        if (game_access::SameModelPath(path, model.path) ||
            (custom_models::StockModel(model.definition) &&
             game_access::SameModelPath(path, custom_models::StockModel(model.definition))))
            return model.definition;
    return 0;
}

__declspec(noinline) bool ConfirmLobbyModel(Address entity, const char *path)
{
    __try
    {
        return ModelHasPath(ModelHandle(entity), path) && ApplyMesh(entity, false);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void TickLobbyModels(ULONGLONG now)
{
    if (faulted || now < nextLobbyTick)
        return;
    nextLobbyTick = now + 100;
    Address list = 0;
    if (!Read(clientBase + global::dwEntityList, list) || !list)
    {
        lobbyModels.clear();
        return;
    }
    std::unordered_set<std::uint32_t> seen;
    // Client-side preview entities can live above the networked player slots.
    for (unsigned index = 1; index < 0x7fff; ++index)
    {
        Address page = 0, entity = 0;
        std::uint32_t handle = 0xffffffffu;
        if (!Read(list + 0x10 + 8 * (index >> 9), page) || !page)
        {
            index |= 511;
            continue;
        }
        const Address entry = page + 0x70 * (index & 511);
        if (!Read(entry, entity) || !entity || !Read(entry + 0x10, handle) ||
            (handle & 0x7fff) != index || Resolve(list, handle) != entity || !IsLobbyPreview(entity))
            continue;
        const auto path = ModelPath(ModelHandle(entity));
        const int definition = PreviewDefinition(path);
        if (!definition)
            continue;
        seen.insert(handle);
        Saved saved;
        {
            std::lock_guard lock(settingsMutex);
            const auto found = selections.find(Key(definition, IsKnife(definition)));
            if (found == selections.end())
                continue;
            saved = found->second;
        }
        auto &state = lobbyModels[handle];
        if (state.list != list || state.entity != entity || state.revision != saved.revision ||
            state.definition != definition)
            state = {list, entity, definition, saved.revision, now};
        if (!saved.choice.customModel)
        {
            // A stock selection must undo a previous custom lobby assignment.
            if (custom_models::Find(path) && Resolve(list, handle) == entity)
                RestoreStockModel(entity, 0, saved.choice.definition);
            continue;
        }
        std::string message;
        if (Resolve(list, handle) == entity && ConfirmLobbyModel(entity, saved.choice.model.c_str()))
        {
            state.confirmed = true;
            state.failed = false;
            message = "Applied in lobby (local preview)";
        }
        else
        {
            if (state.confirmed)
                state = {list, entity, definition, saved.revision, now};
            if (state.failed)
                continue;
            if (now - state.started >= 5000)
            {
                state.failed = true;
                message = "Lobby model loading timed out; press Apply to retry";
            }
            else if (state.requests >= 4 || (state.requests && now - state.lastRequest < 1000))
                continue;
            else if ((!state.requests && !PrecacheCustomModel(saved.choice.model.c_str())) ||
                     Resolve(list, handle) != entity || !ApplyAgent(entity, saved.choice.model.c_str()))
            {
                state.failed = true;
                message = "Lobby model loader unavailable; check game build";
            }
            else
            {
                ++state.requests;
                state.lastRequest = now;
                message = "Loading selected lobby model...";
            }
        }
        std::lock_guard lock(settingsMutex);
        const auto found = selections.find(Key(definition, IsKnife(definition)));
        if (found != selections.end() && found->second.revision == saved.revision)
            found->second.status = message;
    }
    std::erase_if(lobbyModels, [&](const auto &entry) { return !seen.contains(entry.first); });
}
