#include "features/combat/detail/hvh_scan.hpp"
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using hvh_scan::Point;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void FairLiveCenters() {
    // A nearby head contributes many angularly attractive multipoints and old
    // poses. A scan must still test current body centers and another enemy.
    std::vector<Point> points{{0,10,0,false,0,1,100},{1,10,0,false,1,2,100},
                              {2,20,0,false,0,30,500},{3,20,0,false,1,31,500}};
    for (unsigned index=4;index<100;++index) points.push_back({index,10,index%3==0?1u:0u,true,0,.01f,100});
    const auto order=hvh_scan::Order(points,4);
    Check(order==std::vector<std::size_t>{0,2,1,3},"Near head multipoints exhausted the budget before other enemies/body centers");
    unsigned nativeTraces{};
    for ([[maybe_unused]]const auto index:order) ++nativeTraces;
    Check(nativeTraces==4,"A scheduled scan exceeded its native trace budget");
    Check(hvh_scan::Order(points,1)==std::vector<std::size_t>{0},"First target ordering is unstable");
}
void FreshnessAndCenters() {
    std::array points{
        Point{0,10,2,false,0,0,100},Point{1,10,1,true,0,0,100},Point{2,10,0,true,0,0,100},
        Point{3,10,1,false,0,0,100},Point{4,10,0,false,0,30,100},Point{5,20,1,false,0,20,100}
    };
    Check(hvh_scan::Order(points,6)==std::vector<std::size_t>{4,2,3,5,1,0},"Angularly close stale poses displaced authoritative current/newest poses");
    Check(hvh_scan::Order(points,3)==std::vector<std::size_t>{4,2,3},"History fallback escaped the shared scan budget");
    std::reverse(points.begin(),points.end());
    Check(hvh_scan::Order(points,6)==std::vector<std::size_t>{4,2,3,5,1,0},"Input ordering changed the deterministic schedule");
}
void BoundsAndMalformedData() {
    std::vector<Point> points;
    for(unsigned target=1;target<=128;++target)
        for(unsigned point=0;point<20;++point)
            points.push_back({points.size(),target,0,false,point,static_cast<float>(target),100});
    const auto order=hvh_scan::Order(points,48);
    std::set<std::uint64_t> targets;
    unsigned nativeTraces{};
    for(const auto index:order){targets.insert(points[index].targetKey);++nativeTraces;}
    Check(nativeTraces==48&&targets.size()==48,"A crowded server scan failed the48-trace bound or pawn fairness");
    const auto capped=hvh_scan::Order(points,10000);
    Check(capped.size()==hvh_scan::maximumBudget,"The scheduler accepted an unbounded command budget");
    Check(std::set<std::size_t>(capped.begin(),capped.end()).size()==capped.size(),"A candidate was scheduled more than once");
    Check(hvh_scan::Order(points,0).empty()&&hvh_scan::Order({},48).empty(),"Empty/disabled scan produced work");
    std::array malformed{
        Point{0,1,0,false,0,0,100},Point{0,1,0,false,0,0,100},
        Point{1,0,0,false,0,0,100},Point{2,1,17,false,0,0,100},Point{3,1,0,false,256,0,100},
        Point{4,1,0,false,0,NAN,100},Point{5,1,0,false,0,181,100},Point{6,1,0,false,0,0,0},
        Point{7,1,0,false,0,0,INFINITY},Point{8,1,0,false,0,0,32769},
        Point{std::numeric_limits<std::size_t>::max(),1,0,false,0,0,100}
    };
    Check(hvh_scan::Order(malformed,48)==std::vector<std::size_t>{0},"Malformed or duplicate scan points consumed the budget");
}
}
int main() {
    try { FairLiveCenters();FreshnessAndCenters();BoundsAndMalformedData();
        std::cout<<"PASS: bounded fair target scheduling, current centers, history age, multipoints and malformed inputs\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
