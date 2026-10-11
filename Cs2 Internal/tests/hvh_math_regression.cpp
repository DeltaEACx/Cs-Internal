#include "features/combat/detail/hvh_math.hpp"
#include "features/combat/detail/hvh_accuracy.hpp"
#include "features/combat/detail/hvh_antiaim.hpp"
#include "features/combat/detail/hvh_input.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    void Check(bool value, const char* message);
    void ActivationModes()
    {
        using namespace hvh_input;
        Check(Activated(true, true, ActivationMode::Always, false), "Always mode still requires a key");
        Check(!Activated(true, true, ActivationMode::Hold, false) && Activated(true, true, ActivationMode::Hold, true),
              "Hold activation ignored the key state");
        for (const auto mode : {ActivationMode::Hold, ActivationMode::Always})
            for (const bool key : {false, true}) {
                Check(!Activated(false, true, mode, key), "Activation ignored master disable");
                Check(!Activated(true, false, mode, key), "Activation bypassed menu/focus/gameplay pause");
            }
        Check(!Activated(true, true, static_cast<ActivationMode>(99), true), "Invalid activation mode fired");
    }
    using hvh_math::Vec3;
    using hvh_math::Vec2;
    using hvh_math::Candidate;
    void Check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    double Difference(Vec3 a, Vec3 b)
    {
        return std::hypot(double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z);
    }
    Candidate Player(Vec3 point, float damage = 25, std::uint32_t handle = 0x10002)
    {
        Candidate result;
        result.pawn = 0x20000 + (handle & 0x7fff) * 0x100;
        result.handle = handle; result.team = 3; result.health = 100;
        result.valid = true; result.hittable = true; result.point = point; result.damage = damage;
        result.hitbox = 0;
        return result;
    }
    hvh_math::View View()
    {
        hvh_math::View result;
        result.localPawn = 0x10000; result.localHandle = 0x10001; result.localTeam = 2;
        result.range = 1000;
        return result;
    }

    void TargetSelection()
    {
        auto view = View();
        // The low-damage, nonlethal target is closer to the crosshair. Damage
        // must not displace it in favor of a larger angular correction.
        std::array players{Player({100, 2, 0}, 5), Player({100, 10, 0}, 150, 0x10003)};
        auto best = hvh_math::Select(view, players);
        Check(best && best->index == 0 && best->damage == 5, "damage replaced closest crosshair target");
        Check(std::abs(best->angles.y - 1.1457628f) < 1e-5 && best->angles.x == 0 && best->angles.z == 0,
              "target point angle is incorrect");
        players[0].hittable = false;
        best = hvh_math::Select(view, players);
        Check(best && best->index == 1, "blocked closest target hides reachable enemy");
        auto geometry = hvh_math::ProjectCandidate(view, players[0], 42, false);
        Check(geometry && geometry->index == 42 && geometry->score < best->score,
              "geometry sorting requires native hit result");
        players[0].damage = std::numeric_limits<float>::quiet_NaN();
        geometry = hvh_math::ProjectCandidate(view, players[0], 0, false);
        Check(geometry && geometry->damage == 0, "unknown damage corrupts geometry comparator");
        players[0] = Player({100, 0, 0}, 25);
        players[1] = Player({100, 0, 0}, 30, 0x10003);
        Check(hvh_math::Select(view, players)->index == 1, "exact crosshair tie did not prefer damage");
        players[0].damage = 30;
        Check(hvh_math::Select(view, players)->index == 0, "equal candidates are not deterministic");
        players[0].point = {50, 0, 0};
        Check(hvh_math::Select(view, players)->index == 0, "equal angular targets did not prefer nearer point");
        view.minDamage = 31;
        Check(!hvh_math::Select(view, players), "minimum damage is not enforced");
        view.minDamage = 1;
        view.maxFov = 2;
        players[0] = Player({100, 4, 0});
        players[1] = Player({100, 8, 0}, 50, 0x10003);
        Check(!hvh_math::Select(view, players), "angular FOV limit is not enforced");
        view = View();
        view.angles.y = 179;
        players[0] = Player({-100, -1, 0});
        players[1] = Player({-100, -10, 0}, 50, 0x10003);
        best = hvh_math::Select(view, players);
        Check(best && best->index == 0 && best->angularDistance < 2, "yaw seam selected distant target");
        view = View(); view.range = 100;
        players[0] = Player({100, 0, 0}); players[1] = Player({101, 0, 0}, 100, 0x10003);
        Check(hvh_math::Select(view, players)->index == 0, "range endpoint rejected");
        players[0].point.x = 100.01f;
        Check(!hvh_math::Select(view, players), "target beyond weapon range accepted");
    }

    void IdentityAndInputs()
    {
        const auto view = View();
        const auto good = Player({100, 0, 0});
        auto reject = [&](Candidate player)
        {
            Check(!hvh_math::ProjectCandidate(view, player, 0), "ineligible player accepted");
            Check(!hvh_math::ProjectCandidate(view, player, 0, false), "geometry sorting bypassed eligibility");
        };
        auto bad = good; bad.health = 0; reject(bad);
        bad = good; bad.health = -1; reject(bad);
        bad = good; bad.team = 2; reject(bad);
        bad = good; bad.team = 1; reject(bad);
        bad = good; bad.pawn = 0; reject(bad);
        bad = good; bad.pawn = view.localPawn; reject(bad);
        bad = good; bad.handle = view.localHandle; reject(bad);
        bad = good; bad.handle = 0xffffffff; reject(bad);
        bad = good; bad.handle = 0x10000; reject(bad);
        bad = good; bad.handle = 0x17fff; reject(bad);
        bad = good; bad.valid = false; reject(bad);
        bad = good; bad.dormant = true; reject(bad);
        bad = good; bad.immune = true; reject(bad);
        bad = good; bad.point = {0, 0, 0}; reject(bad);
        bad = good; bad.point = {-100, 0, 0}; reject(bad);
        bad = good; bad.point.x = std::numeric_limits<float>::quiet_NaN(); reject(bad);
        bad = good; bad.point.z = std::numeric_limits<float>::infinity(); reject(bad);
        for (float damage : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        {
            bad = good; bad.damage = damage;
            Check(!hvh_math::ProjectCandidate(view, bad, 0), "invalid hit damage accepted");
            Check(bool(hvh_math::ProjectCandidate(view, bad, 0, false)), "pretrace ranking depends on damage");
        }
        for (int condition = 0; condition < 11; ++condition)
        {
            auto invalid = view;
            switch (condition)
            {
            case 0: invalid.localPawn = 0; break;
            case 1: invalid.localHandle = 0xffffffff; break;
            case 2: invalid.localTeam = 0; break;
            case 3: invalid.eye.x = std::numeric_limits<float>::quiet_NaN(); break;
            case 4: invalid.angles.y = std::numeric_limits<float>::infinity(); break;
            case 5: invalid.range = 0; break;
            case 6: invalid.range = 32769; break;
            case 7: invalid.range = std::numeric_limits<float>::quiet_NaN(); break;
            case 8: invalid.maxFov = -1; break;
            case 9: invalid.maxFov = 181; break;
            case 10: invalid.minDamage = -1; break;
            }
            Check(!hvh_math::ProjectCandidate(invalid, good, 0), "invalid local/view state accepted");
        }
        Check(!hvh_math::Select(view, {}), "empty candidate list selected target");
    }

    void ScreenRanking()
    {
        // Camera looks along world X. Horizontal screen X is world Y, vertical
        // screen Y is world Z, and perspective w is world X.
        float matrix[4][4] = {{0, 1, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
        auto view = View(); view.matrix = matrix; view.width = 2000; view.height = 500;
        std::array players{Player({100, 10, 0}), Player({100, 0, 20}, 50, 0x10003)};
        // Angularly, player 0 is closer; screen pixels on this wide viewport
        // put player 1 closer (50px versus 100px).
        Check(hvh_math::Select(view, players)->index == 1, "matrix ranking ignores viewport aspect");
        view.matrix = nullptr;
        Check(hvh_math::Select(view, players)->index == 0, "angular fallback is not spherical angle");
        view.matrix = matrix;
        const auto central = hvh_math::ProjectCandidate(view, Player({100, 0, 0}), 7);
        Check(central && central->score == 0 && central->index == 7, "crosshair center was rejected");
        view.angles.y = 180;
        Check(bool(hvh_math::ProjectCandidate(view, Player({100, 0, 0}), 0)),
              "input angles overrode a visible camera point");
        Check(!hvh_math::ProjectCandidate(view, Player({-100, 0, 0}), 0),
              "point behind the camera passed the matrix visibility check");
        view.angles.y = 0; matrix[3][3] = 200;
        Check(bool(hvh_math::ProjectCandidate(view, Player({-100, 0, 0}), 0)),
              "visible point between third-person camera and player eye was rejected");
        matrix[3][3] = 0;
        Check(!hvh_math::ProjectCandidate(view, Player({100, 101, 0}), 0), "offscreen horizontal target accepted");
        Check(!hvh_math::ProjectCandidate(view, Player({100, 0, -101}), 0), "offscreen vertical target accepted");
        Check(bool(hvh_math::ProjectCandidate(view, Player({100, 100, 0}), 0)), "viewport edge target rejected");
        Check(!hvh_math::ProjectCandidate(view, Player({.009f, 0, 0}), 0), "point before near plane accepted");
        Check(bool(hvh_math::ProjectCandidate(view, Player({.01f, 0, 0}), 0)), "exact near plane rejected");
        view.width = 0;
        Check(!hvh_math::ProjectCandidate(view, players[0], 0), "invalid viewport accepted");
        view.width = 2000; matrix[2][2] = std::numeric_limits<float>::quiet_NaN();
        Check(!hvh_math::ProjectCandidate(view, players[0], 0), "invalid unused matrix row accepted");
        matrix[2][2] = 0;
        std::vector<hvh_math::Selection> sorted;
        for (std::size_t i = 0; i < players.size(); ++i)
        {
            players[i].hittable = false; players[i].damage = 0;
            if (auto selection = hvh_math::ProjectCandidate(view, players[i], i, false)) sorted.push_back(*selection);
        }
        std::sort(sorted.begin(), sorted.end(), hvh_math::Closer);
        Check(sorted.size() == 2 && sorted.front().index == 1, "pretrace candidates cannot sort by crosshair distance");
    }

    Vec3 IndependentPellet(Vec3 angle, Vec2 spread)
    {
        // Independently rotate a local pellet by Source's Euler basis. Local
        // Y points right, while spread.x is added along Source's left vector.
        const double p = double(angle.x) * std::numbers::pi / 180.;
        const double y = double(angle.y) * std::numbers::pi / 180.;
        const double r = double(angle.z) * std::numbers::pi / 180.;
        double x = 1, right = -spread.x, z = spread.y;
        const double rolledRight = right * std::cos(r) - z * std::sin(r);
        z = right * std::sin(r) + z * std::cos(r);
        right = rolledRight;
        const double pitchedX = x * std::cos(p) + z * std::sin(p);
        z = -x * std::sin(p) + z * std::cos(p);
        x = pitchedX;
        const double worldX = x * std::cos(y) - right * std::sin(y);
        const double worldY = x * std::sin(y) + right * std::cos(y);
        const double length = std::hypot(worldX, worldY, z);
        return {float(worldX / length), float(worldY / length), float(z / length)};
    }

    void SpreadGeometry()
    {
        for (Vec3 aim : {Vec3{0, 0, 0}, Vec3{-45, 179, 0}, Vec3{35, -170, 0}, Vec3{-80, 350, 0}})
            for (Vec2 spread : {Vec2{}, Vec2{.03f, 0}, Vec2{-.03f, 0}, Vec2{0, .02f}, Vec2{0, -.02f},
                                Vec2{.02f, -.04f}, Vec2{-.015f, .05f}})
            {
                const auto correction = hvh_math::CorrectSpread(aim, spread);
                Check(bool(correction), "finite spread correction failed");
                const auto expected = IndependentPellet(aim, {});
                Check(Difference(IndependentPellet(*correction, spread), expected) < 3e-7,
                      "corrected pellet does not reach desired direction");
                const auto actual = hvh_math::ShotDirection(*correction, spread);
                Check(actual && Difference(*actual, IndependentPellet(*correction, spread)) < 1e-7,
                      "Source shot direction disagrees with independent rotation");
                if (spread.x != 0)
                {
                    auto discardedRoll = *correction; discardedRoll.z = 0;
                    Check(Difference(IndependentPellet(discardedRoll, spread), expected) > .001,
                          "fixture does not detect lost command roll");
                }
            }
        const auto zero = hvh_math::CorrectSpread({}, {});
        Check(zero && zero->x == 0 && zero->y == 0 && zero->z == 0, "zero aim is treated as failure sentinel");
        Check(!hvh_math::CorrectSpread({88, 0, 0}, {.1f, 0}), "correction requiring pitch clamp accepted");
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        Check(!hvh_math::CorrectSpread({nan, 0, 0}, {}), "NaN aim accepted");
        Check(!hvh_math::CorrectSpread({0, inf, 0}, {}), "infinite aim accepted");
        Check(!hvh_math::CorrectSpread({90, 0, 0}, {}), "unrepresentable aim pitch accepted");
        Check(!hvh_math::CorrectSpread({}, {nan, 0}) && !hvh_math::CorrectSpread({}, {0, inf}), "invalid spread accepted");
        Check(!hvh_math::ShotDirection({0, 0, nan}, {}), "invalid roll accepted");
    }

    void SpreadSeedSearch()
    {
        int trialCalls = 0, sampleCalls = 0, correctedCalls = 0;
        std::vector<float> trialPitches;
        const auto seed = [&](Vec3 angles, int tick) -> std::optional<std::uint32_t>
        {
            Check(tick == 77, "seed callback received wrong shot tick");
            if (angles.z == 0)
            {
                ++trialCalls; trialPitches.push_back(angles.x);
                return std::uint32_t(trialCalls);
            }
            ++correctedCalls;
            return 3; // Only the third trial is a self-consistent angle/seed pair.
        };
        const auto sample = [&](std::uint32_t value) -> std::optional<Vec2>
        {
            ++sampleCalls;
            Check(value == std::uint32_t(sampleCalls), "sample seed was substituted or incremented in math helper");
            return Vec2{.02f, .01f};
        };
        const auto correction = hvh_math::FindSpreadCorrection({10, 20, 0}, 77, seed, sample);
        Check(correction && trialCalls == 3 && correctedCalls == 3 && sampleCalls == 3 &&
              trialPitches == std::vector<float>{0, .5f, 1}, "search does not verify corrected native seed");

        int calls = 0;
        auto mismatch = [&](Vec3 angles, int) -> std::optional<std::uint32_t>
        {
            ++calls;
            return angles.z == 0 ? 1u : 2u;
        };
        auto constantSpread = [](std::uint32_t) -> std::optional<Vec2> { return Vec2{.02f, .01f}; };
        Check(!hvh_math::FindSpreadCorrection({}, 1, mismatch, constantSpread, 5) && calls == 10,
              "mismatched corrected seed accepted or search unbounded");
        auto zeroSeed = [](Vec3, int) -> std::optional<std::uint32_t> { return 0u; };
        auto zeroSpread = [](std::uint32_t) -> std::optional<Vec2> { return Vec2{}; };
        const auto zero = hvh_math::FindSpreadCorrection({}, 0, zeroSeed, zeroSpread);
        Check(zero && zero->x == 0 && zero->y == 0 && zero->z == 0, "valid zero seed/angle search failed");
        auto unavailableSeed = [](Vec3, int) -> std::optional<std::uint32_t> { return {}; };
        auto unavailableSpread = [](std::uint32_t) -> std::optional<Vec2> { return {}; };
        Check(!hvh_math::FindSpreadCorrection({}, 1, unavailableSeed, constantSpread, 2), "missing seed accepted");
        Check(!hvh_math::FindSpreadCorrection({}, 1, zeroSeed, unavailableSpread, 2), "missing sample accepted");
        auto nanSpread = [](std::uint32_t) -> std::optional<Vec2>
        {
            return Vec2{std::numeric_limits<float>::quiet_NaN(), 0};
        };
        Check(!hvh_math::FindSpreadCorrection({}, 1, zeroSeed, nanSpread, 2), "nonfinite native sample accepted");
        for (int attempts : {0, -1, 721})
            Check(!hvh_math::FindSpreadCorrection({}, 1, zeroSeed, zeroSpread, attempts), "invalid search bound accepted");
        Check(!hvh_math::FindSpreadCorrection({}, -1, zeroSeed, zeroSpread), "negative shot tick accepted");
    }

    void IterativeSpreadSeedSearch()
    {
        using hvh_math::FindSpreadCorrectionIterative;
        unsigned seedCalls{}, sampleCalls{};
        constexpr Vec3 desired{0,20,0};
        const auto exactSeed=[&](Vec3 angle,int tick)->std::optional<std::uint32_t> {
            ++seedCalls;
            Check(tick==42&&angle.y==20,"Iterative correction changed native tick or desired yaw");
            return 0u;
        };
        const auto exactSample=[&](std::uint32_t value)->std::optional<Vec2> {
            ++sampleCalls;Check(value==0,"Iterative correction substituted zero native seed");
            return Vec2{.02f,.01f};
        };
        const auto exact=FindSpreadCorrectionIterative(desired,42,exactSeed,exactSample);
        Check(exact&&seedCalls==2&&sampleCalls==1&&
              Difference(IndependentPellet(*exact,{.02f,.01f}),IndependentPellet(desired,{}))<3e-7,
              "A verified fixed point did not produce the independent desired physical ray");

        const auto correctionPitch=[](float spread) {
            return static_cast<float>(std::atan(double(spread))*180./std::numbers::pi);
        };
        seedCalls=0;sampleCalls=0;
        std::vector<std::uint32_t> sampled;
        const auto convergentSeed=[&](Vec3 angle,int)->std::optional<std::uint32_t> {
            ++seedCalls;
            if(angle.x==0)return 1u;
            if(std::abs(angle.x-correctionPitch(.01f))<1e-5)return 2u;
            if(std::abs(angle.x-correctionPitch(.02f))<1e-5)return 3u;
            Check(std::abs(angle.x-correctionPitch(.03f))<1e-5,"Unexpected iterative angle fed native seed");
            return 3u;
        };
        const auto convergentSample=[&](std::uint32_t value)->std::optional<Vec2> {
            ++sampleCalls;sampled.push_back(value);return Vec2{0.f,value*.01f};
        };
        const auto converged=FindSpreadCorrectionIterative(desired,42,convergentSeed,convergentSample);
        Check(converged&&seedCalls==4&&sampleCalls==3&&sampled==std::vector<std::uint32_t>{1,2,3}&&
              convergentSeed(*converged,42)==3u&&
              Difference(IndependentPellet(*converged,{0,.03f}),IndependentPellet(desired,{}))<3e-7,
              "Iterative correction returned an unverified angle or failed multi-step convergence");

        seedCalls=0;sampleCalls=0;
        const auto cycleSeed=[&](Vec3 angle,int)->std::optional<std::uint32_t> {
            ++seedCalls;
            return angle.x==0||std::abs(angle.x-correctionPitch(.02f))<1e-5 ? 1u : 2u;
        };
        const auto cycleSample=[&](std::uint32_t value)->std::optional<Vec2> {
            ++sampleCalls;return Vec2{0.f,value*.01f};
        };
        Check(!FindSpreadCorrectionIterative(desired,42,cycleSeed,cycleSample)&&seedCalls==3&&sampleCalls==2,
              "Iterative correction repeated a seed cycle or returned a mismatched physical angle");

        seedCalls=0;sampleCalls=0;
        const auto noSolutionSeed=[&](Vec3 angle,int)->std::optional<std::uint32_t> {
            ++seedCalls;return static_cast<std::uint32_t>(std::lround(angle.x/.125f))+1u;
        };
        const auto noSolutionSample=[&](std::uint32_t value)->std::optional<Vec2> {
            ++sampleCalls;return Vec2{0.f,static_cast<float>(std::tan(value*.125*std::numbers::pi/180.))};
        };
        Check(!FindSpreadCorrectionIterative(desired,42,noSolutionSeed,noSolutionSample)&&seedCalls==9&&sampleCalls==8,
              "Default iterative correction exceeded its eight-sample bound or invented a solution");
        seedCalls=0;sampleCalls=0;
        Check(!FindSpreadCorrectionIterative(desired,42,noSolutionSeed,noSolutionSample,32)&&seedCalls==33&&sampleCalls==32,
              "Maximum iterative correction exceeded its native callback bound");

        for(unsigned unavailableAt:{1u,2u}) {
            seedCalls=0;sampleCalls=0;
            const auto unavailableSeed=[&](Vec3,int)->std::optional<std::uint32_t> {
                ++seedCalls;return seedCalls==unavailableAt ? std::nullopt : std::optional{1u};
            };
            const auto sample=[&](std::uint32_t)->std::optional<Vec2>{++sampleCalls;return Vec2{0,.01f};};
            Check(!FindSpreadCorrectionIterative(desired,42,unavailableSeed,sample)&&
                  seedCalls==unavailableAt&&sampleCalls==unavailableAt-1,
                  "Missing initial or verification seed claimed an iterative correction");
        }
        for(const auto badSpread:{std::optional<Vec2>{},std::optional{Vec2{NAN,0}},std::optional{Vec2{0,INFINITY}}}) {
            seedCalls=0;sampleCalls=0;
            const auto sample=[&](std::uint32_t)->std::optional<Vec2>{++sampleCalls;return badSpread;};
            Check(!FindSpreadCorrectionIterative(desired,42,exactSeed,sample)&&seedCalls==1&&sampleCalls==1,
                  "Unavailable or malformed native spread claimed iterative correction");
        }
        const auto zeroSeed=[](Vec3,int)->std::optional<std::uint32_t>{return 0u;};
        const auto zeroSample=[](std::uint32_t)->std::optional<Vec2>{return Vec2{};};
        const auto zero=FindSpreadCorrectionIterative({},0,zeroSeed,zeroSample);
        Check(zero&&Difference(*zero,{})==0,"Valid zero iterative seed and angle were rejected");
        seedCalls=0;sampleCalls=0;
        for(int attempts:{0,-1,33})
            Check(!FindSpreadCorrectionIterative(desired,42,exactSeed,exactSample,attempts),"Invalid iterative bound accepted");
        for(Vec3 aim:{Vec3{NAN,0,0},Vec3{0,INFINITY,0},Vec3{0,0,NAN},Vec3{90,0,0}})
            Check(!FindSpreadCorrectionIterative(aim,42,exactSeed,exactSample),"Malformed iterative aim accepted");
        Check(!FindSpreadCorrectionIterative(desired,-1,exactSeed,exactSample)&&seedCalls==0&&sampleCalls==0,
              "Invalid iterative input reached native callbacks");
        const auto excessive=[](std::uint32_t)->std::optional<Vec2>{return Vec2{.1f,0};};
        Check(!FindSpreadCorrectionIterative({88,20,0},42,zeroSeed,excessive),
              "Iterative correction clamped an unrepresentable physical angle");
    }

    void ShotPrediction()
    {
        const Vec3 angle{12,179,37};
        const Vec2 offset{.04f,-.025f};
        unsigned hashes{},samples{};
        const auto seed=[&](Vec3 supplied,int tick)->std::optional<std::uint32_t>{
            ++hashes;
            Check(Difference(supplied,angle)==0 && tick==1200,"Predictor changed the physical angles or shot tick");
            return 0;
        };
        const auto sample=[&](std::uint32_t number)->std::optional<Vec2>{
            ++samples; Check(number==0,"Predictor discarded the valid zero seed"); return offset;
        };
        const auto prediction=hvh_math::PredictShot(angle,1200,seed,sample);
        Check(prediction && prediction->seed==0 && prediction->tick==1200 && hashes==1 && samples==1,
              "A known shot was not predicted with one native hash and sample");
        Check(Difference(prediction->direction,IndependentPellet(angle,offset))<1e-7,
              "Predicted direction disagrees with an independent pellet rotation");
        const auto missing=[](Vec3,int)->std::optional<std::uint32_t>{return {};};
        Check(!hvh_math::PredictShot(angle,1200,missing,sample) && samples==1,
              "Missing seed was replaced by zero or reached the sampler");
        const auto zero=[](Vec3,int)->std::optional<std::uint32_t>{return 0;};
        const auto malformed=[](std::uint32_t)->std::optional<Vec2>{return Vec2{NAN,0};};
        Check(!hvh_math::PredictShot(angle,1200,zero,malformed),"Malformed sample became an exact predicted ray");
        Check(!hvh_math::PredictShot({NAN,0,0},1200,seed,sample) &&
              !hvh_math::PredictShot(angle,-1,seed,sample) && hashes==1,
              "Invalid prediction inputs reached native callbacks");
        const auto corrected=hvh_math::FindSpreadPredictionIterative({10,20,0},1200,zero,
            [&](std::uint32_t)->std::optional<Vec2>{return offset;});
        Check(corrected && Difference(corrected->direction,IndependentPellet({10,20,0},{}))<3e-7,
              "Compensation did not return its verified final predicted ray");
    }

    void AntiAimAngles()
    {
        using namespace hvh_antiaim;
        Options options;
        const Vec3 camera{12,45,7};
        const auto sameAngle=[](float a,float b) {return std::abs(std::remainder(double(a)-b,360.))<1e-5;};
        const auto first=Angles(camera,0,options);
        Check(first&&first->x==89&&first->y==45&&first->z==0,"Spin default orientation or pitch is incorrect");
        const auto quarter=Angles(camera,16,options), full=Angles(camera,64,options);
        Check(quarter&&sameAngle(quarter->y,135)&&full&&sameAngle(full->y,45),"Spin does not use native tick time");
        Check(camera.x==12&&camera.y==45&&camera.z==7,"Anti-aim changed the visible camera input");
        // Repeated/render-sized invocations and skipped callback ticks cannot
        // accelerate or reset a phase anchored in the command's native tick.
        for (int repeat=0;repeat<500;++repeat) {
            const auto value=Angles(camera,16,options);
            Check(value&&Difference(*value,*quarter)==0,"Repeated processing advanced the spin phase");
        }
        Check(sameAngle(Angles(camera,48,options)->y,-45),"Skipped callback ticks changed native-time spin speed");
        const auto twiceRate=Angles(camera,32,options,1.f/128.f);
        Check(twiceRate&&Difference(*twiceRate,*quarter)==0,"Spin speed depends on native tick rate instead of elapsed time");
        options.spinSpeed=720;
        Check(sameAngle(Angles(camera,8,options)->y,135),"Configurable spin speed is ignored");
        options.spinSpeed=30;
        Check(sameAngle(Angles(camera,64,options)->y,75),"Slow spin endpoint is incorrect");
        options.spinSpeed=1440;options.yawOffset=-180;
        Check(sameAngle(Angles(camera,8,options)->y,45),"Fast spin/offset endpoint is incorrect");
        options.yaw=YawMode::Backward;options.yawOffset=0;options.pitch=PitchMode::Keep;
        const auto backward=Angles(camera,0,options,1.f/64.f,45.f), lateBackward=Angles(camera,INT32_MAX,options,1.f/64.f,45.f);
        Check(backward&&backward->x==12&&sameAngle(backward->y,-135)&&lateBackward&&Difference(*backward,*lateBackward)==0,
              "Backward orientation depends on time or does not keep pitch");
        options.pitch=PitchMode::Up;options.yawOffset=180;
        Check(Angles(camera,0,options,1.f/64.f,45.f)->x==-89&&sameAngle(Angles(camera,0,options,1.f/64.f,45.f)->y,45),"Up pitch or backward offset is incorrect");
        options.pitch=PitchMode::Keep;
        Check(Angles({120,179,0},0,options,1.f/64.f,179.f)->x==89&&Angles({-120,-179,0},0,options,1.f/64.f,-179.f)->x==-89,
              "Keep pitch escaped the native usable range");
        options.yaw=YawMode::Spin;options.spinSpeed=360;options.yawOffset=0;
        for (const int tick : {0,1,63,64,65536,INT32_MAX})
            for (const float yaw : {-720.f,-180.f,0.f,180.f,720.f,std::numeric_limits<float>::max()}) {
                const auto angle=Angles({0,yaw,0},tick,options);
                Check(angle&&std::isfinite(angle->y)&&std::abs(angle->y)<=180&&std::abs(angle->x)<=89&&angle->z==0,
                      "Large tick/view yaw escaped bounded finite anti-aim angles");
            }
        auto invalid=options;
        invalid.yaw=static_cast<YawMode>(2);Check(!Angles(camera,1,invalid),"Unknown yaw mode accepted");
        invalid=options;invalid.pitch=static_cast<PitchMode>(3);Check(!Angles(camera,1,invalid),"Unknown pitch mode accepted");
        for (const float value : {29.f,1441.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
            invalid=options;invalid.spinSpeed=value;Check(!Angles(camera,1,invalid),"Invalid spin speed accepted");
        }
        for (const float value : {-181.f,181.f,std::numeric_limits<float>::quiet_NaN()}) {
            invalid=options;invalid.yawOffset=value;Check(!Angles(camera,1,invalid),"Invalid yaw offset accepted");
        }
        for (const float interval : {0.f,-1.f,1.001f,std::numeric_limits<float>::quiet_NaN()})
            Check(!Angles(camera,1,options,interval),"Invalid native tick interval accepted");
        Check(!Angles(camera,-1,options),"Negative native command tick accepted");
        Check(!Angles({std::numeric_limits<float>::quiet_NaN(),0,0},1,options)&&
              !Angles({0,std::numeric_limits<float>::infinity(),0},1,options)&&
              !Angles({0,0,std::numeric_limits<float>::quiet_NaN()},1,options),"Nonfinite visible-angle input accepted");
    }

    void TargetRelativeBackward()
    {
        using namespace hvh_antiaim;
        auto view=View();view.eye={100,200,300};view.angles={12,0,0};
        std::array candidates{Player({200,200,300}),Player({100,300,300},25,0x10003)};
        auto selected=SelectThreat(view,candidates);
        Check(selected&&selected->index==0&&selected->yaw==0,"Backward reference ignored closest crosshair enemy");
        selected=SelectThreat(view,candidates,candidates[1].pawn,candidates[1].handle);
        Check(selected&&selected->index==1&&selected->yaw==90,"HVH shot target did not take orientation priority");
        selected=SelectThreat(view,candidates,candidates[1].pawn,candidates[1].handle+0x8000);
        Check(selected&&selected->index==0,"Recycled preferred handle retained orientation priority");
        candidates[0].hittable=false;candidates[0].damage=0;
        Check(SelectThreat(view,candidates)->index==0,"Body orientation unnecessarily required penetration damage");
        view.angles.y=90;Check(SelectThreat(view,candidates)->index==1,"Reference did not follow the visible crosshair");
        Options options;options.yaw=YawMode::Backward;options.pitch=PitchMode::Keep;
        const auto same=[](float a,float b){return std::abs(std::remainder(double(a)-b,360.))<1e-5;};
        for(const auto point:std::array<Vec3,4>{{{200,200,300},{100,300,300},{0,200,300},{100,100,300}}}){
            const auto bearing=Bearing(view.eye,point);Check(bearing.has_value(),"Cardinal enemy bearing was rejected");
            const auto angle=Angles({12,-137,0},77,options,1.f/64.f,bearing);
            Check(angle&&same(angle->y,*bearing+180)&&angle->x==12,"Backward yaw did not point away from enemy position");
            Check(same(angle->y,Angles({12,80,0},INT32_MAX,options,1.f/64.f,bearing)->y),"Camera yaw/tick rotated target-relative backwards yaw");
        }
        candidates[0].point={0,200,300};candidates[1].valid=false;view.angles.y=0;
        Check(SelectThreat(view,candidates)&&same(SelectThreat(view,candidates)->yaw,180),"Enemy behind the camera cannot provide backwards orientation");
        for(int failure=0;failure<8;++failure){auto bad=candidates;
            switch(failure){case 0:bad[0].team=view.localTeam;break;case 1:bad[0].health=0;break;
                case 2:bad[0].dormant=true;break;case 3:bad[0].immune=true;break;case 4:bad[0].handle=0xffffffffu;break;
                case 5:bad[0].point={NAN,0,0};break;case 6:bad[0].point={100,200,800};break;
                case 7:bad[0].point={40000,0,0};break;}
            Check(!SelectThreat(view,bad),"Ineligible or directionless enemy selected for backwards yaw");
        }
        // No living enemy: still turn around camera-relative (view yaw + 180)
        // with head-hiding pitch, instead of disabling anti aim.
        const auto fallback=Angles(view.angles,77,options);
        Check(fallback&&same(fallback->y,view.angles.y+180)&&fallback->x==view.angles.x,"Missing target did not turn around camera-relative");
        const auto fallbackNan=Angles(view.angles,77,options,1.f/64.f,NAN);
        Check(fallbackNan&&same(fallbackNan->y,view.angles.y+180),"NaN target yaw did not fall back to camera-relative backwards yaw");
        Check(!Bearing({NAN,0,0},{100,0,0})&&!Bearing({0,0,0},{0,0,100}),"Invalid/coincident horizontal bearing accepted");
    }

    void RecoilAndSpreadToggles()
    {
        const Vec3 desired{10,20,0}, punch{2,3,0};
        // This seed fixture depends on quantized physical pitch/yaw and tick,
        // as the native helper does. It deliberately ignores command roll.
        const auto seed=[](Vec3 angles,int tick)->std::optional<std::uint32_t> {
            const auto pitchBin=static_cast<int>(std::floor(std::remainder(double(angles.x),360.)*2.+.5));
            const auto yawBin=static_cast<int>(std::floor(std::remainder(double(angles.y),360.)*2.+.5));
            return (static_cast<std::uint32_t>(tick)&255u)*0x1000000u+
                   static_cast<std::uint32_t>(pitchBin+720)*4096u+static_cast<std::uint32_t>(yawBin+720);
        };
        constexpr Vec2 spread{.02f,.015f};
        const auto sample=[](std::uint32_t)->std::optional<Vec2> {return spread;};
        for (const bool recoilCorrection : {false,true}) {
            const auto physical=hvh_math::ShotAim(desired,punch,recoilCorrection);
            Check(bool(physical),"Valid recoil-toggle physical shot aim failed");
            const auto ordinary=hvh_math::RemovePunch(*physical,punch);
            Check(ordinary&&ordinary->x==(recoilCorrection?8.f:10.f)&&ordinary->y==(recoilCorrection?17.f:20.f),
                  "Ordinary command no longer respects recoil toggle");
            const auto corrected=hvh_math::FindSpreadCorrection(*physical,77,seed,sample);
            Check(bool(corrected),"Recoil toggle prevented a consistent spread solution");
            const auto command=hvh_math::RemovePunch(*corrected,punch);
            Check(command&&command->z==corrected->z,"Removing recoil lost solved spread roll");
            const auto serverPhysical=hvh_math::ShotAim(*command,punch,false);
            Check(serverPhysical&&seed(*serverPhysical,77)==seed(*corrected,77),
                  "Server post-recoil physical angle hashes a different spread seed");
            Check(Difference(IndependentPellet(*serverPhysical,spread),IndependentPellet(*physical,{}))<3e-7,
                  "Recoil toggle changed compensated physical pellet direction");
        }
        // Reproduce the old recoil-OFF bug: solving the selected angle and
        // writing it directly lets server-added recoil invalidate that seed.
        const auto oldCorrected=hvh_math::FindSpreadCorrection(desired,77,seed,sample);
        Check(bool(oldCorrected),"Old-path reproduction lacks a valid correction");
        const auto oldServerPhysical=hvh_math::ShotAim(*oldCorrected,punch,false);
        Check(oldServerPhysical&&seed(*oldServerPhysical,77)!=seed(*oldCorrected,77)&&
              Difference(IndependentPellet(*oldServerPhysical,spread),IndependentPellet(desired,{}))>.01,
              "Recoil-OFF fixture does not detect the original physical seed/direction mismatch");
        const auto seam=hvh_math::ShotAim({12,179,4},{1,4,99},false);
        Check(seam&&seam->x==13&&seam->y==-177&&seam->z==4,"Post-recoil yaw seam or roll was mishandled");
        const auto seamCommand=hvh_math::RemovePunch(*seam,{1,4,99});
        Check(seamCommand&&seamCommand->x==12&&seamCommand->y==179&&seamCommand->z==4,
              "Recoil roundtrip lost yaw seam or included native roll punch");
        const auto steep=hvh_math::ShotAim({88,0,0},{10,0,0},false);
        Check(steep&&steep->x==98&&hvh_math::RemovePunch(*steep,{10,0,0})->x==88,
              "Valid ordinary command rejected its natural post-recoil pitch");
        Check(!hvh_math::RemovePunch({88,0,0},{-2,0,0}),"Recoil command pitch was clamped after seed solve");
        const float nan=std::numeric_limits<float>::quiet_NaN(), inf=std::numeric_limits<float>::infinity();
        for (const Vec3 invalid : {Vec3{nan,0,0},Vec3{0,inf,0},Vec3{0,0,nan}}) {
            Check(!hvh_math::ShotAim(invalid,punch,true)&&!hvh_math::ShotAim(desired,invalid,false)&&
                  !hvh_math::RemovePunch(invalid,punch)&&!hvh_math::RemovePunch(desired,invalid),
                  "Nonfinite recoil/physical shot input accepted");
        }
        volatile float maximumValue=std::numeric_limits<float>::max();
        const float maximum=maximumValue;
        Check(!hvh_math::ShotAim({maximum,0,0},{maximum,0,0},false),"Physical pitch overflow accepted");
        Check(!hvh_math::RemovePunch({maximum,0,0},{-maximum,0,0}),"Command pitch overflow accepted");
    }
    void AllDirectionFov()
    {
        auto view=View();
        const auto behind=Player({-100,0,0});
        Check(!hvh_math::ProjectCandidate(view,behind,0),
              "Default angular targeting unexpectedly accepted a rear target");
        view.allowBehind=true;view.maxFov=180;
        const auto full=hvh_math::ProjectCandidate(view,behind,0);
        Check(full&&full->angularDistance==180.f,
              "Explicit full-angle targeting rejected a valid rear target");
        view.maxFov=90;
        Check(!hvh_math::ProjectCandidate(view,behind,0),
              "All-direction targeting bypassed the configured angular FOV");
        view.maxFov=180;view.allowBehind=false;
        Check(!hvh_math::ProjectCandidate(view,behind,0),
              "Rear targeting remained enabled after its explicit policy was cleared");
    }

    void QualifiedShotRanking()
    {
        using hvh_accuracy::QualifiedShot;
        using hvh_accuracy::Choose;
        std::array shots{
            QualifiedShot{30.f,.9f,1.f,100.f,100,false,0},
            QualifiedShot{110.f,.75f,30.f,500.f,100,true,1}
        };
        Check(Choose(shots)==1,"Crosshair proximity displaced a qualified lethal shot");
        shots[0].damage=120.f;shots[0].chance=.8f;
        Check(Choose(shots)==0,"Qualified lethal shots did not prefer higher confidence");
        shots[0]={60.f,.75f,1.f,100.f,100,false,0};
        shots[1]={55.f,.95f,30.f,500.f,100,true,1};
        Check(Choose(shots)==1,"Nonlethal shot ranking ignored damage weighted by estimated probability");
        shots[0]={50.f,.8f,20.f,100.f,100,false,0};
        shots[1]={50.f,.8f,1.f,100.f,100,true,1};
        Check(Choose(shots)==0,"Equal qualified shot quality did not prefer a clear path");
        shots[1].penetrated=false;
        Check(Choose(shots)==1,"Equal shot quality did not prefer the smaller angular correction");
        shots[0]=shots[1];shots[0].index=0;shots[1].index=1;
        Check(Choose(shots)==0,"Qualified shot ties were not deterministic");
        for (int failure{}; failure<8; ++failure) {
            auto invalid=shots;
            switch(failure) {
            case 0:invalid[0].damage=NAN;break;
            case 1:invalid[0].damage=0;break;
            case 2:invalid[0].chance=0;break;
            case 3:invalid[0].chance=1.1f;break;
            case 4:invalid[0].angularDistance=181;break;
            case 5:invalid[0].distance=0;break;
            case 6:invalid[0].distance=INFINITY;break;
            case 7:invalid[0].health=0;break;
            }
            Check(Choose(invalid)==1,"Malformed qualified shot displaced a valid candidate");
        }
        Check(!Choose({}),"Empty qualified-shot list produced a target");
    }

    void FirstImpactGroupConfidence()
    {
        using hvh_accuracy::Evaluate;
        using hvh_accuracy::EvaluateGroup;
        // Independent ray/sphere first intersections model a small head and
        // torso below it. Counting any part of the same pawn would call this
        // 100% head accuracy, despite half the rays first hitting its torso.
        const auto firstGroup=[](Vec3 delta)->std::optional<int> {
            struct Sphere { Vec3 center; int group; };
            constexpr std::array geometry{Sphere{{100,0,0},1},Sphere{{100,0,-3},2}};
            const double lengthSquared=double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z;
            double nearest=1.;int group{};
            for(const auto& sphere:geometry) {
                const auto& center=sphere.center;
                const double projection=double(delta.x)*center.x+double(delta.y)*center.y+double(delta.z)*center.z;
                const double squared=double(center.x)*center.x+double(center.y)*center.y+double(center.z)*center.z;
                const double discriminant=projection*projection-lengthSquared*(squared-1.);
                if(discriminant<0.)continue;
                const double entry=(projection-std::sqrt(discriminant))/lengthSquared;
                if(entry>=0.&&entry<nearest){nearest=entry;group=sphere.group;}
            }
            return group;
        };
        const auto mixed=[](unsigned seed)->std::optional<Vec2> {
            return seed<64 ? Vec2{} : Vec2{0.f,-.03f};
        };
        const auto anyRegion=Evaluate({},1000.f,.7f,true,mixed,
            [&](Vec3 delta)->std::optional<bool> {return firstGroup(delta).value_or(0)!=0;});
        const auto head=EvaluateGroup({},1000.f,.7f,true,1,mixed,firstGroup);
        Check(anyRegion&&anyRegion->passed&&anyRegion->chance==1.f&&
              head&&!head->passed&&head->hits==64&&head->samples<128&&head->chance<.7f,
              "Torso spread hits inherited a head candidate's damage confidence");
        const auto torso=EvaluateGroup({},1000.f,.7f,true,2,mixed,firstGroup);
        Check(torso&&!torso->passed&&torso->samples<128&&torso->chance<.7f,
              "Head spread hits inherited a torso candidate's damage confidence");
        const auto headFull=EvaluateGroup({},1000.f,.4f,true,1,mixed,firstGroup);
        const auto torsoFull=EvaluateGroup({},1000.f,.4f,true,2,mixed,firstGroup);
        Check(headFull&&headFull->passed&&headFull->hits==64&&headFull->samples==128&&headFull->chance==.5f&&
              torsoFull&&torsoFull->passed&&torsoFull->hits==64&&torsoFull->samples==128&&torsoFull->chance==.5f,
              "Passing group-specific estimates did not retain their full ranking confidence");
        const auto accurate=[](unsigned)->std::optional<Vec2> {return Vec2{};};
        const Vec3 bodyAim{static_cast<float>(std::atan2(3.,100.)*180./std::numbers::pi),0,0};
        const auto bodyShot=EvaluateGroup(bodyAim,1000.f,1.f,true,2,accurate,firstGroup);
        Check(bodyShot&&bodyShot->passed&&bodyShot->chance==1.f,
              "An independently aimed body candidate could not qualify its own impact group");
        const auto miss=EvaluateGroup({0,90,0},1000.f,.7f,true,1,accurate,firstGroup);
        Check(miss&&miss->chance==0.f&&!miss->passed,
              "A clear geometric miss was confused with unavailable impact metadata");
        const auto unknown=[](Vec3)->std::optional<int> {return {};};
        const auto malformed=[](Vec3)->std::optional<int> {return 9;};
        Check(!EvaluateGroup({},1000.f,.7f,true,1,accurate,unknown)&&
              !EvaluateGroup({},1000.f,.7f,true,1,accurate,malformed)&&
              !EvaluateGroup({},1000.f,.7f,true,0,accurate,firstGroup)&&
              !EvaluateGroup({},1000.f,.7f,true,9,accurate,firstGroup),
              "Unavailable or unsupported impact groups qualified a damage claim");
    }

    void AccuracyQualification()
    {
        using hvh_accuracy::Evaluate;
        unsigned calls{}, intersections{};
        // A real angularly small target: a sphere at X=100 with radius 1.
        // The point at the ray's nearest approach must be inside the sphere,
        // and that approach must be between the eye and the weapon endpoint.
        const auto sphere=[&](Vec3 delta)->std::optional<bool> {
            ++intersections;
            const double lengthSquared=double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z;
            const double fraction=100.*delta.x/lengthSquared;
            if (fraction<0. || fraction>1.) return false;
            const double x=delta.x*fraction-100., y=delta.y*fraction, z=delta.z*fraction;
            return x*x+y*y+z*z<=1.;
        };
        const auto moving=[&](unsigned seed)->std::optional<Vec2> {
            ++calls;
            return seed<80 ? Vec2{} : Vec2{.03f,0.f};
        };
        const auto qualified=Evaluate({},1000.f,.6f,true,moving,sphere);
        Check(qualified&&qualified->passed&&qualified->hits==80&&qualified->samples==128&&
              qualified->chance==.625f&&calls==128&&intersections==128,
              "Native spread qualification did not count actual target intersections");
        calls=0;intersections=0;
        const auto rejected=Evaluate({},1000.f,.8f,true,moving,sphere);
        Check(rejected&&!rejected->passed&&rejected->hits==80&&rejected->samples==106&&
              rejected->chance==static_cast<float>(80./106.)&&calls==106&&intersections==106,
              "Low-hitchance moving shots were qualified");
        const auto stopped=[](unsigned)->std::optional<Vec2> {return Vec2{};};
        const auto accurate=Evaluate({},1000.f,1.f,true,stopped,sphere);
        Check(accurate&&accurate->passed&&accurate->chance==1.f,
              "Fully accurate shot was rejected");
        const auto outOfRange=Evaluate({},50.f,.1f,true,stopped,sphere);
        Check(outOfRange&&!outOfRange->passed&&outOfRange->chance==0.f,
              "Hitchance ignored weapon range");
        const auto wrongAim=Evaluate({0.f,90.f,0.f},1000.f,.1f,true,stopped,sphere);
        Check(wrongAim&&!wrongAim->passed&&wrongAim->chance==0.f,
              "Hitchance ignored physical shot angles");
        const auto missing=[](unsigned)->std::optional<Vec2> {return {};};
        const auto malformed=[](unsigned)->std::optional<Vec2> {return Vec2{NAN,0.f};};
        const auto missingGeometry=[](Vec3)->std::optional<bool> {return {};};
        Check(!Evaluate({},1000.f,.8f,true,missing,sphere)&&
              !Evaluate({},1000.f,.8f,true,malformed,sphere)&&
              !Evaluate({},1000.f,.8f,true,stopped,missingGeometry),
              "Unavailable native samples or hitbox geometry claimed valid accuracy");
        const auto lateFailure=[](unsigned seed)->std::optional<Vec2> {
            return seed==127 ? std::nullopt : std::optional{Vec2{}};
        };
        Check(!Evaluate({},1000.f,.8f,true,lateFailure,sphere),
              "An early success concealed failed native accuracy sampling");
        calls=0;intersections=0;
        Check(!Evaluate({},1000.f,.8f,false,moving,sphere)&&calls==0&&intersections==0,
              "Unknown weapon accuracy was sampled or qualified");
        for (const float range : {0.f,-1.f,32769.f,NAN,INFINITY})
            Check(!Evaluate({},range,.8f,true,stopped,sphere),"Invalid weapon range accepted for hitchance");
        for (const float chance : {-.1f,1.1f,NAN,INFINITY})
            Check(!Evaluate({},1000.f,chance,true,stopped,sphere),"Invalid hitchance threshold accepted");
        Check(!Evaluate({NAN,0.f,0.f},1000.f,.8f,true,stopped,sphere)&&
              !Evaluate({},1000.f,.8f,true,stopped,sphere,0)&&
              !Evaluate({},1000.f,.8f,true,stopped,sphere,513),
              "Invalid aim or unbounded accuracy sampling accepted");
    }

    void AccuracyBoundsAndCache()
    {
        using hvh_accuracy::Evaluate;
        // Exhaustively compare all possible hit counts in this16-ray fixture
        // with the full-set verdict. Passing ranks must retain the full set.
        for(unsigned totalHits=0;totalHits<=16;++totalHits)
            for(float threshold:{0.f,.25f,.5f,.75f,1.f}) {
                unsigned intersections{};
                const auto result=Evaluate({},1000.f,threshold,true,
                    [](unsigned)->std::optional<Vec2>{return Vec2{};},
                    [&](Vec3)->std::optional<bool>{return intersections++<totalHits;},16);
                const bool passed=double(totalHits)/16>=double(threshold);
                Check(result&&result->passed==passed,"Early accuracy exit changed the full-set verdict");
                if(passed)
                    Check(result->hits==totalHits&&result->samples==16&&intersections==16,
                          "Early accuracy success changed a passing ranking estimate");
                else
                    Check(double(result->hits+16-result->samples)/16<double(threshold)&&intersections==result->samples,
                          "Accuracy exited before the threshold became mathematically impossible");
            }
        unsigned misses{};
        const auto impossible=Evaluate({},1000.f,1.f,true,
            [](unsigned)->std::optional<Vec2>{return Vec2{};},
            [&](Vec3)->std::optional<bool>{++misses;return false;});
        Check(impossible&&!impossible->passed&&impossible->samples==1&&misses==1,
              "A100-percent threshold miss sampled unnecessary remaining rays");

        hvh_accuracy::SpreadCache<128> cache;
        std::array<unsigned,128> nativeCalls{};
        const auto native=[&](unsigned seed)->std::optional<Vec2>{++nativeCalls[seed];return Vec2{};};
        for(unsigned point=0;point<32;++point) {
            const auto result=Evaluate({},1000.f,.7f,true,
                [&](unsigned seed){return cache.Get(seed,native);},
                [](Vec3)->std::optional<bool>{return true;});
            Check(result&&result->passed&&result->samples==128,"Cached samples changed target qualification");
        }
        for(const auto calls:nativeCalls)Check(calls==1,"One command regenerated the same native spread seed");
        unsigned unavailableCalls{};
        hvh_accuracy::SpreadCache<128> unavailable;
        const auto failed=[&](unsigned)->std::optional<Vec2>{++unavailableCalls;return {};};
        Check(!unavailable.Get(7,failed)&&!unavailable.Get(7,failed)&&unavailableCalls==1,
              "Unavailable native spread was not cached");
        Check(!unavailable.Get(128,failed)&&unavailableCalls==1,"An out-of-bounds spread seed reached the native sampler");
        unsigned malformedCalls{};
        const auto malformed=[&](unsigned)->std::optional<Vec2>{++malformedCalls;return Vec2{NAN,0};};
        Check(!unavailable.Get(8,malformed)&&!unavailable.Get(8,malformed)&&malformedCalls==1,
              "Malformed spread was returned or resampled");
        hvh_accuracy::SpreadCache<128> nextCommand;
        Check(nextCommand.Get(7,native).has_value()&&nativeCalls[7]==2,
              "A new command reused stale weapon accuracy samples");
    }
}

int main()
{
    try
    {
        TargetSelection();
        IdentityAndInputs();
        ScreenRanking();
        SpreadGeometry();
        SpreadSeedSearch();
        IterativeSpreadSeedSearch();
        ShotPrediction();
        AntiAimAngles();
        TargetRelativeBackward();
        RecoilAndSpreadToggles();
        AccuracyQualification();
        AccuracyBoundsAndCache();
        AllDirectionFov();
        QualifiedShotRanking();
        FirstImpactGroupConfidence();
        ActivationModes();
        std::cout << "HVH math regression: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "HVH math regression: FAIL: " << error.what() << '\n';
        return 1;
    }
}
