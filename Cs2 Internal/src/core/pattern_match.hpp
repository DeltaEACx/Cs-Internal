#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace native_startup {
// Search the longest literal run first, then validate the complete wildcard
// pattern. Advancing one byte after a match preserves overlapping matches.
class PatternMatcher {
    std::span<const int> pattern;
    std::vector<std::uint8_t> literal;
    std::size_t literalOffset{};
public:
    explicit PatternMatcher(std::span<const int> value):pattern(value) {
        std::size_t bestLength{};
        for(std::size_t begin{};begin<pattern.size();) {
            if(pattern[begin]<0){++begin;continue;}
            auto end=begin;
            while(end<pattern.size()&&pattern[end]>=0)++end;
            if(end-begin>bestLength){literalOffset=begin;bestLength=end-begin;}
            begin=end;
        }
        for(std::size_t i{};i<bestLength;++i)literal.push_back(static_cast<std::uint8_t>(pattern[literalOffset+i]));
    }
    template<class Match>
    bool Scan(std::span<const std::uint8_t> bytes,Match&& match) const {
        if(literal.empty()||bytes.size()<pattern.size())return true;
        const std::boyer_moore_horspool_searcher searcher(literal.begin(),literal.end());
        const auto begin=bytes.data(),end=begin+bytes.size();
        auto cursor=begin;
        while(cursor<end) {
            const auto found=std::search(cursor,end,searcher);
            if(found==end)break;
            const auto offset=static_cast<std::size_t>(found-begin);
            if(offset>=literalOffset) {
                const auto candidate=offset-literalOffset;
                if(candidate<=bytes.size()-pattern.size()) {
                    bool valid=true;
                    for(std::size_t i{};i<pattern.size();++i)
                        if(pattern[i]>=0&&bytes[candidate+i]!=pattern[i]){valid=false;break;}
                    if(valid&&!match(candidate))return false;
                }
            }
            cursor=found+1;
        }
        return true;
    }
};
}
