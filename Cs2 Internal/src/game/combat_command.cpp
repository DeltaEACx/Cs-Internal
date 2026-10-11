#include "game/combat_command.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <limits>
#include <vector>

namespace combat_command
{
    namespace
    {
        using namespace combat_bindings;
        struct Write { Address address; std::vector<std::byte> before, after; };
        struct CrcBytes { std::array<std::uint8_t, 64> bytes{}; int size{}; };
        CrcBytes Serialize(const ButtonPayload &buttons, const AnglePayload &angles)
        {
            CrcBytes result;
            auto *cursor = result.bytes.data();
            const auto put = [&](std::uint8_t tag, const auto &value)
            {
                *cursor++ = tag;
                std::memcpy(cursor, &value, sizeof(value));
                cursor += sizeof(value);
            };
            const auto buttonSize = static_cast<std::uint8_t>((buttons.value ? 9 : 0) +
                (buttons.changed ? 9 : 0) + (buttons.scroll ? 9 : 0));
            if (buttonSize)
            {
                *cursor++ = 0x1a; *cursor++ = buttonSize;
                if (buttons.value) put(0x09, buttons.value);
                if (buttons.changed) put(0x11, buttons.changed);
                if (buttons.scroll) put(0x19, buttons.scroll);
            }
            const auto angleSize = static_cast<std::uint8_t>((angles.pitch != 0 ? 5 : 0) +
                (angles.yaw != 0 ? 5 : 0) + (angles.roll != 0 ? 5 : 0));
            if (angleSize)
            {
                *cursor++ = 0x22; *cursor++ = angleSize;
                if (angles.pitch != 0) put(0x0d, angles.pitch);
                if (angles.yaw != 0) put(0x15, angles.yaw);
                if (angles.roll != 0) put(0x1d, angles.roll);
            }
            result.size = static_cast<int>(cursor - result.bytes.data());
            return result;
        }
    }
    bool Edit(Address command, Address baseAddress, int commandNumber,
              const std::optional<std::array<float, 3>> &aim, bool pressAttack, const IO &io,
              const std::optional<Shot> &shot, AngleMode mode, FireButton fire)
    {
        using namespace combat_bindings;
        if (!command || !baseAddress || commandNumber <= 0 || (!aim && !pressAttack) ||
            !io.read || !io.write || !io.allocateButtons || !io.crc) return false;
        if (mode == AngleMode::BaseOnly && (!aim || pressAttack || shot)) return false;
        if (fire != FireButton::Primary && fire != FireButton::Secondary) return false;
        // Scope owns only the secondary action. It must never rewrite aiming,
        // firing time, target history or an existing primary shot.
        if (fire == FireButton::Secondary && (!pressAttack || aim || shot || mode != AngleMode::ShotHistory)) return false;
        if (aim && !std::ranges::all_of(*aim, [](float value) { return std::isfinite(value); })) return false;
        if (shot && (!aim || !pressAttack || shot->tick <= 0 || !std::isfinite(shot->fraction) ||
            shot->fraction < 0 || shot->fraction >= 1 ||
            !std::ranges::all_of(shot->angles, [](float value) { return std::isfinite(value); }) ||
            std::abs(shot->angles[0]) > 89.f || std::abs((*aim)[0]) > 89.f)) return false;
        if (shot && shot->backtrack) {
            const auto& target = *shot->backtrack;
            if (target.render.tick <= 0 || target.render.tick > shot->tick + 1 ||
                shot->tick - target.render.tick > 64 || !std::isfinite(target.render.fraction) ||
                target.render.fraction < 0 || target.render.fraction >= 1 ||
                target.targetIndex <= 0 || target.targetIndex >= 0x7fff) return false;
        }
        BaseCommand base{};
        CommandProto proto{};
        ButtonState buttons{};
        int sequence{};
        if (!io.read(&sequence, command + kCommandSequence, sizeof(sequence)) || sequence != commandNumber ||
            !io.read(&base, baseAddress, sizeof(base)) ||
            !io.read(&proto, command + kCommandProto, sizeof(proto)) ||
            !(proto.bits & 1) || proto.base + kMessagePayload != baseAddress ||
            !io.read(&buttons, command + kCommandButtons, sizeof(buttons))) return false;
        const auto originalBase = base;
        const auto originalProto = proto;
        const auto originalButtons = buttons;
        ButtonPayload buttonPayload{};
        if (base.buttons && !io.read(&buttonPayload, base.buttons + kMessagePayload, sizeof(buttonPayload))) return false;
        const auto originalButtonPayload = buttonPayload;
        AnglePayload angles{};
        if (base.angles && !io.read(&angles, base.angles + kMessagePayload, sizeof(angles))) return false;
        if (!std::isfinite(angles.pitch) || !std::isfinite(angles.yaw) || !std::isfinite(angles.roll)) return false;
        const auto originalAngles = angles;
        if (aim && (!base.angles || !std::isfinite(angles.pitch) || !std::isfinite(angles.yaw))) return false;
        std::vector<Write> writes;
        bool validPlan = true;
        const auto plan = [&](Address address, const auto &before, const auto &after)
        {
            Write write{address, std::vector<std::byte>(sizeof(before)), std::vector<std::byte>(sizeof(after))};
            std::memcpy(write.before.data(), &before, sizeof(before));
            std::memcpy(write.after.data(), &after, sizeof(after));
            if (!address || address > std::numeric_limits<Address>::max() - sizeof(after)) { validPlan = false; return; }
            for (const auto &previous : writes)
            {
                if (address >= previous.address + previous.after.size() ||
                    previous.address >= address + sizeof(after)) continue;
                if (address == previous.address && write.before == previous.before && write.after == previous.after) return;
                validPlan = false;
                return;
            }
            writes.push_back(std::move(write));
        };
        const auto entries = [&](const RepeatedField &field, int maximum, std::vector<Address> &out)
        {
            if (field.size < 0 || field.size > maximum || field.capacity < field.size) return false;
            if (!field.size) return true;
            int allocated{};
            if (!field.entries || !io.read(&allocated, field.entries, sizeof(allocated)) || allocated < field.size) return false;
            out.resize(field.size);
            return io.read(out.data(), field.entries + 8, out.size() * sizeof(Address)) &&
                std::ranges::all_of(out, [](Address value) { return value != 0; });
        };
        std::vector<Address> history;
        if (!entries(proto.history, 64, history)) return false;
        // Roll compensation belongs to a real shot history entry. A command
        // without history must not be fired using only the unrolled base angle.
        if (shot && history.empty()) return false;
        if (aim)
        {
            angles.bits |= 7;
            angles.pitch = std::clamp((*aim)[0], -89.f, 89.f);
            angles.yaw = std::remainder((*aim)[1], 360.f);
            angles.roll = 0;
            base.bits |= 4;
            plan(base.angles + kMessagePayload, originalAngles, angles);
            if (mode == AngleMode::ShotHistory)
            for (const auto raw : history)
            {
                HistoryHeader header{};
                AnglePayload previous{};
                const auto payload = raw + kMessagePayload;
                if (!io.read(&header, payload, sizeof(header)) || !header.angles ||
                    !io.read(&previous, header.angles + kMessagePayload, sizeof(previous))) return false;
                auto next = previous;
                next.bits |= 7;
                next.pitch = shot ? shot->angles[0] : angles.pitch;
                next.yaw = shot ? std::remainder(shot->angles[1], 360.f) : angles.yaw;
                next.roll = shot ? std::remainder(shot->angles[2], 360.f) : 0.f;
                auto nextHeader = header;
                nextHeader.bits |= 1;
                if (shot)
                {
                    HistoryShotTiming previousTime{};
                    if (!io.read(&previousTime, payload + kHistoryPlayerTick, sizeof(previousTime))) return false;
                    const HistoryShotTiming nextTime{shot->tick, shot->fraction};
                    nextHeader.bits |= kHistoryPlayerTickBit | kHistoryPlayerFractionBit;
                    plan(payload + kHistoryPlayerTick, previousTime, nextTime);
                    if (shot->backtrack) {
                        HistoryShotTiming beforeRender{};
                        int beforeTarget{};
                        if (!io.read(&beforeRender, payload + kHistoryRenderTick, sizeof(beforeRender)) ||
                            !io.read(&beforeTarget, payload + kHistoryTargetIndex, sizeof(beforeTarget))) return false;
                        nextHeader.bits |= kHistoryRenderBits | 0x4000;
                        plan(payload + kHistoryRenderTick, beforeRender, shot->backtrack->render);
                        plan(payload + kHistoryTargetIndex, beforeTarget, shot->backtrack->targetIndex);
                        // Existing interpolation messages describe the old render
                        // timestamp. Reset only target interpolation, retaining
                        // player interpolation and the shoot-position message.
                        for (unsigned index = 0; index < 3; ++index) {
                            const auto bit = 2u << index;
                            if (!(header.bits & bit)) continue;
                            Address rawInterpolation{};
                            if (!io.read(&rawInterpolation, payload + 0x10 + index * sizeof(Address), sizeof(Address)) ||
                                !rawInterpolation) return false;
                            if (!index) {
                                struct ClientInterpolation { std::uint32_t bits, cachedSize; float fraction; };
                                ClientInterpolation before{};
                                if (!io.read(&before, rawInterpolation + kMessagePayload, sizeof(before))) return false;
                                auto after = before; after.bits |= 1; after.fraction = 0;
                                plan(rawInterpolation + kMessagePayload, before, after);
                            } else {
                                Interpolation before{};
                                if (!io.read(&before, rawInterpolation + kMessagePayload, sizeof(before))) return false;
                                auto after = before; after.bits |= 7;
                                after.fraction = 0; after.source = after.destination = -1;
                                plan(rawInterpolation + kMessagePayload, before, after);
                            }
                        }
                    }
                }
                plan(header.angles + kMessagePayload, previous, next);
                plan(payload, header, nextHeader);
            }
            // Preserve the world-space direction of WASD and native analog subticks.
            const auto radians = std::remainder(angles.yaw - originalAngles.yaw, 360.f) * std::numbers::pi_v<float> / 180.f;
            const auto cosine = std::cos(radians), sine = std::sin(radians);
            const auto rotate = [&](float &forward, float &left)
            {
                if (!std::isfinite(forward) || !std::isfinite(left)) return false;
                const auto oldForward = forward;
                forward = oldForward * cosine + left * sine;
                left = left * cosine - oldForward * sine;
                return std::isfinite(forward) && std::isfinite(left);
            };
            base.forward = base.bits & 0x40 ? base.forward : 0.f;
            base.left = base.bits & 0x80 ? base.left : 0.f;
            if (!rotate(base.forward, base.left)) return false;
            const auto movementScale = std::max({1.f, std::abs(base.forward), std::abs(base.left)});
            base.forward /= movementScale;
            base.left /= movementScale;
            base.bits |= 0xc0;
            std::vector<Address> subticks;
            if (!entries(base.subticks, 256, subticks)) return false;
            for (const auto raw : subticks)
            {
                SubtickPayload previous{};
                if (!io.read(&previous, raw + kMessagePayload, sizeof(previous))) return false;
                if (!(previous.bits & 0x18)) continue;
                auto next = previous;
                next.forward = previous.bits & 8 ? previous.forward : 0.f;
                next.left = previous.bits & 16 ? previous.left : 0.f;
                if (!rotate(next.forward, next.left)) return false;
                next.bits |= 0x18;
                plan(raw + kMessagePayload, previous, next);
            }
            constexpr std::uint64_t forward = 8, back = 16, left = 512, right = 1024;
            buttons.value &= ~(forward | back | left | right);
            if (base.forward > .0001f) buttons.value |= forward;
            if (base.forward < -.0001f) buttons.value |= back;
            if (base.left > .0001f) buttons.value |= left;
            if (base.left < -.0001f) buttons.value |= right;
            buttons.changed |= originalButtons.value ^ buttons.value;
        }
        if (pressAttack)
        {
            const auto mask = fire == FireButton::Secondary ? 1ull << 11 : kAttack;
            buttons.value |= mask;
            buttons.changed |= mask;
            buttons.scroll |= mask;
        }
        if (fire == FireButton::Secondary)
        {
            proto.bits |= 0x40;
            proto.attack2History = static_cast<int>(history.size()) - 1;
        }
        else if (mode == AngleMode::ShotHistory && (buttons.value & kAttack))
        {
            proto.bits |= 0x20;
            proto.attack1History = static_cast<int>(history.size()) - 1;
        }
        if (!base.buttons)
        {
            base.buttons = io.allocateButtons(baseAddress - kMessagePayload);
            if (!base.buttons || !io.read(&buttonPayload, base.buttons + kMessagePayload, sizeof(buttonPayload))) return false;
        }
        const auto previousAllocatedButtons = buttonPayload;
        buttonPayload.bits |= 7;
        if(mode==AngleMode::BaseOnly || fire==FireButton::Secondary)
        {
            // Raw/PB buttons can differ during a pending manual action. Each
            // editor preserves every protobuf action outside the mask it owns.
            const std::uint64_t ownedMask=fire==FireButton::Secondary?1ull<<11:8|16|512|1024;
            const auto previousValue=previousAllocatedButtons.bits&1?previousAllocatedButtons.value:0;
            const auto previousChanged=previousAllocatedButtons.bits&2?previousAllocatedButtons.changed:0;
            const auto previousScroll=previousAllocatedButtons.bits&4?previousAllocatedButtons.scroll:0;
            buttonPayload.value=(previousValue&~ownedMask)|(buttons.value&ownedMask);
            buttonPayload.changed=(previousChanged&~ownedMask)|(buttons.changed&ownedMask)|
                ((previousValue^buttonPayload.value)&ownedMask);
            buttonPayload.scroll=(previousScroll&~ownedMask)|(buttons.scroll&ownedMask);
        }
        else
        {
            buttonPayload.value = buttons.value;
            buttonPayload.changed = buttons.changed;
            buttonPayload.scroll = buttons.scroll;
        }
        base.bits |= 3;
        plan(base.buttons + kMessagePayload, previousAllocatedButtons, buttonPayload);
        plan(command + kCommandButtons, originalButtons, buttons);
        plan(command + kCommandProto, originalProto, proto);
        plan(baseAddress, originalBase, base);
        if (!validPlan) return false;
        if (mode == AngleMode::BaseOnly)
        {
            // Separate native angle objects are expected. Reject an alias that
            // would make a base-only write alter a shot history indirectly.
            constexpr std::size_t historyPayloadSize = 0x68;
            for (const auto raw : history)
            {
                HistoryHeader header{};
                if (raw > std::numeric_limits<Address>::max() - kMessagePayload - historyPayloadSize ||
                    !io.read(&header, raw + kMessagePayload, sizeof(header))) return false;
                const auto overlaps = [](Address first, std::size_t firstSize, Address second, std::size_t secondSize)
                {
                    return second && second <= std::numeric_limits<Address>::max() - secondSize &&
                        first < second + secondSize && second < first + firstSize;
                };
                for (const auto &write : writes)
                    if (overlaps(write.address, write.after.size(), raw + kMessagePayload, historyPayloadSize) ||
                        (header.angles && (header.angles > std::numeric_limits<Address>::max() - kMessagePayload ||
                         overlaps(write.address, write.after.size(), header.angles + kMessagePayload, sizeof(AnglePayload)))))
                        return false;
            }
        }
        std::size_t applied{};
        const auto rollback = [&]
        {
            while (applied)
            {
                const auto &write = writes[--applied];
                io.write(write.address, write.before.data(), write.before.size());
            }
        };
        for (const auto &write : writes)
        {
            ++applied; // Include a failed write in case the provider wrote a partial payload.
            if (!io.write(write.address, write.after.data(), write.after.size())) { rollback(); return false; }
        }
        const auto crc = Serialize(buttonPayload, angles);
        if (!io.crc(baseAddress - kMessagePayload, baseAddress + offsetof(BaseCommand, moveCrc), crc.bytes.data(), crc.size))
        {
            rollback();
            const auto previousCrc = Serialize(originalButtonPayload, originalAngles);
            io.crc(baseAddress - kMessagePayload, baseAddress + offsetof(BaseCommand, moveCrc), previousCrc.bytes.data(), previousCrc.size);
            return false;
        }
        return true;
    }
    bool EditMovement(Address command, Address baseAddress, int commandNumber,
                      const Movement &movement, const MovementIO &provider)
    {
        using namespace combat_bindings;
        const auto &io = provider.command;
        constexpr std::uint64_t ownedButtons = (1ull<<1)|(1ull<<2)|(1ull<<3)|(1ull<<4)|(1ull<<9)|(1ull<<10);
        if (!command || !baseAddress || commandNumber <= 0 || !io.read || !io.write || !io.allocateButtons || !io.crc ||
            movement.count > movement.steps.size() || movement.replaceCount>movement.replaceSteps.size() || (movement.count && !provider.acquireSubtick) ||
            (movement.zeroBaseAnalog && (!movement.analog || !movement.replaceAnalog || !movement.count)) ||
            ((movement.clearButtons | movement.setButtons) & ~ownedButtons) ||
            (!movement.analog && !movement.angles && !movement.count && !movement.clearButtons && !movement.setButtons)) return false;
        if (movement.analog && !std::ranges::all_of(*movement.analog, [](float v) { return std::isfinite(v) && std::abs(v)<=1; })) return false;
        if (movement.angles && (!std::ranges::all_of(*movement.angles, [](float v) { return std::isfinite(v); }) ||
            std::abs((*movement.angles)[0])>89)) return false;
        for (std::size_t index=0;index<movement.count;++index)
        {
            const auto &step=movement.steps[index];
            if ((step.bits & ~0x7fu) || !(step.bits & 4) || !std::isfinite(step.when) || step.when<0 || step.when>=1 ||
                (step.button & ~ownedButtons) || !std::isfinite(step.forward) || !std::isfinite(step.left) ||
                !std::isfinite(step.pitch) || !std::isfinite(step.yaw) || std::abs(step.forward)>2.002f || std::abs(step.left)>2.002f ||
                std::abs(step.pitch)>360.f || std::abs(step.yaw)>360.f) return false;
        }
        BaseCommand base{}; CommandProto proto{}; ButtonState buttons{}; int sequence{};
        if (!io.read(&sequence,command+kCommandSequence,sizeof(sequence)) || sequence!=commandNumber ||
            !io.read(&base,baseAddress,sizeof(base)) || !io.read(&proto,command+kCommandProto,sizeof(proto)) ||
            !(proto.bits&1) || proto.base+kMessagePayload!=baseAddress ||
            !io.read(&buttons,command+kCommandButtons,sizeof(buttons))) return false;
        const auto initialBase=base;
        const auto originalButtons=buttons;
        if (base.subticks.size<0 || base.subticks.size>256 || base.subticks.capacity<base.subticks.size ||
            base.subticks.capacity>65536 || movement.replaceCount>static_cast<std::size_t>(base.subticks.size) ||
            base.subticks.size-movement.replaceCount+movement.count>256) return false;
        std::vector<Address> previousSteps,originalAllocation;
        if (base.subticks.entries)
        {
            int allocated{};
            if (!io.read(&allocated,base.subticks.entries,sizeof(allocated)) || allocated<base.subticks.size || allocated>base.subticks.capacity) return false;
            previousSteps.resize(base.subticks.size);
            if ((!previousSteps.empty()&&!io.read(previousSteps.data(),base.subticks.entries+8,previousSteps.size()*sizeof(Address))) ||
                !std::ranges::all_of(previousSteps,[](Address raw){return raw && raw<=std::numeric_limits<Address>::max()-kMessagePayload-sizeof(SubtickPayload);})) return false;
            auto sorted=previousSteps;std::ranges::sort(sorted);
            if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())return false;
            originalAllocation.resize(allocated);
            if(!originalAllocation.empty()&&!io.read(originalAllocation.data(),base.subticks.entries+8,originalAllocation.size()*sizeof(Address)))return false;
        }
        else if(base.subticks.size || base.subticks.capacity) return false;
        std::vector<Address> history;
        if (proto.history.size<0 || proto.history.size>64 || proto.history.capacity<proto.history.size) return false;
        if (proto.history.size)
        {
            int allocated{};
            if (!proto.history.entries || !io.read(&allocated,proto.history.entries,sizeof(allocated)) || allocated<proto.history.size) return false;
            history.resize(proto.history.size);
            if (!io.read(history.data(),proto.history.entries+8,history.size()*sizeof(Address))) return false;
        }
        ButtonPayload buttonPayload{}; AnglePayload angles{};
        if ((base.buttons && !io.read(&buttonPayload,base.buttons+kMessagePayload,sizeof(buttonPayload))) ||
            (base.angles && !io.read(&angles,base.angles+kMessagePayload,sizeof(angles))) ||
            (movement.angles && !base.angles) || !std::isfinite(angles.pitch) || !std::isfinite(angles.yaw) || !std::isfinite(angles.roll)) return false;
        const auto originalButtonPayload=buttonPayload;
        const auto originalAngles=angles;
        const auto intact=[&]{
            int currentSequence{};CommandProto currentProto{};ButtonState currentButtons{};BaseCommand currentBase{};
            ButtonPayload currentPayload{};AnglePayload currentAngles{};
            if(!io.read(&currentSequence,command+kCommandSequence,sizeof(currentSequence))||currentSequence!=commandNumber||
               !io.read(&currentProto,command+kCommandProto,sizeof(currentProto))||std::memcmp(&currentProto,&proto,sizeof(proto))||
               !io.read(&currentButtons,command+kCommandButtons,sizeof(currentButtons))||std::memcmp(&currentButtons,&originalButtons,sizeof(originalButtons))||
               !io.read(&currentBase,baseAddress,sizeof(currentBase))||
               (initialBase.buttons&&(!io.read(&currentPayload,initialBase.buttons+kMessagePayload,sizeof(currentPayload))||std::memcmp(&currentPayload,&originalButtonPayload,sizeof(currentPayload))))||
               (initialBase.angles&&(!io.read(&currentAngles,initialBase.angles+kMessagePayload,sizeof(currentAngles))||std::memcmp(&currentAngles,&originalAngles,sizeof(currentAngles)))))return false;
            // Native append may legitimately grow this field. Every other
            // command/base field must still belong to the captured command.
            currentBase.subticks=initialBase.subticks;
            return !std::memcmp(&currentBase,&initialBase,sizeof(initialBase));
        };
        const auto overlaps=[](Address first,std::size_t firstSize,Address second,std::size_t secondSize){
            return first&&second&&first<=std::numeric_limits<Address>::max()-firstSize&&second<=std::numeric_limits<Address>::max()-secondSize&&
                first<second+secondSize&&second<first+firstSize;};
        const auto arraySize=8+base.subticks.capacity*sizeof(Address);
        if(overlaps(base.subticks.entries,arraySize,command,0x98)||
            overlaps(base.subticks.entries,arraySize,baseAddress-kMessagePayload,sizeof(BaseCommand)+kMessagePayload)||
            overlaps(base.subticks.entries,arraySize,base.buttons,sizeof(ButtonPayload)+kMessagePayload)||
            overlaps(base.subticks.entries,arraySize,base.angles,sizeof(AnglePayload)+kMessagePayload))return false;
        for(const auto raw:history){HistoryHeader header{};
            if(!raw||raw>std::numeric_limits<Address>::max()-kMessagePayload-0x68||!io.read(&header,raw+kMessagePayload,sizeof(header))||
                overlaps(base.subticks.entries,arraySize,raw,kMessagePayload+0x68)||
                overlaps(base.subticks.entries,arraySize,header.angles,kMessagePayload+sizeof(AnglePayload)))return false;
        }
        for(const auto raw:originalAllocation)if(!raw||raw>std::numeric_limits<Address>::max()-kMessagePayload-sizeof(SubtickPayload)||
            overlaps(base.subticks.entries,arraySize,raw,kMessagePayload+sizeof(SubtickPayload)))return false;
        std::vector<Address> replaced(movement.replaceSteps.begin(),movement.replaceSteps.begin()+movement.replaceCount),retained;
        for(const auto raw:replaced)if(!raw||std::ranges::count(replaced,raw)!=1||std::ranges::count(previousSteps,raw)!=1)return false;
        for(const auto raw:previousSteps)if(std::ranges::find(replaced,raw)==replaced.end())retained.push_back(raw);
        std::vector<Address> acquired;
        const auto field=baseAddress+offsetof(BaseCommand,subticks);
        const auto restoreCount=[&]
        {
            RepeatedField current{};
            if (!io.read(&current,field,sizeof(current)) || current.size<0 || current.size>256 ||
                current.capacity<std::max(current.size,initialBase.subticks.size) || current.capacity>65536) return false;
            int allocated{};
            if(!current.entries||!io.read(&allocated,current.entries,sizeof(allocated))||
                allocated<std::max(current.size,initialBase.subticks.size)||allocated>current.capacity)return false;
            bool success=true;
            if(!originalAllocation.empty()&&!io.write(current.entries+8,originalAllocation.data(),originalAllocation.size()*sizeof(Address))){
                success=false;io.write(current.entries+8,originalAllocation.data(),originalAllocation.size()*sizeof(Address));}
            current.size=initialBase.subticks.size;base.subticks=current;
            if(!io.write(field+offsetof(RepeatedField,size),&current.size,sizeof(current.size))){
                success=false;io.write(field+offsetof(RepeatedField,size),&current.size,sizeof(current.size));}
            return success;
        };
        if(movement.replaceCount&&movement.count){
            auto partition=retained;
            for(const auto raw:previousSteps)if(std::ranges::find(replaced,raw)!=replaced.end())partition.push_back(raw);
            const auto count=static_cast<int>(retained.size());
            if(!io.write(base.subticks.entries+8,partition.data(),partition.size()*sizeof(Address))||
                !io.write(field+offsetof(RepeatedField,size),&count,sizeof(count))){restoreCount();return false;}
        }
        for (std::size_t index=0;index<movement.count;++index)
        {
            const auto raw=provider.acquireSubtick(field);
            if(!intact())return false;
            if (!raw || std::ranges::find(acquired,raw)!=acquired.end() || (std::ranges::find(previousSteps,raw)!=previousSteps.end()&&std::ranges::find(replaced,raw)==replaced.end())) { restoreCount(); return false; }
            acquired.push_back(raw);
            RepeatedField current{};Address appended{};
            if(!io.read(&current,field,sizeof(current))||current.size!=static_cast<int>(retained.size()+acquired.size())||
                current.capacity<current.size||current.capacity>65536||!current.entries||
                !io.read(&appended,current.entries+8+(current.size-1)*sizeof(Address),sizeof(appended))||appended!=raw){restoreCount();return false;}
        }
        // Native append may grow and free the old pointer array. Retain the new
        // storage even on failure; rollback restores logical content and size.
        if (movement.count && !restoreCount()) return false;
        std::vector<Write> writes;
        const auto planBytes=[&](Address address,const void *after,std::size_t size)
        {
            if (!address || !size || address>std::numeric_limits<Address>::max()-size) return false;
            Write write{address,std::vector<std::byte>(size),std::vector<std::byte>(size)};
            if (!io.read(write.before.data(),address,write.before.size())) return false;
            std::memcpy(write.after.data(),after,size);
            for(const auto &old:writes) if(address<old.address+old.after.size()&&old.address<address+write.after.size()) return false;
            writes.push_back(std::move(write));return true;
        };
        const auto plan=[&](Address address,const auto &after){return planBytes(address,&after,sizeof(after));};
        auto replacedButtons=movement.clearButtons|movement.setButtons;
        for(std::size_t index=0;index<movement.count;++index)if(movement.steps[index].bits&1)replacedButtons|=movement.steps[index].button;
        if(movement.replaceAnalog||replacedButtons)
        {
            for(const auto raw:retained)
            {
                SubtickPayload step{};
                if(!io.read(&step,raw+kMessagePayload,sizeof(step)))return false;
                bool changed=false;
                // Source clears all subticks. Retain events outside this plan's
                // ownership, including separate bits in a mixed-button event.
                if(movement.replaceAnalog&&(step.bits&0x18)){step.bits&=~0x18u;step.forward=step.left=0;changed=true;}
                if((step.bits&1)&&(step.button&replacedButtons)){
                    step.button&=~replacedButtons;changed=true;
                    if(!step.button){step.bits&=~3u;step.pressed=false;}
                }
                if(changed&&!plan(raw+kMessagePayload,step))return false;
            }
        }
        for(std::size_t index=0;index<acquired.size();++index)
        {
            const auto payload=acquired[index]+kMessagePayload;
            if(overlaps(payload,sizeof(SubtickPayload),command,0x98)||overlaps(payload,sizeof(SubtickPayload),baseAddress-kMessagePayload,sizeof(BaseCommand)+kMessagePayload)||
                overlaps(payload,sizeof(SubtickPayload),base.buttons,sizeof(ButtonPayload)+kMessagePayload)||
                overlaps(payload,sizeof(SubtickPayload),base.angles,sizeof(AnglePayload)+kMessagePayload)||
                overlaps(payload,sizeof(SubtickPayload),base.subticks.entries,8+base.subticks.capacity*sizeof(Address)))return false;
            for(const auto raw:retained)if(overlaps(payload,sizeof(SubtickPayload),raw,sizeof(SubtickPayload)+kMessagePayload))return false;
            if(!plan(payload,movement.steps[index]))return false;
        }
        struct Timed {Address raw;float when;};std::vector<Timed> chronological;
        for(const auto raw:retained){SubtickPayload step{};
            if(!io.read(&step,raw+kMessagePayload,sizeof(step))||!std::isfinite(step.when)||step.when<0||step.when>1)return false;
            // The current native converter reads raw when even without bit4.
            chronological.push_back({raw,step.when});}
        for(std::size_t index=0;index<acquired.size();++index)chronological.push_back({acquired[index],movement.steps[index].when});
        std::stable_sort(chronological.begin(),chronological.end(),[](const auto &a,const auto &b){return a.when<b.when;});
        base.subticks.size=static_cast<int>(chronological.size());
        if(base.subticks.entries){
            int allocated{};if(!io.read(&allocated,base.subticks.entries,sizeof(allocated))||allocated<base.subticks.size||allocated>base.subticks.capacity)return false;
            std::vector<Address> storage(allocated),ordered;ordered.reserve(allocated);
            if(!storage.empty()&&!io.read(storage.data(),base.subticks.entries+8,storage.size()*sizeof(Address)))return false;
            auto unique=storage;std::ranges::sort(unique);if(std::adjacent_find(unique.begin(),unique.end())!=unique.end())return false;
            for(const auto &entry:chronological)ordered.push_back(entry.raw);
            for(const auto raw:storage)if(std::ranges::none_of(chronological,[&](const auto &entry){return entry.raw==raw;}))ordered.push_back(raw);
            if(ordered.size()!=storage.size())return false;
            if(ordered!=storage&&!planBytes(base.subticks.entries+8,ordered.data(),ordered.size()*sizeof(Address)))return false;
        }
        if(movement.analog){base.forward=(*movement.analog)[0];base.left=(*movement.analog)[1];base.bits|=0xc0;}
        if(movement.zeroBaseAnalog){
            base.forward=base.left=0; base.bits|=0xc0;
        }
        if(movement.angles)
        {
            angles.bits|=7;angles.pitch=(*movement.angles)[0];angles.yaw=std::remainder((*movement.angles)[1],360.f);angles.roll=0;
            base.bits|=4;if(!plan(base.angles+kMessagePayload,angles))return false;
        }
        buttons.value=(buttons.value&~movement.clearButtons)|movement.setButtons;
        buttons.changed|=originalButtons.value^buttons.value;
        buttons.scroll&=~movement.clearButtons;
        if(!base.buttons)
        {
            base.buttons=io.allocateButtons(baseAddress-kMessagePayload);
            if(!base.buttons||!io.read(&buttonPayload,base.buttons+kMessagePayload,sizeof(buttonPayload)))return false;
        }
        const auto previousValue=buttonPayload.bits&1?buttonPayload.value:0;
        const auto previousChanged=buttonPayload.bits&2?buttonPayload.changed:0;
        const auto previousScroll=buttonPayload.bits&4?buttonPayload.scroll:0;
        buttonPayload.bits|=7;
        // Synchronize only this plan's buttons with final raw input. Analog
        // steering owns direction keys; unrelated jump, wheel, duck and weapon
        // fields remain independent until a plan explicitly edits them.
        const auto syncButtons=replacedButtons|(movement.analog?std::uint64_t{8|16|512|1024}:0);
        buttonPayload.value=(previousValue&~syncButtons)|(buttons.value&syncButtons);
        buttonPayload.changed=(previousChanged&~syncButtons)|(buttons.changed&syncButtons);
        buttonPayload.scroll=(previousScroll&~syncButtons)|(buttons.scroll&syncButtons);base.bits|=3;
        if(!plan(base.buttons+kMessagePayload,buttonPayload)||!plan(command+kCommandButtons,buttons)||!plan(baseAddress,base))return false;
        // No movement plan may alter a shot history, even through aliased objects.
        for(const auto raw:history)
        {
            HistoryHeader header{};
            if(!raw||raw>std::numeric_limits<Address>::max()-kMessagePayload-0x68||!io.read(&header,raw+kMessagePayload,sizeof(header)))return false;
            for(const auto &write:writes)
            {
                const auto overlaps=[&](Address start,std::size_t size){return start&&start<=std::numeric_limits<Address>::max()-size&&
                    write.address<start+size&&start<write.address+write.after.size();};
                if(overlaps(raw,kMessagePayload+0x68)||(header.angles&&(header.angles>std::numeric_limits<Address>::max()-kMessagePayload||
                    overlaps(header.angles,kMessagePayload+sizeof(AnglePayload)))))return false;
            }
        }
        // Restore only the size before any planned write. Old vector allocation
        // remains owned by the engine and reused by subsequent command edits.
        if(!intact())return false;
        std::size_t applied{};
        const auto rollback=[&]{while(applied){const auto &write=writes[--applied];io.write(write.address,write.before.data(),write.before.size());}};
        for(const auto &write:writes){++applied;if(!io.write(write.address,write.after.data(),write.after.size())){rollback();return false;}}
        const auto crc=Serialize(buttonPayload,angles);
        if(!io.crc(baseAddress-kMessagePayload,baseAddress+offsetof(BaseCommand,moveCrc),crc.bytes.data(),crc.size))
        {
            rollback();const auto previous=Serialize(originalButtonPayload,originalAngles);
            io.crc(baseAddress-kMessagePayload,baseAddress+offsetof(BaseCommand,moveCrc),previous.bytes.data(),previous.size);return false;
        }
        if(provider.committedSubticks){provider.committedSubticks->fill(0);std::copy(acquired.begin(),acquired.end(),provider.committedSubticks->begin());}
        return true;
    }

}
