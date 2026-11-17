// Eco's languages: English (the one in the code) and Spanish.
//
// - Texts are written in English and passed through T("..."): in Spanish it returns the translation from
//   common/i18n_es.inc (or the same text if it has none yet). T() always returns a pointer that lives until the
//   program ends, so it is fine for ImGui and for keeping it around.
// - What the server sends (notices, errors, kick reasons) always travels in Spanish, so that 2.x clients and
//   servers keep understanding each other. Each client translates it with fromServer(), which recognizes the
//   sentences with gaps ({}).
// - tools/check_translations.py reports the T("...") texts that have no Spanish translation.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace eco::i18n {

enum class Lang : uint8_t { Es = 0, En = 1 };

std::atomic<Lang>& current();
inline Lang lang() { return current().load(std::memory_order_relaxed); }
inline bool english() { return lang() == Lang::En; }
void set(Lang l);

// "es", "en", "español", "english", "es_ES.UTF-8", "en-US"... false if it is not recognized
bool parse(const std::string& v, Lang& out);
const char* code(Lang l);  // "es" / "en"
const char* name(Lang l);  // "Español" / "English"

// The system language (Windows: the UI language; elsewhere: LC_ALL, LC_MESSAGES or LANG). Not Spanish: English.
Lang systemLang();
// At startup: ECO_IDIOMA (or ECO_LANG) if set; otherwise the saved one (clients only, useSaved); otherwise the
// system one
Lang initial(bool useSaved);
// The one the user chose in the client (eco_idioma file, next to the other settings)
bool loadSaved(Lang& out);
void save(Lang l);

// Translation of a fixed text (in English) to the current language. Whatever follows "##" or "###" (ImGui
// identifiers) is kept as is.
const char* T(const char* en);
const char* toSpanish(const char* en);  // the translation, or en if there is none

// Server notices: "Cuenta borrada: Juan" -> "Account deleted: Juan" in English; unchanged in Spanish.
std::string fromServer(const std::string& es);

}  // namespace eco::i18n

namespace eco {
using i18n::T;
}
