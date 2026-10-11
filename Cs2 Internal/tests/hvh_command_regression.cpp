#include "game/combat_command.h"
#include "features/combat/detail/hvh_input.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace combat_bindings;
    using Address = combat_command::Address;
    constexpr std::size_t baseRaw = 0x100, buttonRaw = 0x240, angleRaw = 0x280;
    constexpr std::array<std::size_t, 3> historyRaw{0x400, 0x500, 0x600};
    constexpr std::array<std::size_t, 3> historyAngles{0x700, 0x740, 0x780};
    constexpr std::size_t historyRep = 0x800, cameraOffset = 0xa00, allocatedButtonRaw = 0xd00;
    constexpr Address goodCrc = 0xcafe;
    struct FullHistory
    {
        std::uint32_t bits{}, cachedSize{};
        Address angles{}, clientInterpolation{}, serverInterpolation0{}, serverInterpolation1{}, playerInterpolation{};
        Address shootPosition{}, targetHead{}, targetPosition{}, targetAngles{};
        int renderTick{};
        float renderFraction{};
        int playerTick{};
        float playerFraction{};
        int frameNumber{}, targetIndex{};
    };
    static_assert(offsetof(FullHistory, renderTick) == 0x50);
    static_assert(offsetof(FullHistory, playerTick) == kHistoryPlayerTick);
    static_assert(offsetof(FullHistory, playerFraction) == kHistoryPlayerFraction);
    static_assert(sizeof(FullHistory) == 0x68);

    void Check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    bool Near(float a, float b) { return std::abs(a - b) < 1e-6f; }
    struct Fixture
    {
        std::array<std::byte, 8192> bytes{};
        unsigned writes{}, failWrite{}, crcCalls{}, allocations{};
        bool failCrc{}, failAllocation{};
        unsigned subtickAcquires{},failSubtickAcquire{};bool aliasSubtick{};
        int mutateSubtick{},mutateButtons{};
        std::array<Address,64> committedSteps{};
        std::vector<std::vector<std::uint8_t>> serialized;
        Address At(std::size_t offset = 0) const { return reinterpret_cast<Address>(bytes.data()) + offset; }
        template<class T> void Put(std::size_t offset, const T& value)
        {
            Check(offset <= bytes.size() && sizeof(value) <= bytes.size() - offset, "fixture write exceeds memory");
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        }
        template<class T> T Get(std::size_t offset) const
        {
            Check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture read exceeds memory");
            T value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }
        bool Contains(Address address, std::size_t size) const
        {
            return address >= At() && address <= At() + bytes.size() && size <= At() + bytes.size() - address;
        }
        Fixture()
        {
            bytes.fill(std::byte{0x59});
            Put(kCommandSequence, 77);
            CommandProto proto{};
            proto.bits = 1; proto.cachedSize = 55; proto.base = At(baseRaw);
            proto.history = {0, 3, 3, At(historyRep)};
            proto.leftHand = true; proto.attack1History = -1; proto.attack2History = 7;
            Put(kCommandProto, proto);
            Put(kCommandButtons, ButtonState{0x123456, 8, 0, 0});
            BaseCommand base{};
            base.bits = 0xc0; base.cachedSize = 111; base.moveCrc = goodCrc;
            base.buttons = At(buttonRaw); base.angles = At(angleRaw);
            base.forward = 1; base.clientTick = 1234; base.mouseX = 23; base.mouseY = -17;
            base.subticks = {0, 2, 2, At(0x980)};
            Put(baseRaw + kMessagePayload, base);
            Put(buttonRaw + kMessagePayload, ButtonPayload{7, 41, 8, 0, 0});
            Put(angleRaw + kMessagePayload, AnglePayload{7, 42, 4, 0, 0});
            for (std::size_t i = 0; i < historyRaw.size(); ++i)
            {
                FullHistory entry{};
                entry.bits = 0x628; entry.cachedSize = 100 + static_cast<unsigned>(i);
                entry.angles = At(historyAngles[i]);
                entry.clientInterpolation = At(0xb00); entry.serverInterpolation0 = At(0xb40);
                entry.serverInterpolation1 = At(0xb80); entry.playerInterpolation = At(0xbc0);
                entry.shootPosition = At(0xc00); entry.targetHead = At(0xc40);
                entry.targetPosition = At(0xc80); entry.targetAngles = At(0xcc0);
                entry.renderTick = 500 + static_cast<int>(i); entry.renderFraction = .23f;
                entry.playerTick = 70 + static_cast<int>(i); entry.playerFraction = .3f;
                entry.frameNumber = 800 + static_cast<int>(i); entry.targetIndex = 99;
                Put(historyRaw[i] + kMessagePayload, entry);
                Put(historyAngles[i] + kMessagePayload, AnglePayload{0, 200 + static_cast<unsigned>(i), 3, 1, 2});
                Put(historyRep + 8 + i * sizeof(Address), At(historyRaw[i]));
            }
            Put(historyRep, 3);
            SubtickPayload subtick{};
            subtick.bits = 8; subtick.button = 0x80; subtick.pressed = true; subtick.when = .4f;
            subtick.forward = .2f; subtick.left = std::numeric_limits<float>::quiet_NaN();
            Put(0x900 + kMessagePayload, subtick);
            subtick.bits = 16; subtick.forward = std::numeric_limits<float>::quiet_NaN(); subtick.left = .05f;
            Put(0x940 + kMessagePayload, subtick);
            Put(0x980, 2); Put(0x988, At(0x900)); Put(0x990, At(0x940));
            Put(cameraOffset, std::array<float, 3>{41, 23, 9});
            Put(allocatedButtonRaw + kMessagePayload, ButtonPayload{});
        }
    };
    Fixture* active{};
    void MutateCommand(int kind)
    {
        if(kind==1)active->Put(kCommandSequence,78);
        if(kind==2){auto value=active->Get<CommandProto>(kCommandProto);++value.cachedSize;active->Put(kCommandProto,value);}
        if(kind==3){auto value=active->Get<BaseCommand>(baseRaw+kMessagePayload);value.forward=.75f;active->Put(baseRaw+kMessagePayload,value);}
        if(kind==4){auto value=active->Get<ButtonState>(kCommandButtons);value.value|=1ull<<5;active->Put(kCommandButtons,value);}
        if(kind==5){auto value=active->Get<ButtonPayload>(buttonRaw+kMessagePayload);value.value|=kAttack;active->Put(buttonRaw+kMessagePayload,value);}
        if(kind==6){auto value=active->Get<AnglePayload>(angleRaw+kMessagePayload);value.yaw+=1;active->Put(angleRaw+kMessagePayload,value);}
    }
    bool Read(void* destination, Address address, std::size_t size)
    {
        if (!destination || !active->Contains(address, size)) return false;
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    }
    bool Write(Address address, const void* source, std::size_t size)
    {
        if (!active->Contains(address, size)) return false;
        if (++active->writes == active->failWrite)
        {
            if (size) std::memcpy(reinterpret_cast<void*>(address), source, std::max(std::size_t{1}, size / 2));
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    }
    Address Allocate(Address raw)
    {
        Check(raw == active->At(baseRaw), "allocation used wrong raw base");
        ++active->allocations;
        MutateCommand(active->mutateButtons);
        return active->failAllocation ? 0 : active->At(allocatedButtonRaw);
    }
    bool Crc(Address raw, Address slot, const std::uint8_t* bytes, int size)
    {
        Check(raw == active->At(baseRaw) && slot == active->At(baseRaw + kMessagePayload + offsetof(BaseCommand, moveCrc)),
              "CRC used wrong destination or raw message");
        Check(size >= 0 && size <= 64, "invalid CRC size");
        active->serialized.emplace_back(bytes, bytes + size);
        ++active->crcCalls;
        const bool failure = active->failCrc && active->crcCalls == 1;
        const Address value = failure ? 0xbad0 : goodCrc;
        std::memcpy(reinterpret_cast<void*>(slot), &value, sizeof(value));
        return !failure;
    }
    constexpr combat_command::IO io{Read, Write, Allocate, Crc};
    Address AcquireSubtick(Address field)
    {
        Check(field==active->At(baseRaw+kMessagePayload+offsetof(BaseCommand,subticks)),"subtick append received wrong field");
        if(++active->subtickAcquires==active->failSubtickAcquire)return 0;
        auto data=active->Get<RepeatedField>(baseRaw+kMessagePayload+offsetof(BaseCommand,subticks));
        int allocated=data.entries?active->Get<int>(data.entries-active->At()):0;
        if(!data.entries){data.entries=active->At(0x1800);data.capacity=64;active->Put(0x1800,0);}
        if(data.capacity==2)
        {
            data.entries=active->At(0x1800);data.capacity=64;
            active->Put(0x1800,allocated);active->Put(0x1808,active->At(0x900));active->Put(0x1810,active->At(0x940));
        }
        Address raw{};
        if(data.size<allocated)raw=active->Get<Address>(data.entries-active->At()+8+data.size*sizeof(Address));
        else
        {
            raw=active->aliasSubtick?active->At(historyRaw[0]):active->At(0xe00+allocated*0x40);
            if(!active->aliasSubtick)active->Put(raw-active->At()+kMessagePayload,SubtickPayload{});
            active->Put(data.entries-active->At()+8+allocated*sizeof(Address),raw);
            active->Put(data.entries-active->At(),++allocated);
        }
        ++data.size;active->Put(baseRaw+kMessagePayload+offsetof(BaseCommand,subticks),data);
        MutateCommand(active->mutateSubtick);return raw;
    }
    bool MovementEdit(Fixture &fixture,const combat_command::Movement &movement)
    {
        active=&fixture;
        return combat_command::EditMovement(fixture.At(),fixture.At(baseRaw+kMessagePayload),77,movement,{io,AcquireSubtick,&fixture.committedSteps});
    }
    constexpr std::array<float, 3> baseAim{10, 90, 123};
    constexpr combat_command::Shot shot{{15, 35, -450}, 777, .625f};
    bool Edit(Fixture& fixture, std::optional<std::array<float, 3>> aim = baseAim, bool attack = true,
              std::optional<combat_command::Shot> timing = shot, int sequence = 77)
    {
        active = &fixture;
        return combat_command::Edit(fixture.At(), fixture.At(baseRaw + kMessagePayload), sequence, aim, attack, io, timing);
    }

    struct Decoded
    {
        std::array<std::uint64_t, 3> buttons{};
        std::array<float, 3> angles{};
        std::array<bool, 3> anglePresent{};
    };
    Decoded Decode(const std::vector<std::uint8_t>& bytes)
    {
        Decoded result;
        std::size_t cursor = 0;
        while (cursor < bytes.size())
        {
            const auto tag = bytes[cursor++];
            Check((tag == 0x1a || tag == 0x22) && cursor < bytes.size(), "unexpected CRC message tag");
            const auto end = cursor + 1 + bytes[cursor];
            ++cursor;
            Check(end <= bytes.size(), "CRC message length exceeds buffer");
            while (cursor < end)
            {
                const auto field = bytes[cursor++];
                const auto index = (field >> 3) - 1;
                Check(index >= 0 && index < 3, "unexpected CRC field number");
                if (tag == 0x1a)
                {
                    Check((field & 7) == 1 && end - cursor >= 8, "button CRC field is not fixed64");
                    std::memcpy(&result.buttons[index], bytes.data() + cursor, 8); cursor += 8;
                }
                else
                {
                    Check((field & 7) == 5 && end - cursor >= 4, "angle CRC field is not fixed32");
                    std::memcpy(&result.angles[index], bytes.data() + cursor, 4); cursor += 4;
                    result.anglePresent[index] = true;
                }
            }
        }
        return result;
    }

    void SecondaryScopeTransactions()
    {
        constexpr std::uint64_t secondary = 1ull << 11;
        const auto scope = [](Fixture &fixture) {
            active = &fixture;
            return combat_command::Edit(fixture.At(), fixture.At(baseRaw + kMessagePayload), 77,
                std::nullopt, true, io, std::nullopt, combat_command::AngleMode::ShotHistory,
                combat_command::FireButton::Secondary);
        };
        const auto manual = [](Fixture &fixture) {
            auto payload = fixture.Get<ButtonPayload>(buttonRaw + kMessagePayload);
            payload.value |= kAttack; payload.changed |= kAttack; payload.scroll |= kAttack;
            fixture.Put(buttonRaw + kMessagePayload, payload);
            auto proto = fixture.Get<CommandProto>(kCommandProto);
            proto.bits |= 0x20; proto.attack1History = 1;
            fixture.Put(kCommandProto, proto);
        };
        Fixture success; manual(success);
        const auto before = success.bytes;
        Check(scope(success), "Secondary scope transaction failed");
        const auto raw = success.Get<ButtonState>(kCommandButtons);
        const auto payload = success.Get<ButtonPayload>(buttonRaw + kMessagePayload);
        const auto proto = success.Get<CommandProto>(kCommandProto);
        Check((raw.value & secondary) && (raw.changed & secondary) && (raw.scroll & secondary) &&
              !(raw.value & kAttack), "Scope did not send only the secondary raw action");
        Check((payload.value & (kAttack | secondary)) == (kAttack | secondary) &&
              (payload.changed & (kAttack | secondary)) == (kAttack | secondary) &&
              (payload.scroll & (kAttack | secondary)) == (kAttack | secondary),
              "Scope erased a protobuf-only manual primary action");
        Check((proto.bits & 0x40) && proto.attack2History == 2 && proto.attack1History == 1 && (proto.bits & 0x20),
              "Scope changed primary history or omitted secondary history");
        Check(std::equal(before.begin() + historyRaw[0], before.begin() + historyRep,
                         success.bytes.begin() + historyRaw[0]) &&
              !std::memcmp(before.data() + angleRaw, success.bytes.data() + angleRaw, 0x40),
              "Scope changed shot history, timing or base aim");
        const auto encoded = Decode(success.serialized.front());
        Check(encoded.buttons[0] == payload.value && encoded.angles == std::array<float,3>{4,0,0},
              "Scope checksum does not describe preserved aim and secondary buttons");
        for (unsigned index = 1; index <= success.writes; ++index) {
            Fixture failure; manual(failure); failure.failWrite = index;
            const auto original = failure.bytes;
            Check(!scope(failure) && failure.bytes == original && !failure.crcCalls,
                  "Secondary partial-write failure did not roll back command/history/buttons");
        }
        Fixture crc; manual(crc); crc.failCrc = true;
        const auto original = crc.bytes;
        Check(!scope(crc) && crc.bytes == original && crc.crcCalls == 2,
              "Secondary CRC failure did not restore its full transaction and old checksum");
        Fixture held;
        held.Put(kCommandButtons, ButtonState{0x123456, kAttack | 8, kAttack, 0});
        Check(scope(held) && (held.Get<ButtonState>(kCommandButtons).value & kAttack),
              "Secondary action erased a raw manual primary action");
        for (int invalid = 0; invalid < 3; ++invalid) {
            Fixture rejected; active = &rejected;
            Check(!combat_command::Edit(rejected.At(), rejected.At(baseRaw + kMessagePayload), 77,
                    invalid == 0 ? std::optional{baseAim} : std::nullopt, invalid != 1, io,
                    invalid == 2 ? std::optional{shot} : std::nullopt, combat_command::AngleMode::ShotHistory,
                    combat_command::FireButton::Secondary) && !rejected.writes && !rejected.crcCalls,
                  "Secondary action accepted a shot, aiming edit or non-press request");
        }
    }

    void ValidShot()
    {
        Fixture fixture;
        const auto before = fixture.bytes;
        const auto previousBase = fixture.Get<BaseCommand>(baseRaw + kMessagePayload);
        std::array<FullHistory, 3> previous;
        for (std::size_t i = 0; i < previous.size(); ++i) previous[i] = fixture.Get<FullHistory>(historyRaw[i] + kMessagePayload);
        Check(Edit(fixture), "valid shot edit failed");
        const auto base = fixture.Get<BaseCommand>(baseRaw + kMessagePayload);
        const auto angles = fixture.Get<AnglePayload>(angleRaw + kMessagePayload);
        Check(angles.pitch == 10 && angles.yaw == 90 && angles.roll == 0 && (angles.bits & 7) == 7,
              "base angle received shot roll or wrong aim");
        Check(base.clientTick == previousBase.clientTick && base.mouseX == 23 && base.mouseY == -17,
              "shot changed base timing or mouse input");
        Check(Near(base.forward, 0) && Near(base.left, -1) && (base.bits & 0xc7) == 0xc7,
              "movement did not rotate with base yaw");
        for (std::size_t i = 0; i < previous.size(); ++i)
        {
            const auto history = fixture.Get<FullHistory>(historyRaw[i] + kMessagePayload);
            auto expected = previous[i];
            expected.bits |= 1 | kHistoryPlayerTickBit | kHistoryPlayerFractionBit;
            expected.playerTick = shot.tick; expected.playerFraction = shot.fraction;
            Check(std::memcmp(&history, &expected, sizeof(history)) == 0,
                  "shot timing changed render/interpolation fields or missed presence bits");
            const auto angle = fixture.Get<AnglePayload>(historyAngles[i] + kMessagePayload);
            Check(angle.pitch == 15 && angle.yaw == 35 && angle.roll == -90 && (angle.bits & 7) == 7,
                  "history lost distinct shot angle or nonzero roll");
            Check(angle.cachedSize == 200 + i, "shot overwrote history angle metadata");
            Check(std::equal(before.begin() + historyRaw[i], before.begin() + historyRaw[i] + kMessagePayload,
                             fixture.bytes.begin() + historyRaw[i]), "shot modified raw message header");
        }
        const auto proto = fixture.Get<CommandProto>(kCommandProto);
        Check((proto.bits & 0x20) && proto.attack1History == 2 && proto.attack2History == 7 && proto.leftHand,
              "attack does not reference final shot history or changed unrelated command state");
        const auto buttons = fixture.Get<ButtonState>(kCommandButtons);
        const auto payload = fixture.Get<ButtonPayload>(buttonRaw + kMessagePayload);
        Check((buttons.value & kAttack) && (buttons.changed & kAttack) && (buttons.scroll & kAttack) &&
              (buttons.value & 1024) && !(buttons.value & 8), "attack or rotated digital movement missing");
        Check(payload.value == buttons.value && payload.changed == buttons.changed && payload.scroll == buttons.scroll &&
              (payload.bits & 7) == 7, "protobuf/native button states disagree");
        Check(fixture.crcCalls == 1 && fixture.allocations == 0, "shot does not commit one final CRC");
        const auto decoded = Decode(fixture.serialized[0]);
        Check(decoded.buttons == std::array<std::uint64_t, 3>{buttons.value, buttons.changed, buttons.scroll} &&
              decoded.angles == std::array<float, 3>{10, 90, 0} && !decoded.anglePresent[2],
              "CRC serialized rolled history angles instead of base state");
        Check(std::equal(before.begin() + cameraOffset, before.begin() + 0xd00, fixture.bytes.begin() + cameraOffset),
              "camera or interpolation object changed");
        const auto first = fixture.Get<SubtickPayload>(0x900 + kMessagePayload);
        const auto second = fixture.Get<SubtickPayload>(0x940 + kMessagePayload);
        Check(Near(first.forward, 0) && Near(first.left, -.2f) && Near(second.forward, .05f) && Near(second.left, 0) &&
              first.when == .4f && first.pressed && first.button == 0x80, "subtick movement or unrelated fields changed");
    }

    void InvalidShots()
    {
        auto reject = [](auto mutate, std::optional<std::array<float, 3>> aim = baseAim, bool attack = true,
                         std::optional<combat_command::Shot> timing = shot, int sequence = 77)
        {
            Fixture fixture; mutate(fixture);
            const auto before = fixture.bytes;
            Check(!Edit(fixture, aim, attack, timing, sequence), "invalid shot accepted");
            Check(fixture.writes == 0 && fixture.crcCalls == 0 && fixture.bytes == before,
                  "invalid shot mutated command before validation completed");
        };
        const auto noop = [](Fixture&) {};
        reject(noop, std::nullopt);
        reject(noop, baseAim, false);
        reject(noop, baseAim, true, shot, 76);
        reject([](Fixture& f) { f.Put(kCommandSequence, 76); });
        reject([](Fixture& f) { auto p = f.Get<CommandProto>(kCommandProto); p.bits &= ~1u; f.Put(kCommandProto, p); });
        reject([](Fixture& f) { auto p = f.Get<CommandProto>(kCommandProto); p.history.size = 0; f.Put(kCommandProto, p); });
        reject([](Fixture& f) { auto p = f.Get<CommandProto>(kCommandProto); p.history.capacity = 2; f.Put(kCommandProto, p); });
        reject([](Fixture& f) { f.Put(historyRep, 2); });
        reject([](Fixture& f) { f.Put(historyRep + 16, Address{}); });
        reject([](Fixture& f) { auto h = f.Get<FullHistory>(historyRaw[1] + kMessagePayload); h.angles = 0; f.Put(historyRaw[1] + kMessagePayload, h); });
        reject([](Fixture& f) {
            const auto raw = f.bytes.size() - 40;
            f.Put(historyRep + 16, f.At(raw));
            f.Put(raw + kMessagePayload, HistoryHeader{0, 0, f.At(historyAngles[1])});
        });
        reject([](Fixture& f) { auto h = f.Get<FullHistory>(historyRaw[1] + kMessagePayload); h.angles = f.At(historyRaw[1] + kHistoryPlayerTick); f.Put(historyRaw[1] + kMessagePayload, h); });
        for (int tick : {0, -1})
        {
            auto invalid = shot; invalid.tick = tick;
            reject(noop, baseAim, true, invalid);
        }
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        for (float fraction : {-.01f, 1.f, nan, inf})
        {
            auto invalid = shot; invalid.fraction = fraction;
            reject(noop, baseAim, true, invalid);
        }
        for (int component = 0; component < 3; ++component)
        {
            auto invalid = shot; invalid.angles[component] = nan;
            reject(noop, baseAim, true, invalid);
            auto aim = baseAim; aim[component] = inf;
            reject(noop, aim);
        }
        for (float pitch : {-90.f, 90.f})
        {
            auto invalid = shot; invalid.angles[0] = pitch;
            reject(noop, baseAim, true, invalid);
            auto aim = baseAim; aim[0] = pitch;
            reject(noop, aim);
        }
        reject([](Fixture& f) { auto b = f.Get<BaseCommand>(baseRaw + kMessagePayload); b.buttons = 0; f.Put(baseRaw + kMessagePayload, b); f.failAllocation = true; });
    }

    void RollbackAndSharedAngles()
    {
        Fixture successful;
        Check(Edit(successful), "cannot enumerate valid transaction writes");
        const auto totalWrites = successful.writes;
        Check(totalWrites > 10, "fixture lacks full shot transaction");
        // Fail every staged write after partially copying it, including timing,
        // angles, presence headers, buttons, command and final base messages.
        for (unsigned index = 1; index <= totalWrites; ++index)
        {
            Fixture fixture; fixture.failWrite = index;
            const auto before = fixture.bytes;
            Check(!Edit(fixture) && fixture.bytes == before && fixture.crcCalls == 0,
                  "partial-write failure did not restore entire shot transaction");
        }
        Fixture crcFailure; crcFailure.failCrc = true;
        const auto before = crcFailure.bytes;
        Check(!Edit(crcFailure) && crcFailure.bytes == before && crcFailure.crcCalls == 2,
              "CRC failure did not restore stamps, roll, base and old checksum");
        const auto restored = Decode(crcFailure.serialized[1]);
        Check(restored.buttons == std::array<std::uint64_t, 3>{8, 0, 0} &&
              restored.angles == std::array<float, 3>{4, 0, 0}, "rollback rebuilt CRC from modified state");
        Fixture shared;
        auto header = shared.Get<FullHistory>(historyRaw[1] + kMessagePayload);
        header.angles = shared.At(historyAngles[0]); shared.Put(historyRaw[1] + kMessagePayload, header);
        Check(Edit(shared), "identical shared history angle object was rejected");
        Check(shared.Get<AnglePayload>(historyAngles[0] + kMessagePayload).roll == -90 &&
              shared.Get<FullHistory>(historyRaw[1] + kMessagePayload).playerTick == 777,
              "shared angle transaction lost timing or roll");
        Fixture boundary;
        auto finalFraction = shot; finalFraction.fraction = std::nextafter(1.f, 0.f);
        Check(Edit(boundary, baseAim, true, finalFraction), "valid fraction immediately below one rejected");
    }

    void BaseOnlyOrientation()
    {
        const auto orient=[](Fixture &fixture) {
            active=&fixture;
            return combat_command::Edit(fixture.At(),fixture.At(baseRaw+kMessagePayload),77,
                std::array<float,3>{89,90,0},false,io,std::nullopt,combat_command::AngleMode::BaseOnly);
        };
        Fixture fixture;const auto before=fixture.bytes;
        Check(orient(fixture),"Base-only orientation transaction failed");
        const auto angles=fixture.Get<AnglePayload>(angleRaw+kMessagePayload);
        Check(angles.pitch==89 && angles.yaw==90 && angles.roll==0 && fixture.crcCalls==1,
              "Orientation did not update base angle and checksum");
        Check(std::equal(before.begin()+historyRaw[0],before.begin()+historyRep,fixture.bytes.begin()+historyRaw[0]),
              "Base-only orientation changed a shot history header, timestamp or angle");
        const auto proto=fixture.Get<CommandProto>(kCommandProto);
        Check(proto.attack1History==-1 && proto.attack2History==7 &&
              std::equal(before.begin()+cameraOffset,before.begin()+0xd00,fixture.bytes.begin()+cameraOffset),
              "Orientation changed attack-history indices, camera or interpolation data");
        const auto buttons=fixture.Get<ButtonState>(kCommandButtons);
        Check(!((buttons.value|buttons.changed|buttons.scroll)&kAttack),"Orientation synthesized attack states");
        const auto base=fixture.Get<BaseCommand>(baseRaw+kMessagePayload);
        Check(Near(base.forward,0) && Near(base.left,-1),"Orientation did not preserve world movement");
        const auto totalWrites=fixture.writes;
        for(unsigned index=1;index<=totalWrites;++index) {
            Fixture partial;partial.failWrite=index;const auto original=partial.bytes;
            Check(!orient(partial)&&partial.bytes==original&&partial.crcCalls==0,
                  "Orientation partial-write failure did not roll back fully");
        }
        Fixture checksum;checksum.failCrc=true;const auto original=checksum.bytes;
        Check(!orient(checksum)&&checksum.bytes==original&&checksum.crcCalls==2,
              "Orientation CRC failure did not restore the prior command");
        Fixture alias;auto baseAlias=alias.Get<BaseCommand>(baseRaw+kMessagePayload);
        baseAlias.angles=alias.At(historyAngles[0]);alias.Put(baseRaw+kMessagePayload,baseAlias);
        const auto aliased=alias.bytes;
        Check(!orient(alias)&&alias.bytes==aliased&&!alias.writes,"Base/history angle alias altered protected history");
        Fixture invalid;active=&invalid;const auto unmodified=invalid.bytes;
        Check(!combat_command::Edit(invalid.At(),invalid.At(baseRaw+kMessagePayload),77,baseAim,true,io,
                std::nullopt,combat_command::AngleMode::BaseOnly) &&
              !combat_command::Edit(invalid.At(),invalid.At(baseRaw+kMessagePayload),77,baseAim,false,io,
                shot,combat_command::AngleMode::BaseOnly) && invalid.bytes==unmodified,
              "Base-only mode accepted an attack or timed shot");
    }
    void CommittedShotThenOrientation()
    {
        const auto orient=[](Fixture &f){active=&f;return combat_command::Edit(f.At(),f.At(baseRaw+kMessagePayload),77,
            std::array<float,3>{89,175,0},false,io,std::nullopt,combat_command::AngleMode::BaseOnly);};
        Fixture f;Check(Edit(f),"combined fixture shot failed");const auto committed=f.bytes;const auto shotWrites=f.writes;
        Check(orient(f),"AA after committed shot failed");
        Check(std::equal(committed.begin()+historyRaw[0],committed.begin()+historyRep,f.bytes.begin()+historyRaw[0]),
            "AA after shot altered history timing, spread roll or angle");
        const auto proto=f.Get<CommandProto>(kCommandProto);const auto beforeProto=*reinterpret_cast<const CommandProto*>(committed.data()+kCommandProto);
        Check(std::memcmp(&proto,&beforeProto,sizeof(proto))==0,"AA after shot altered history attack index");
        const auto raw=f.Get<ButtonState>(kCommandButtons);const auto pb=f.Get<ButtonPayload>(buttonRaw+kMessagePayload);
        Check(((raw.value&raw.changed&raw.scroll)&kAttack)&&((pb.value&pb.changed&pb.scroll)&kAttack),"AA erased committed attack flags");
        const auto count=f.writes-shotWrites;
        for(unsigned index=1;index<=count;++index){
            Fixture partial;Check(Edit(partial),"partial combined shot failed");const auto prior=partial.bytes;
            partial.failWrite=partial.writes+index;
            Check(!orient(partial)&&partial.bytes==prior,"AA failure corrupted a prior committed shot");
        }
        Fixture failed;Check(Edit(failed),"checksum combined shot failed");const auto prior=failed.bytes;
        failed.crcCalls=0;failed.failCrc=true;
        Check(!orient(failed)&&failed.bytes==prior,"AA checksum failure did not retain previous shot");
        for(std::uint64_t action:{kAttack,1ull<<11,1ull<<5}){
            Fixture pbOnly;auto buttons=pbOnly.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            buttons.value|=action;buttons.changed|=action;buttons.scroll|=action;pbOnly.Put(buttonRaw+kMessagePayload,buttons);
            Check(orient(pbOnly),"PB-only action orientation failed");const auto preserved=pbOnly.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            Check(((preserved.value&preserved.changed&preserved.scroll)&action)&&
                !((pbOnly.Get<ButtonState>(kCommandButtons).value)&action),"AA lost PB-only action or invented raw action");
            Fixture absent;buttons.bits=0;absent.Put(buttonRaw+kMessagePayload,buttons);Check(orient(absent),"absent PB flags rejected");
            const auto clean=absent.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            Check(!((clean.value|clean.changed|clean.scroll)&action),"absent PB fields became manual actions");
        }
    }
    combat_command::Movement MovementPlan()
    {
        combat_command::Movement result;result.analog=std::array{.25f,-.5f};result.replaceAnalog=true;
        result.clearButtons=8|16|512|1024;result.setButtons=8|1024|2;
        result.count=2;
        for(std::size_t i=0;i<result.count;++i){auto &step=result.steps[i];step.bits=0x7f;step.button=4;step.pressed=i==0;
            step.when=.1f+static_cast<float>(i)*.2f;step.forward=.1f;step.left=-.2f;}
        return result;
    }
    void MovementTransactions()
    {
        const auto prepare=[](Fixture &value){auto step=value.Get<SubtickPayload>(0x900+kMessagePayload);step.bits|=7;step.button=kAttack;step.pressed=true;step.when=.8f;
            value.Put(0x900+kMessagePayload,step);auto pb=value.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            pb.value|=kAttack;pb.changed|=kAttack;pb.scroll|=kAttack;value.Put(buttonRaw+kMessagePayload,pb);};
        Fixture f;prepare(f);const auto old=f.Get<SubtickPayload>(0x900+kMessagePayload);
        const auto before=f.bytes;const auto plan=MovementPlan();Check(MovementEdit(f,plan),"native movement transaction failed");
        const auto base=f.Get<BaseCommand>(baseRaw+kMessagePayload);
        Check(base.forward==.25f&&base.left==-.5f&&base.subticks.size==4&&base.subticks.capacity==64&&base.subticks.entries==f.At(0x1800),
            "movement did not retain grown native vector or analog state");
        const auto preserved=f.Get<SubtickPayload>(0x900+kMessagePayload);
        Check((preserved.bits&0x18)==0&&(preserved.bits&7)==7&&preserved.button==kAttack&&preserved.pressed&&preserved.when==old.when,
            "movement replacement erased a manual button or double-applied analog impulses");
        const auto currentPb=f.Get<ButtonPayload>(buttonRaw+kMessagePayload);
        const auto currentRaw=f.Get<ButtonState>(kCommandButtons);
        constexpr std::uint64_t movementButtons=2|4|8|16|512|1024;
        Check(((currentPb.value&currentPb.changed&currentPb.scroll)&kAttack)&&!(currentRaw.value&kAttack)&&
              ((currentPb.value^currentRaw.value)&movementButtons)==0&&((currentPb.changed^currentRaw.changed)&movementButtons)==0&&
              ((currentPb.scroll^currentRaw.scroll)&movementButtons)==0,
            "movement failed to synchronize owned input or erased an independent protobuf attack");
        Check(std::equal(before.begin()+historyRaw[0],before.begin()+historyRep,f.bytes.begin()+historyRaw[0])&&
            std::equal(before.begin()+cameraOffset,before.begin()+0xd00,f.bytes.begin()+cameraOffset),"movement altered shot history or camera");
        Check(std::memcmp(before.data()+angleRaw+kMessagePayload,f.bytes.data()+angleRaw+kMessagePayload,sizeof(AnglePayload))==0,
            "analog movement altered base angle");
        for(int i=0;i<2;++i){const auto raw=f.committedSteps[i];
            const auto payload=f.Get<SubtickPayload>(raw-f.At()+kMessagePayload);Check(std::memcmp(&payload,&plan.steps[i],sizeof(payload))==0,"native subtick payload incorrect");}
        const auto totalWrites=f.writes;
        for(unsigned index=1;index<=totalWrites;++index){
            Fixture partial;prepare(partial);partial.failWrite=index;const auto prior=partial.bytes;
            Check(!MovementEdit(partial,plan),"movement partial write accepted");
            const auto retained=partial.Get<BaseCommand>(baseRaw+kMessagePayload);
            Check(retained.subticks.size==2&&retained.subticks.entries==partial.At(0x1800)&&retained.subticks.capacity==64,
                "rollback discarded native allocation or retained active extra subticks");
            Check(std::memcmp(prior.data()+kCommandButtons,partial.bytes.data()+kCommandButtons,sizeof(ButtonState))==0&&
                std::memcmp(prior.data()+0x900,partial.bytes.data()+0x900,0x80)==0&&
                std::equal(prior.begin()+historyRaw[0],prior.begin()+historyRep,partial.bytes.begin()+historyRaw[0]),
                "movement partial write changed prior buttons, movement steps or history");
        }
        Fixture crc;crc.failCrc=true;const auto prior=crc.bytes;Check(!MovementEdit(crc,plan)&&crc.crcCalls==2,
            "movement checksum failure did not regenerate prior checksum");
        Check(crc.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==2&&
            std::memcmp(prior.data()+0x900,crc.bytes.data()+0x900,0x80)==0,"checksum rollback did not restore original movement");
        crc.failCrc=false;Check(MovementEdit(crc,plan)&&crc.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==4,
            "grown inactive native message storage was not reusable after rollback");
        Fixture allocation;allocation.failSubtickAcquire=2;Check(!MovementEdit(allocation,plan)&&
            allocation.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==2,"partial acquisition retained an active movement step");
        Fixture alias;alias.aliasSubtick=true;const auto protectedHistory=alias.Get<FullHistory>(historyRaw[0]+kMessagePayload);
        Check(!MovementEdit(alias,plan)&&std::memcmp(&protectedHistory,alias.bytes.data()+historyRaw[0]+kMessagePayload,sizeof(protectedHistory))==0,
            "subtick allocation alias corrupted history before validation");
        for(auto mask:{kAttack,1ull<<5,1ull<<11}){auto invalid=plan;invalid.setButtons=mask;Fixture rejected;
            Check(!MovementEdit(rejected,invalid)&&!rejected.writes&&!rejected.subtickAcquires,"movement owned a weapon action");}
        Fixture malformed;malformed.Put(0x980,3);Check(!MovementEdit(malformed,plan)&&!malformed.subtickAcquires,
            "malformed allocated-count field reached native append");
        Fixture angle;auto ladder=plan;ladder.angles=std::array{89.f,-175.f,0.f};
        Check(MovementEdit(angle,ladder)&&angle.Get<AnglePayload>(angleRaw+kMessagePayload).yaw==-175.f,"ladder base angle did not commit");
        Fixture historyAlias;auto aliasedBase=historyAlias.Get<BaseCommand>(baseRaw+kMessagePayload);aliasedBase.angles=historyAlias.At(historyAngles[0]);
        historyAlias.Put(baseRaw+kMessagePayload,aliasedBase);const auto untouched=historyAlias.Get<AnglePayload>(historyAngles[0]+kMessagePayload);
        Check(!MovementEdit(historyAlias,ladder)&&std::memcmp(&untouched,historyAlias.bytes.data()+historyAngles[0]+kMessagePayload,sizeof(untouched))==0,
            "movement base/history alias altered shot angles");
    }
    void ManualJumpDuringAnalogMovement()
    {
        combat_command::Movement analog;analog.analog=std::array{.25f,-.5f};analog.replaceAnalog=true;
        analog.clearButtons=8|16|512|1024;analog.setButtons=8|1024;
        constexpr std::uint64_t jump=2;
        for(int source=0;source<4;++source){Fixture f;
            auto raw=f.Get<ButtonState>(kCommandButtons);auto pb=f.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            if(source==0){raw.value|=jump;raw.changed|=jump;}
            if(source==1){pb.value|=jump;pb.changed|=jump;}
            if(source==2){raw.scroll|=jump;pb.scroll|=jump;}
            if(source==3){pb.scroll|=jump;}
            f.Put(kCommandButtons,raw);f.Put(buttonRaw+kMessagePayload,pb);
            auto native=f.Get<SubtickPayload>(0x900+kMessagePayload);native.bits|=7;native.button=jump;native.pressed=true;native.when=.25f;
            f.Put(0x900+kMessagePayload,native);const auto before=f.bytes;
            Check(MovementEdit(f,analog),"analog assist could not coexist with manual jump");
            const auto afterRaw=f.Get<ButtonState>(kCommandButtons);const auto afterPb=f.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            Check((afterRaw.value&jump)==(raw.value&jump)&&(afterRaw.changed&jump)==(raw.changed&jump)&&
                (afterRaw.scroll&jump)==(raw.scroll&jump)&&(afterPb.value&jump)==(pb.value&jump)&&
                (afterPb.changed&jump)==(pb.changed&jump)&&(afterPb.scroll&jump)==(pb.scroll&jump),
                "analog movement changed independent raw or protobuf jump intent");
            const auto after=f.Get<SubtickPayload>(0x900+kMessagePayload);
            Check((after.bits&7)==7&&after.button==jump&&after.pressed&&after.when==.25f,
                "analog assist erased a native takeoff jump subtick");
            Check(std::equal(before.begin()+historyRaw[0],before.begin()+historyRep,f.bytes.begin()+historyRaw[0]),
                "manual-jump movement touched working shot history");
        }
        Fixture release;auto step=release.Get<SubtickPayload>(0x900+kMessagePayload);
        step.bits|=7;step.button=jump;step.pressed=false;step.when=.8f;release.Put(0x900+kMessagePayload,step);
        Check(MovementEdit(release,analog)&&release.Get<SubtickPayload>(0x900+kMessagePayload).button==jump&&
            !release.Get<SubtickPayload>(0x900+kMessagePayload).pressed,"analog movement erased a native jump release");
        for(const std::uint64_t action:{jump,std::uint64_t{4},kAttack,1ull<<5,1ull<<11}){
            Fixture separate;auto raw=separate.Get<ButtonState>(kCommandButtons);raw.value=raw.changed=raw.scroll=0;
            auto pb=separate.Get<ButtonPayload>(buttonRaw+kMessagePayload);pb.value=pb.changed=pb.scroll=action;
            separate.Put(kCommandButtons,raw);separate.Put(buttonRaw+kMessagePayload,pb);
            Check(MovementEdit(separate,analog),"analog plan rejected independent protobuf action");
            const auto after=separate.Get<ButtonPayload>(buttonRaw+kMessagePayload);
            Check(((after.value&after.changed&after.scroll)&action)==action&&
                !(separate.Get<ButtonState>(kCommandButtons).value&action),
                "analog plan erased protobuf-only action or invented raw action");
        }
    }
    void MovementCallbackFreshness()
    {
        for(bool buttons:{false,true})for(int kind=1;kind<=6;++kind){
            Fixture f;auto plan=MovementPlan();
            if(buttons){auto base=f.Get<BaseCommand>(baseRaw+kMessagePayload);base.buttons=0;f.Put(baseRaw+kMessagePayload,base);
                plan.count=0;f.mutateButtons=kind;
                // There is no attached button message for this allocation path.
                if(kind==5)continue;
            }else f.mutateSubtick=kind;
            const auto before=f.bytes;
            Check(!MovementEdit(f,plan)&&!f.crcCalls,"Movement committed an allocator-mutated command");
            Check(std::equal(before.begin()+historyRaw[0],before.begin()+historyRep,f.bytes.begin()+historyRaw[0]),
                "Allocator freshness failure altered protected shot history");
            const auto base=f.Get<BaseCommand>(baseRaw+kMessagePayload);
            Check(base.forward==(kind==3?.75f:1.f)&&base.left==0,
                "Freshness failure applied a stale analog plan or rolled back new native input");
            Check(f.Get<int>(kCommandSequence)==(kind==1?78:77)&&
                f.Get<CommandProto>(kCommandProto).cachedSize==(kind==2?56u:55u)&&
                f.Get<AnglePayload>(angleRaw+kMessagePayload).yaw==(kind==6?1.f:0.f),
                "Freshness failure overwrote native command identity or angles");
            if(kind==4)Check(f.Get<ButtonState>(kCommandButtons).value&(1ull<<5),"Freshness failure overwrote native Use");
            if(kind==5)Check(f.Get<ButtonPayload>(buttonRaw+kMessagePayload).value&kAttack,"Freshness failure overwrote protobuf attack");
        }
    }
    void EmptyChronologicalAndRepeatedMovement()
    {
        const auto plan=MovementPlan();
        const auto ordered=[](const Fixture &f){const auto base=f.Get<BaseCommand>(baseRaw+kMessagePayload);float previous=-1;
            for(int index=0;index<base.subticks.size;++index){const auto raw=f.Get<Address>(base.subticks.entries-f.At()+8+index*sizeof(Address));
                const auto step=f.Get<SubtickPayload>(raw-f.At()+kMessagePayload);const auto when=step.when;
                Check(when>=previous,"native movement subticks are not chronologically ordered");previous=when;}
            const auto allocated=f.Get<int>(base.subticks.entries-f.At());std::vector<Address> unique;
            for(int index=0;index<allocated;++index)unique.push_back(f.Get<Address>(base.subticks.entries-f.At()+8+index*sizeof(Address)));
            std::ranges::sort(unique);Check(std::adjacent_find(unique.begin(),unique.end())==unique.end(),"native vector owns duplicate message pointers");};
        for(bool retained:{false,true}){Fixture empty;auto base=empty.Get<BaseCommand>(baseRaw+kMessagePayload);
            base.subticks=retained?RepeatedField{0,0,2,empty.At(0x980)}:RepeatedField{};empty.Put(baseRaw+kMessagePayload,base);
            Check(MovementEdit(empty,plan),"first movement window from empty native field failed");ordered(empty);
            Check(empty.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==2,"empty native field retained wrong active count");}
        Fixture failedEmpty;auto emptyBase=failedEmpty.Get<BaseCommand>(baseRaw+kMessagePayload);emptyBase.subticks={};failedEmpty.Put(baseRaw+kMessagePayload,emptyBase);
        failedEmpty.failSubtickAcquire=2;Check(!MovementEdit(failedEmpty,plan)&&failedEmpty.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==0,
            "failed first movement acquisition did not restore an empty field");
        Fixture late;auto manual=late.Get<SubtickPayload>(0x900+kMessagePayload);manual.bits|=7;manual.button=2|4|kAttack|32;manual.pressed=true;manual.when=.9f;
        late.Put(0x900+kMessagePayload,manual);Check(MovementEdit(late,plan),"late native event movement failed");ordered(late);
        const auto retained=late.Get<SubtickPayload>(0x900+kMessagePayload);
        Check(retained.button==(kAttack|32)&&(retained.bits&7)==7&&retained.pressed&&retained.when==.9f,
            "owned landing buttons were not removed from mixed manual event safely");
        Fixture bhop;manual.bits=7;manual.button=2|32;bhop.Put(0x900+kMessagePayload,manual);
        combat_command::Movement clearJump;clearJump.clearButtons=2;Check(MovementEdit(bhop,clearJump),"airborne bhop jump clear failed");
        Check(bhop.Get<SubtickPayload>(0x900+kMessagePayload).button==32,"native late jump superseded the airborne jump clear");
        Fixture repeated;Check(MovementEdit(repeated,plan),"cannot establish repeated movement window");const auto initial=repeated.Get<BaseCommand>(baseRaw+kMessagePayload);
        for(int pass=0;pass<32;++pass){auto replacement=plan;replacement.replaceSteps=repeated.committedSteps;replacement.replaceCount=2;
            replacement.analog=std::array{pass&1?.3f:.7f,-.2f};replacement.steps[0].forward=.25f;
            Check(MovementEdit(repeated,replacement),"same-command owned window could not be replaced");ordered(repeated);
            const auto base=repeated.Get<BaseCommand>(baseRaw+kMessagePayload);Check(base.subticks.size==4&&base.subticks.entries==initial.subticks.entries&&
                repeated.Get<int>(base.subticks.entries-repeated.At())==4,"repeated movement accumulated duplicate subticks or allocations");}
        const auto owned=repeated.committedSteps;auto shrink=plan;shrink.replaceSteps=owned;shrink.replaceCount=2;shrink.count=1;
        Check(MovementEdit(repeated,shrink)&&repeated.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==3,"owned window shrink failed");ordered(repeated);
        auto grow=plan;grow.replaceSteps=repeated.committedSteps;grow.replaceCount=1;
        Check(MovementEdit(repeated,grow)&&repeated.Get<BaseCommand>(baseRaw+kMessagePayload).subticks.size==4,"owned inactive slot could not grow back");ordered(repeated);
        auto replacement=plan;replacement.replaceSteps=repeated.committedSteps;replacement.replaceCount=2;
        Fixture successful;Check(MovementEdit(successful,plan),"repeat rollback control setup failed");replacement.replaceSteps=successful.committedSteps;
        successful.writes=0;Check(MovementEdit(successful,replacement),"repeat rollback control failed");const auto writes=successful.writes;
        for(unsigned index=1;index<=writes;++index){Fixture partial;Check(MovementEdit(partial,plan),"repeat partial setup failed");
            replacement.replaceSteps=partial.committedSteps;const auto prior=partial.bytes;partial.writes=0;partial.failWrite=index;
            Check(!MovementEdit(partial,replacement)&&partial.bytes==prior,"repeated movement partial write did not restore prior window/order");}
        Fixture crc;Check(MovementEdit(crc,plan),"repeat checksum setup failed");replacement.replaceSteps=crc.committedSteps;
        const auto before=crc.bytes;crc.crcCalls=0;crc.failCrc=true;
        Check(!MovementEdit(crc,replacement)&&crc.bytes==before&&crc.crcCalls==2,"repeat checksum failure changed prior window/order");
    }
    void RepeatedNativeCommand()
    {
        Fixture fixture;
        Check(Edit(fixture), "cannot commit a shot before native overwrite test");
        const auto committed = fixture.bytes;
        constexpr std::uint32_t localHandle = 0x12348002;
        const hvh_input::CommandStamp stamp{fixture.At(), 77, localHandle, true};
        hvh_input::SilentCommand pending;
        pending.Applied(stamp);
        unsigned originalCalls{};
        // The native overwrite branch keeps the command number and rewrites its
        // base angle from camera input (B66190 -> B85A80 on the audited client).
        const auto createMove = [&](const hvh_input::CommandStamp& current)
        {
            if (pending.Preserve(current)) return;
            ++originalCalls;
            auto angle = fixture.Get<AnglePayload>(angleRaw + kMessagePayload);
            angle.pitch = 4; angle.yaw = 0; angle.roll = 0;
            fixture.Put(angleRaw + kMessagePayload, angle);
        };
        createMove(stamp);
        createMove(stamp);
        Check(originalCalls == 0 && fixture.bytes == committed,
              "repeated native overwrite erased a committed silent shot");

        const auto mustRunOriginal = [&](hvh_input::CommandStamp current)
        {
            fixture.bytes = committed;
            const auto beforeCalls = originalCalls;
            createMove(current);
            Check(originalCalls == beforeCalls + 1 &&
                  fixture.Get<AnglePayload>(angleRaw + kMessagePayload).yaw == 0,
                  "preservation suppressed an unrelated native command");
        };
        auto other = stamp;
        other.subtickOverwrite = false; mustRunOriginal(other);
        other = stamp; other.sequence = 78; mustRunOriginal(other);
        other = stamp; other.sequence += 150; mustRunOriginal(other); // Same ring slot, new sequence.
        other = stamp; other.command += 0x98; mustRunOriginal(other);
        other = stamp; other.localHandle += 0x8000; mustRunOriginal(other); // New entity serial.
        other = stamp; other.localHandle = 0xffffffffu; mustRunOriginal(other);
        pending.Clear(); mustRunOriginal(stamp);
        pending.Applied({stamp.command, 0, stamp.localHandle, true}); mustRunOriginal(stamp);
        pending.Applied({stamp.command, stamp.sequence, 0xffffffffu, true}); mustRunOriginal(stamp);
    }
}

