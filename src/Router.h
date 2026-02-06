/*
    Author: Lukas (thelstv)
    Copyright: (c) https://lstv.space

    Last modified: 2026
    License: GPL-3.0
    Version: 1.1.0-cpp
    Description: A routing/matching module for Akeno, allowing to match domains and paths with wildcards and groups.
    Translated from the original JavaScript implementation.

    Warning: this is a prototype implementation and not production-ready code.
*/

/**
 * Pattern syntax:
 * - Static segments: /home, /about => match exactly /home, /about
 * - Wildcards (one segment): /user/* => match /user/123, but *not* /user/ or /user/123/profile ({,*} can be used to allow /user too)
 * - Double wildcards: /files/** (zero or more segments) => match /files/, /files/docs/report.pdf, etc.
 * - Groups: /user/{a,b,c} => match /user/a, /user/b, or /user/c
*/

#ifndef AKENO_ROUTER_H
#define AKENO_ROUTER_H

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <stdexcept>

namespace Akeno {
    namespace Internal {
        inline std::string trim_copy(std::string s) {
            auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }

        inline std::vector<std::string> split(const std::string& input, char delimiter) {
            std::vector<std::string> parts;
            size_t start = 0;
            size_t end = input.find(delimiter);
            while (end != std::string::npos) {
                parts.push_back(input.substr(start, end - start));
                start = end + 1;
                end = input.find(delimiter, start);
            }
            parts.push_back(input.substr(start));
            return parts;
        }

        inline void expandPattern(std::string pattern, std::vector<std::string>& out) {
            size_t searchFrom = 0;
            while (true) {
                size_t group = pattern.find('{', searchFrom);
                if (group == std::string::npos) break;

                const char prevChar = (group > 0? pattern[group - 1]: '\0');
                if (prevChar != '!') {
                    size_t endGroup = pattern.find('}', group);
                    if (endGroup == std::string::npos) {
                        throw std::runtime_error("Unmatched group in pattern: " + pattern);
                    }

                    std::string groupValues = pattern.substr(group + 1, endGroup - group - 1);
                    std::string patternStart = pattern.substr(0, group);
                    std::string patternEnd = pattern.substr(endGroup + 1);

                    for (std::string value : split(groupValues, ',')) {
                        value = trim_copy(std::move(value));
                        std::string nextEnd = patternEnd;

                        if (value.empty() && !patternEnd.empty() && patternEnd.front() == '.') {
                            nextEnd = patternEnd.substr(1);
                        }

                        expandPattern(patternStart + value + nextEnd, out);
                    }
                    return;
                }

                searchFrom = group + 1;
            }

            if (!pattern.empty() && pattern.back() == '/') {
                pattern.pop_back();
            }
            out.push_back(std::move(pattern));
        }

        inline std::vector<std::string_view> splitSegments(std::string_view s, char segmentChar) {
            if (s.empty()) return {std::string_view{}};

            bool hasLeadingSep = (!s.empty() && s.front() == segmentChar);

            std::vector<std::string_view> parts;
            if (!hasLeadingSep) {
                parts.push_back(std::string_view{});
            }

            size_t start = 0;
            while (start <= s.size()) {
                size_t pos = s.find(segmentChar, start);
                if (pos == std::string_view::npos) {
                    parts.push_back(s.substr(start));
                    break;
                }
                parts.push_back(s.substr(start, pos - start));
                start = pos + 1;
                if (start == s.size()) {
                    parts.push_back(std::string_view{});
                    break;
                }
            }
            return parts;
        }

        inline bool containsWildcardOrNegSet(std::string_view p) {
            return (p.find('*') != std::string_view::npos) || (p.find("!{") != std::string_view::npos);
        }
    }

    template <class Handler>
    struct MatcherOptions {
        bool simpleMatcher = false;
        bool mergeHandlers = false;


        std::function<Handler(Handler existing, const Handler &incoming)> mergeFn;
    };

    template <class Handler>
    class WildcardMatcher {
    public:
        explicit WildcardMatcher(char segmentChar = '/', std::vector<int> /*unused*/ = {})
            : segmentChar_(segmentChar ? segmentChar : '/') {}


