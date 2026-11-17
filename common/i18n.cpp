#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eco::i18n {

std::string fromServerDepth(const std::string& es, int depth);

namespace {

struct Pair { const char* a; const char* b; };

// X: fixed text (English -> Spanish). S: fixed server text (Spanish -> English). P: server notice with gaps {}.
// W: loose word the server puts in its notices (roles...), to translate what falls in a gap.
const Pair kText[] = {
#define X(en, es) {en, es},
#define S(es, en)
#define P(es, en)
#define W(es, en)
#include "i18n_es.inc"
#undef X
#undef S
#undef P
#undef W
};
const Pair kServerText[] = {
#define X(en, es)
#define S(es, en) {es, en},
#define P(es, en)
#define W(es, en)
#include "i18n_es.inc"
#undef X
#undef S
#undef P
#undef W
};
const Pair kPattern[] = {
#define X(en, es)
#define S(es, en)
#define P(es, en) {es, en},
#define W(es, en)
#include "i18n_es.inc"
#undef X
#undef S
#undef P
#undef W
};
const Pair kWord[] = {
#define X(en, es)
#define S(es, en)
#define P(es, en)
#define W(es, en) {es, en},
#include "i18n_es.inc"
#undef X
#undef S
#undef P
#undef W
};

using Map = std::unordered_map<std::string_view, const char*>;

template <size_t N>
Map makeMap(const Pair (&pairs)[N]) {
    Map r;
    r.reserve(N);
    for (auto& p : pairs) r.emplace(p.a, p.b);
    return r;
}

const Map& textMap() {
    static const Map m = makeMap(kText);
    return m;
}
const Map& serverMap() {
    static const Map m = makeMap(kServerText);
    return m;
}
const Map& wordMap() {
    static const Map m = makeMap(kWord);
    return m;
}

struct Pattern {
    std::vector<std::string> pieces;  // the text between gaps: n gaps -> n+1 pieces
    const char* en;
    size_t fixed = 0;  // how much fixed text it has (the most specific ones are tried first)
};

const std::vector<Pattern>& patterns() {
    static const auto v = [] {
        std::vector<Pattern> r;
        for (auto& p : kPattern) {
            Pattern x;
            x.en = p.b;
            std::string_view s = p.a;
            for (;;) {
                size_t h = s.find("{}");
                x.pieces.emplace_back(s.substr(0, h));
                if (h == std::string_view::npos) break;
                s.remove_prefix(h + 2);
            }
            for (auto& q : x.pieces) x.fixed += q.size();
            r.push_back(std::move(x));
        }
        std::stable_sort(r.begin(), r.end(), [](const Pattern& a, const Pattern& b) { return a.fixed > b.fixed; });
        return r;
    }();
    return v;
}

bool match(const Pattern& p, const std::string& s, std::vector<std::string>& caps) {
    caps.clear();
    const auto& pc = p.pieces;
    if (s.compare(0, pc[0].size(), pc[0]) != 0) return false;
    size_t pos = pc[0].size();
    for (size_t i = 1; i < pc.size(); ++i) {
        const std::string& lit = pc[i];
        size_t at;
        if (i + 1 == pc.size()) {  // the last piece has to be the end
            if (s.size() < pos + lit.size() || s.compare(s.size() - lit.size(), lit.size(), lit) != 0) return false;
            at = s.size() - lit.size();
        } else {
            if (lit.empty()) return false;  // two gaps in a row: ambiguous
            at = s.find(lit, pos);
            if (at == std::string::npos) return false;
        }
        caps.push_back(s.substr(pos, at - pos));
        pos = at + lit.size();
    }
    return pc.size() > 1 || pos == s.size();
}

std::string translateCapture(const std::string& c, int depth) {
    auto& w = wordMap();
    if (auto it = w.find(c); it != w.end()) return it->second;
    if (depth > 0) return fromServerDepth(c, depth - 1);  // e.g. a kick reason inside "{} was kicked ({})"
    return c;
}

std::string lowerAscii(std::string s) {
    for (auto& ch : s) ch = char(std::tolower((unsigned char)ch));
    return s;
}

std::string savedPath() {
#ifdef _WIN32
    const char* d = std::getenv("APPDATA");
    return d ? std::string(d) + "\\eco_idioma.txt" : "eco_idioma.txt";
#else
    const char* d = std::getenv("HOME");
    return d ? std::string(d) + "/.eco_idioma" : "eco_idioma.txt";
#endif
}

}  // namespace