void BacktrackTransactions() {
    const std::array<float,3> aim{5,90,0};
    const auto prepare = [](Fixture& f) {
        active=&f;
        for (auto raw : historyRaw) {
            auto entry=f.Get<FullHistory>(raw+kMessagePayload); entry.bits|=0xe;
            f.Put(raw+kMessagePayload,entry);
        }
        struct Client { std::uint32_t bits,size; float fraction; };
        f.Put(0xb00+kMessagePayload,Client{1,12,.6f});
        f.Put(0xb40+kMessagePayload,Interpolation{7,20,.6f,60,61});
        f.Put(0xb80+kMessagePayload,Interpolation{7,20,.4f,61,62});
    };
    combat_command::Shot shot{aim,72,.3f,BacktrackTiming{{68,0},100}};
    Fixture success;prepare(success);const auto before=success.bytes;
    Check(combat_command::Edit(success.At(),success.At(baseRaw+kMessagePayload),77,aim,true,io,shot),"Historical shot rejected");
    for(auto raw:historyRaw){
        const auto entry=success.Get<FullHistory>(raw+kMessagePayload);
        Check(entry.renderTick==68&&entry.renderFraction==0&&entry.targetIndex==100,"Historical render timestamp missing");
        Check(entry.playerTick==72&&Near(entry.playerFraction,.3f),"Backtrack changed player firing/seed time");
        Check((entry.bits&0x4600)==0x4600,"Historical protobuf presence bits missing");
        Check(entry.playerInterpolation==success.At(0xbc0)&&entry.shootPosition==success.At(0xc00),"Backtrack changed source-eye timing");
    }
    Check(success.Get<Interpolation>(0xb40+kMessagePayload).source==-1&&success.Get<Interpolation>(0xb80+kMessagePayload).destination==-1,"Stale target interpolation retained");
    Check(std::memcmp(before.data()+0xbc0,success.bytes.data()+0xbc0,0x80)==0,"Player interpolation or eye message was modified");
    const auto writes=success.writes;
    const auto historical=success.bytes;
    auto movement=MovementPlan();movement.zeroBaseAnalog=true;
    Check(MovementEdit(success,movement),"Movement following historical shot failed");
    active=&success;
    Check(combat_command::Edit(success.At(),success.At(baseRaw+kMessagePayload),77,std::array{30.f,160.f,0.f},
        false,io,{},combat_command::AngleMode::BaseOnly),"Anti aim following historical shot and movement failed");
    Check(success.Get<BaseCommand>(baseRaw+kMessagePayload).forward==0&&success.Get<BaseCommand>(baseRaw+kMessagePayload).left==0,
        "Orientation restored base movement after an explicit subtick window");
    Check(std::equal(historical.begin()+historyRaw[0],historical.begin()+historyRep,success.bytes.begin()+historyRaw[0])&&
        std::equal(historical.begin()+cameraOffset,historical.begin()+0xd00,success.bytes.begin()+cameraOffset),
        "Movement/anti aim changed historical shot angles, timing, interpolation or camera");
    for(int invalid=0;invalid<3;++invalid){Fixture rejected;auto invalidPlan=movement;
        if(invalid==0)invalidPlan.analog.reset();if(invalid==1)invalidPlan.replaceAnalog=false;if(invalid==2)invalidPlan.count=0;
        Check(!MovementEdit(rejected,invalidPlan)&&!rejected.subtickAcquires&&!rejected.writes,"Invalid base-analog clear acquired native storage");}
    for(unsigned fail=1;fail<=writes;++fail){
        Fixture f;prepare(f);const auto original=f.bytes;f.failWrite=fail;
        Check(!combat_command::Edit(f.At(),f.At(baseRaw+kMessagePayload),77,aim,true,io,shot)&&f.bytes==original,"Historical command partial-write rollback failed");
    }
    Fixture crc;prepare(crc);const auto original=crc.bytes;crc.failCrc=true;
    Check(!combat_command::Edit(crc.At(),crc.At(baseRaw+kMessagePayload),77,aim,true,io,shot)&&crc.bytes==original,"Historical CRC rollback failed");
    for(const auto bad:std::array<BacktrackTiming,4>{{{{74,0},100},{{0,0},100},{{68,1},100},{{68,0},0}}}){
        Fixture f;prepare(f);shot.backtrack=bad;
        Check(!combat_command::Edit(f.At(),f.At(baseRaw+kMessagePayload),77,aim,true,io,shot)&&!f.writes,"Invalid historical timing wrote a command");
    }
}

int main()
{
    try
    {
        ValidShot();
        SecondaryScopeTransactions();
        BacktrackTransactions();
        InvalidShots();
        RollbackAndSharedAngles();
        BaseOnlyOrientation();
        CommittedShotThenOrientation();
        MovementTransactions();ManualJumpDuringAnalogMovement();MovementCallbackFreshness();
        EmptyChronologicalAndRepeatedMovement();
        RepeatedNativeCommand();
        std::cout << "HVH command regression: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "HVH command regression: FAIL: " << error.what() << '\n';
        return 1;
    }
}