        struct Part {
            enum class Type { Literal, Star, DoubleStar, NegSet, Set } type = Type::Literal;
            std::string literal;
            std::unordered_set<std::string> set;

            static Part literalPart(std::string s) {
                Part p;
                p.type = Type::Literal;
                p.literal = std::move(s);
                return p;
            }
            static Part star() {
                Part p;
                p.type = Type::Star;
                return p;
            }
            static Part doubleStar() {
                Part p;
                p.type = Type::DoubleStar;
                return p;
            }
            static Part negSet(std::unordered_set<std::string> s) {
                Part p;
                p.type = Type::NegSet;
                p.set = std::move(s);
                return p;
            }
            static Part posSet(std::unordered_set<std::string> s) {
                Part p;
                p.type = Type::Set;
                p.set = std::move(s);
                return p;
            }
        };

        struct Route {
            std::vector<Part> parts;
            Handler handler;
            std::string pattern;
            bool hasDoubleStar = false;
        };

        void add(const std::string &pattern, const Handler &handler) {

            auto raw = Internal::splitSegments(pattern, segmentChar_);
            std::vector<Part> parts;
            parts.reserve(raw.size());

            for (auto sv : raw) {
                std::string seg(sv);
                if (seg == "**") {
                    parts.push_back(Part::doubleStar());
                } else if (seg == "*") {
                    parts.push_back(Part::star());
                } else if (seg.size() > 3 && seg.rfind("!{", 0) == 0 && seg.back() == '}') {
                    std::string inner = seg.substr(2, seg.size() - 3);
                    auto values = Internal::split(inner, ',');
                    std::unordered_set<std::string> set;
                    for (auto &v : values) {
                        v = Internal::trim_copy(std::move(v));
                        if (!v.empty())
                            set.insert(std::move(v));
                    }
                    parts.push_back(Part::negSet(std::move(set)));
                } else {
                    parts.push_back(Part::literalPart(std::move(seg)));
                }
            }



            for (auto &existing : patterns_) {
                if (!(existing.handler == handler))
                    continue;
                if (existing.parts.size() != parts.size())
                    continue;

                int diffIndex = -1;
                bool canMerge = true;

                for (size_t i = 0; i < parts.size(); i++) {
                    const auto &ep = existing.parts[i];
                    const auto &np = parts[i];

                    auto equalPart = [&](const Part &a, const Part &b) -> bool {
                        if (a.type != b.type)
                            return false;
                        if (a.type == Part::Type::Literal)
                            return a.literal == b.literal;
                        if (a.type == Part::Type::NegSet || a.type == Part::Type::Set)
                            return a.set == b.set;
                        return true;
                    };

                    if (equalPart(ep, np))
                        continue;


                    if (ep.type == Part::Type::Set && np.type == Part::Type::Literal) {
                        if (diffIndex != -1) {
                            canMerge = false;
                            break;
                        }
                        diffIndex = static_cast<int>(i);
                        continue;
                    }


                    if (ep.type == Part::Type::Literal && np.type == Part::Type::Literal) {
                        if (diffIndex != -1) {
                            canMerge = false;
                            break;
                        }
                        diffIndex = static_cast<int>(i);
                        continue;
                    }

                    canMerge = false;
                    break;
                }

                if (canMerge && diffIndex != -1) {
                    auto &ep = existing.parts[static_cast<size_t>(diffIndex)];
                    const auto &np = parts[static_cast<size_t>(diffIndex)];

                    if (ep.type == Part::Type::Set && np.type == Part::Type::Literal) {
                        ep.set.insert(np.literal);
                    } else if (ep.type == Part::Type::Literal && np.type == Part::Type::Literal) {
                        std::unordered_set<std::string> s;
                        s.insert(ep.literal);
                        s.insert(np.literal);
                        ep = Part::posSet(std::move(s));
                    }
                    return;
                }
            }

            bool hasDoubleStar = false;
            for (const auto &p : parts) {
                if (p.type == Part::Type::DoubleStar) {
                    hasDoubleStar = true;
                    break;
                }
            }

            patterns_.push_back(Route{std::move(parts), handler, pattern, hasDoubleStar});

            std::sort(patterns_.begin(), patterns_.end(),
                      [](const Route &a, const Route &b) { return a.parts.size() > b.parts.size(); });

            indexDirty_ = true;
        }