std::atomic<Lang>& current() {
    static std::atomic<Lang> l{Lang::Es};
    return l;
}

void set(Lang l) { current().store(l); }

bool parse(const std::string& v, Lang& out) {
    std::string s = lowerAscii(v);
    if (s.rfind("es", 0) == 0 || s == "español" || s == "espanol" || s == "spanish" || s == "castellano") {
        out = Lang::Es;
        return true;
    }
    if (s.rfind("en", 0) == 0 || s == "english" || s == "inglés" || s == "ingles") {
        out = Lang::En;
        return true;
    }
    return false;
}

const char* code(Lang l) { return l == Lang::En ? "en" : "es"; }
const char* name(Lang l) { return l == Lang::En ? "English" : "Español"; }

Lang systemLang() {
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_SPANISH) return Lang::Es;
    return id ? Lang::En : Lang::Es;
#else
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        Lang l;
        if (parse(v, l)) return l;
        // "C", "POSIX", "C.UTF-8": they say nothing; any other language (fr_FR, de_DE...): English
        std::string s = v;
        if (s == "C" || s == "POSIX" || s.rfind("C.", 0) == 0) break;
        return Lang::En;
    }
    return Lang::Es;  // no hints: the one Eco always had
#endif
}

bool loadSaved(Lang& out) {
    std::ifstream f(savedPath());
    std::string s;
    return f && std::getline(f, s) && parse(s, out);
}

void save(Lang l) {
    std::ofstream f(savedPath(), std::ios::trunc);
    if (f) f << code(l) << "\n";
}

Lang initial(bool useSaved) {
    Lang l;
    for (const char* var : {"ECO_IDIOMA", "ECO_LANG"})
        if (const char* v = std::getenv(var); v && parse(v, l)) return l;
    if (useSaved && loadSaved(l)) return l;
    return systemLang();
}

const char* toSpanish(const char* en) {
    if (!en) return en;
    auto& m = textMap();
    if (auto it = m.find(en); it != m.end()) return it->second;
    const char* id = std::strstr(en, "##");
    if (!id || id == en) return en;
    // "Text##id": the text is translated and the identifier kept. It is stored so the same pointer always comes back.
    static std::mutex mu;
    static std::unordered_map<std::string, std::string> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto [it, fresh] = cache.try_emplace(en);
    if (fresh) {
        std::string head(en, size_t(id - en));
        auto t = m.find(head);
        it->second = (t != m.end() ? std::string(t->second) : head) + id;
    }
    return it->second.c_str();
}

const char* T(const char* en) { return english() ? en : toSpanish(en); }

std::string fromServerDepth(const std::string& es, int depth) {
    auto& m = serverMap();
    if (auto it = m.find(es); it != m.end()) return it->second;
    std::vector<std::string> caps;
    for (auto& p : patterns()) {
        if (!match(p, es, caps)) continue;
        std::string out;
        size_t next = 0;
        for (const char* c = p.en; *c; ++c) {
            if (c[0] == '{' && c[1] == '}') {
                if (next < caps.size()) out += translateCapture(caps[next], depth);
                ++next;
                ++c;
            } else if (c[0] == '{' && c[1] >= '1' && c[1] <= '9' && c[2] == '}') {
                size_t k = size_t(c[1] - '1');
                if (k < caps.size()) out += translateCapture(caps[k], depth);
                c += 2;
            } else {
                out += *c;
            }
        }
        return out;
    }
    return es;
}

std::string fromServer(const std::string& es) { return english() ? fromServerDepth(es, 1) : es; }

}  // namespace eco::i18n