        void filter(std::function<bool(const Route &)> cb) {
            std::vector<Route> kept;
            kept.reserve(patterns_.size());
            for (auto &r : patterns_) {
                if (cb(r))
                    kept.push_back(std::move(r));
            }
            patterns_ = std::move(kept);
            indexDirty_ = true;
        }

        const Handler *match(std::string_view input) const {
            auto path = Internal::splitSegments(input, segmentChar_);

            if (auto it = exactMatches_.find(input); it != exactMatches_.end()) {
                return &it->second;
            }

            rebuildIndexIfNeeded();

            for (const auto &group : sizeGroups_) {
                if (group.size > path.size() && !group.hasAnyDoubleStar) {
                    continue;
                }

                const std::string_view firstSeg = path.empty() ? std::string_view{} : path[0];
                auto literalIt = group.literalFirst.find(std::string(firstSeg));
                if (literalIt != group.literalFirst.end()) {
                    for (const Route *routePtr : literalIt->second) {
                        const Route &route = *routePtr;
                        if (route.parts.size() > path.size() && !route.hasDoubleStar)
                            continue;
                        if (matchRoute(route, path))
                            return &route.handler;
                    }
                }

                for (const Route *routePtr : group.nonLiteral) {
                    const Route &route = *routePtr;
                    if (route.parts.size() > path.size() && !route.hasDoubleStar)
                        continue;
                    if (matchRoute(route, path))
                        return &route.handler;
                }
            }

            return nullptr;
        }

        bool matchRoute(const Route &route, const std::vector<std::string_view> &path) const {
            const auto &parts = route.parts;


            if (parts.size() == 1) {
                const auto &only = parts[0];
                if (only.type == Part::Type::DoubleStar)
                    return true;

                if (only.type == Part::Type::Star) {
                    if (path.size() == 1 && path[0] != std::string_view{})
                        return true;
                } else if (only.type == Part::Type::Literal) {
                    if (path.size() == 1 && path[0] == only.literal)
                        return true;
                } else if (only.type == Part::Type::NegSet) {
                    if (path.size() == 1 && path[0] != std::string_view{} &&
                        only.set.find(std::string(path[0])) == only.set.end()) {
                        return true;
                    }
                } else if (only.type == Part::Type::Set) {
                    if (path.size() == 1 && only.set.find(std::string(path[0])) != only.set.end()) {
                        return true;
                    }
                }
                return false;
            }

                size_t pi = 0, si = 0;
                int starPi = -1, starSi = -1;

                while (si < path.size()) {
                    const Part *part = (pi < parts.size() ? &parts[pi] : nullptr);

                    if (pi < parts.size() && part->type == Part::Type::DoubleStar) {
                        starPi = static_cast<int>(pi);
                        starSi = static_cast<int>(si);
                        pi++;
                    } else if (pi < parts.size() && part->type == Part::Type::Star) {
                        if (path[si] == std::string_view{})
                            break;
                        pi++;
                        si++;
                    } else if (pi < parts.size() &&
                               (part->type == Part::Type::NegSet || part->type == Part::Type::Set)) {
                        if (path[si] == std::string_view{})
                            break;

                        std::string seg(path[si]);
                        if (part->type == Part::Type::NegSet) {
                            if (part->set.find(seg) != part->set.end())
                                break;
                        } else {
                            if (part->set.find(seg) == part->set.end())
                                break;
                        }
                        pi++;
                        si++;
                    } else if (pi < parts.size() && part->type == Part::Type::Literal &&
                               path[si] == part->literal) {
                        pi++;
                        si++;
                    } else if (starPi != -1) {
                        pi = static_cast<size_t>(starPi + 1);
                        starSi++;
                        si = static_cast<size_t>(starSi);
                    } else {
                        break;
                    }
                }

                while (pi < parts.size() && parts[pi].type == Part::Type::DoubleStar)
                    pi++;
            if (pi == parts.size() && si == path.size()) {
                return true;
            }

            return false;
        }

        void clear() {
            patterns_.clear();
            exactMatches_.clear();
            sizeGroups_.clear();
            indexDirty_ = true;
        }

        const std::vector<Route> &patterns() const { return patterns_; }

    private:
        char segmentChar_;
        std::vector<Route> patterns_;
        std::map<std::string, Handler, std::less<>> exactMatches_;

        struct SizeGroup {
            size_t size = 0;
            bool hasAnyDoubleStar = false;
            std::unordered_map<std::string, std::vector<const Route *>> literalFirst;
            std::vector<const Route *> nonLiteral;
        };

        mutable bool indexDirty_ = true;
        mutable std::vector<SizeGroup> sizeGroups_;

        void rebuildIndexIfNeeded() const {
            if (!indexDirty_)
                return;

            sizeGroups_.clear();
            std::unordered_map<size_t, size_t> sizeToIndex;

            for (const auto &route : patterns_) {
                size_t sz = route.parts.size();
                auto it = sizeToIndex.find(sz);
                if (it == sizeToIndex.end()) {
                    sizeGroups_.push_back(SizeGroup{});
                    sizeGroups_.back().size = sz;
                    sizeToIndex.emplace(sz, sizeGroups_.size() - 1);
                    it = sizeToIndex.find(sz);
                }

                SizeGroup &group = sizeGroups_[it->second];
                if (route.hasDoubleStar)
                    group.hasAnyDoubleStar = true;

                if (!route.parts.empty() && route.parts.front().type == Part::Type::Literal) {
                    group.literalFirst[route.parts.front().literal].push_back(&route);
                } else {
                    group.nonLiteral.push_back(&route);
                }
            }

            std::sort(sizeGroups_.begin(), sizeGroups_.end(),
                      [](const SizeGroup &a, const SizeGroup &b) { return a.size > b.size; });

            indexDirty_ = false;
        }
    };

    template <class Handler>
    class SimpleWildcardMatcher {
    public:
        struct Compiled {
            std::vector<std::string> parts;
            Handler handler;
            std::string pattern;
            bool hasPrefix = false;
            bool hasSuffix = false;
            std::vector<std::string> nonEmptyParts;
        };

        void add(const std::string &pattern, const Handler &handler) {

            std::vector<std::string> parts;
            {
                size_t start = 0;
                while (true) {
                    size_t pos = pattern.find('*', start);
                    if (pos == std::string::npos) {
                        parts.push_back(pattern.substr(start));
                        break;
                    }
                    parts.push_back(pattern.substr(start, pos - start));
                    start = pos + 1;
                }
            }

            Compiled c;
            c.parts = std::move(parts);
            c.handler = handler;
            c.pattern = pattern;
            c.hasPrefix = !c.parts.empty() && !c.parts.front().empty();
            c.hasSuffix = !c.parts.empty() && !c.parts.back().empty();
            for (const auto &p : c.parts)
                if (!p.empty())
                    c.nonEmptyParts.push_back(p);

            compiled_.push_back(std::move(c));
        }

        void filter(std::function<bool(const Compiled &)> cb) {
            std::vector<Compiled> kept;
            kept.reserve(compiled_.size());
            for (auto &c : compiled_)
                if (cb(c))
                    kept.push_back(std::move(c));
            compiled_ = std::move(kept);
        }

        const Handler *match(std::string_view input) const {

            for (const auto &c : compiled_) {
                if (c.hasPrefix) {
                    if (input.size() < c.parts.front().size())
                        continue;
                    if (input.substr(0, c.parts.front().size()) != c.parts.front())
                        continue;
                }
                if (c.hasSuffix) {
                    if (input.size() < c.parts.back().size())
                        continue;
                    if (input.substr(input.size() - c.parts.back().size()) != c.parts.back())
                        continue;
                }

                if (c.nonEmptyParts.size() <= 2) {
                    return &c.handler;
                }

                size_t pos = c.hasPrefix ? c.parts.front().size() : 0;
                bool failed = false;


                for (size_t i = 1; i + 1 < c.parts.size(); ++i) {
                    if (c.parts[i].empty())
                        continue;
                    auto found = input.find(c.parts[i], pos);
                    if (found == std::string_view::npos) {
                        failed = true;
                        break;
                    }
                    pos = found + c.parts[i].size();
                }

                if (!failed)
                    return &c.handler;
            }

            return nullptr;
        }

        void clear() { compiled_.clear(); }

    private:
        std::vector<Compiled> compiled_;
    };

    template <class Handler>
    class Matcher {
    public:
        explicit Matcher(MatcherOptions<Handler> options = {}, char segmentChar = '/')
            : options_(std::move(options)),
              segmentChar_(segmentChar ? segmentChar : '/'),
              wildcards_(segmentChar_),
              simpleWildcards_() {}


        void add(const std::vector<std::string> &patterns, Handler handler) {
            for (const auto &p : patterns)
                add(p, handler);
        }

        void add(std::string pattern, Handler handler) {

            if (!pattern.empty() && pattern.back() == '.') {

                pattern.pop_back();
            }

            if (pattern == "*" || pattern == "**") {
                fallback_ = handler;
                return;
            }

            if (pattern.empty()) {

                return;
            }

            std::vector<std::string> expanded;
            Internal::expandPattern(pattern, expanded);

            for (const auto &expandedPattern : expanded) {

                if (Internal::containsWildcardOrNegSet(expandedPattern)) {
                    if (options_.simpleMatcher) {
                        simpleWildcards_.add(expandedPattern, handler);
                    } else {
                        wildcards_.add(expandedPattern, handler);
                    }
                    continue;
                }

                auto it = exactMatches_.find(expandedPattern);
                if (it != exactMatches_.end()) {
                    if (!(it->second == handler)) {
                        if (options_.mergeHandlers && options_.mergeFn) {
                            it->second = options_.mergeFn(it->second, handler);
                            continue;
                        }

                    }
                }

                exactMatches_[expandedPattern] = handler;
            }
        }

        void clear() {
            exactMatches_.clear();
            wildcards_.clear();
            simpleWildcards_.clear();
            fallback_.reset();
        }

        void remove(const std::string &pattern) {
            std::vector<std::string> expanded;
            Internal::expandPattern(pattern, expanded);

            for (const auto &expandedPattern : expanded) {
                exactMatches_.erase(expandedPattern);

                if (options_.simpleMatcher) {
                    simpleWildcards_.filter([&](const auto &r) { return r.pattern != expandedPattern; });
                } else {
                    wildcards_.filter([&](const auto &r) { return r.pattern != expandedPattern; });
                }
            }
        }


        const Handler *match(std::string_view input) const {

            if (auto it = exactMatches_.find(input); it != exactMatches_.end()) {
                return &it->second;
            }


            if (options_.simpleMatcher) {
                if (auto h = simpleWildcards_.match(input))
                    return h;
            } else {
                if (auto h = wildcards_.match(input))
                    return h;
            }


            if (fallback_)
                return &*fallback_;
            return nullptr;
        }

        Handler *match(std::string_view input) {
            return const_cast<Handler *>(const_cast<const Matcher *>(this)->match(input));
        }

    protected:
        MatcherOptions<Handler> options_;
        char segmentChar_;

        std::map<std::string, Handler, std::less<>> exactMatches_;

        WildcardMatcher<Handler> wildcards_;
        SimpleWildcardMatcher<Handler> simpleWildcards_;

        std::optional<Handler> fallback_;
    };

    template <class Handler>
    class DomainRouter : public Matcher<Handler> {
    public:
        explicit DomainRouter(MatcherOptions<Handler> options = {})
            : Matcher<Handler>(std::move(options), '.') {}
    };

    template <class Handler>
    class PathMatcher : public Matcher<Handler> {
    public:
        explicit PathMatcher(MatcherOptions<Handler> options = {})
            : Matcher<Handler>(std::move(options), '/') {}
    };
}

#endif