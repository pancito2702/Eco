// The chat commands, for eco-gui's autocompletion and the Spanish names: name, what they take, what they do,
// who can use them and what to complete in their first argument. If you add a command in client.cpp, add it here too.
#pragma once
#include <string>

#include "../common/i18n.hpp"
#include "../common/protocol.hpp"

namespace eco {

enum class ArgKind { None, Nick, Channel };

struct CommandInfo {
    const char* name;  // without the slash; this is the name the code uses
    const char* es;    // the same command in Spanish (both can always be typed)
    const char* args;  // "<nick> <text>"
    const char* desc;
    Role role;         // from which role it can be used
    ArgKind first;     // what is completed in the first argument
};

inline const CommandInfo kCommands[] = {
    {"channels", "canales", "", "list channels and users", Role::User, ArgKind::None},
    {"join", "unir", "<channel>", "switch channel", Role::User, ArgKind::Channel},
    {"pm", "pm", "<nick> <text>", "private message (if they have an account and are away, they get it when they log in)", Role::User, ArgKind::Nick},
    {"history", "historial", "[n]", "latest messages in the channel (max. 200)", Role::User, ArgKind::None},
    {"pms", "privados", "[n]", "your saved private messages", Role::User, ArgKind::None},
    {"register", "registrar", "<password>", "create your account: reserves your name and keeps your role", Role::User, ArgKind::None},
    {"password", "clave", "<new>", "change your password", Role::User, ArgKind::None},
    {"verify", "verificar", "<nick>", "safety code to check that nobody is in the middle", Role::User, ArgKind::Nick},
    {"confirm", "confirmar", "<nick>", "mark them as verified (if their key changes, they get blocked)", Role::User, ArgKind::Nick},
    {"accept", "aceptar", "<nick>", "accept the new key of a blocked contact (after checking it)", Role::User, ArgKind::Nick},
    {"invite", "invitar", "", "invitation to this server to pass on to a friend", Role::User, ArgKind::None},
    {"e2e", "e2e", "<phrase> | off", "also encrypt with a phrase shared by the group", Role::User, ArgKind::None},
    {"mute", "mute", "", "mute your mic (again to unmute)", Role::User, ArgKind::None},
    {"deafen", "sordo", "", "stop hearing the others (again to come back)", Role::User, ArgKind::None},
    {"ptt", "ptt", "<key> | off", "push to talk (v, f8, ctrl, alt, mouse4...)", Role::User, ArgKind::None},
    {"threshold", "umbral", "<0-100>", "voice detection sensitivity (default 12)", Role::User, ArgKind::None},
    {"vol", "vol", "<0-200>", "output volume in %", Role::User, ArgKind::None},
    {"volume", "volumen", "<nick> <0-300>", "volume of one person in % (remembered)", Role::User, ArgKind::Nick},
    {"quality", "calidad", "<kbps>", "voice quality (8-128, default 40)", Role::User, ArgKind::None},
    {"noise", "ruido", "on | off", "remove background noise from the mic", Role::User, ArgKind::None},
    {"agc", "agc", "on | off", "automatic volume for your mic", Role::User, ArgKind::None},
    {"share", "compartir", "[fps] [width] [source]", "share your screen (or a window: see /sources)", Role::User, ArgKind::None},
    {"sources", "fuentes", "", "what can be shared: each screen and each open window", Role::User, ArgKind::None},
    {"stopshare", "parar", "", "stop sharing", Role::User, ArgKind::None},
    {"shareaudio", "audiopantalla", "on | off | apps", "also send what plays on the PC while sharing", Role::User, ArgKind::None},
    {"watch", "ver", "<nick>", "watch someone's screen", Role::User, ArgKind::Nick},
    {"unwatch", "dejar", "", "close the screen you are watching", Role::User, ArgKind::None},
    {"devices", "dispositivos", "", "list microphones and outputs", Role::User, ArgKind::None},
    {"mic", "mic", "[n | auto]", "choose microphone", Role::User, ArgKind::None},
    {"output", "salida", "[n | auto]", "choose output: speakers or headphones", Role::User, ArgKind::None},
    {"level", "nivel", "", "how much your mic is picking up right now", Role::User, ArgKind::None},
    {"ping", "ping", "", "ping to the server and lost voice", Role::User, ArgKind::None},
    {"stats", "stats", "", "network and encryption statistics", Role::User, ArgKind::None},
    {"version", "version", "", "Eco and server version", Role::User, ArgKind::None},
    {"update", "actualizar", "", "install the new version offered by the server", Role::User, ArgKind::None},
    {"language", "idioma", "es | en", "Eco's language: Spanish or English", Role::User, ArgKind::None},
    {"login", "login", "<password>", "become mod or admin with the server's password", Role::User, ArgKind::None},
    {"help", "ayuda", "", "list of commands", Role::User, ArgKind::None},
    {"quit", "salir", "", "close Eco", Role::User, ArgKind::None},
    // Moderation
    {"kick", "expulsar", "<nick> [reason]", "kick someone off the server", Role::Mod, ArgKind::Nick},
    {"move", "mover", "<nick> <channel>", "move someone to another channel", Role::Mod, ArgKind::Nick},
    {"silence", "silenciar", "<nick>", "so they can't be heard (persists even if they reconnect)", Role::Mod, ArgKind::Nick},
    {"unsilence", "desilenciar", "<nick>", "remove the mute", Role::Mod, ArgKind::Nick},
    {"createchannel", "crearcanal", "<name>", "create a channel", Role::Mod, ArgKind::None},
    // Administration
    {"ban", "banear", "<nick>", "ban their IP and their account (even if they are not connected)", Role::Admin, ArgKind::Nick},
    {"unban", "desbanear", "<ip or nick>", "remove a ban", Role::Admin, ArgKind::None},
    {"bans", "baneados", "", "banned IPs and accounts, and muted users", Role::Admin, ArgKind::None},
    {"deletechannel", "borrarcanal", "<name>", "delete a channel", Role::Admin, ArgKind::Channel},
    {"role", "rol", "<nick> admin|mod|user", "change someone's role", Role::Admin, ArgKind::Nick},
    {"accounts", "cuentas", "", "registered accounts", Role::Admin, ArgKind::None},
    {"deleteaccount", "borrarcuenta", "<nick>", "delete an account", Role::Admin, ArgKind::Nick},
    {"resetpassword", "resetclave", "<nick> <password>", "set another password for an account", Role::Admin, ArgKind::Nick},
};

// The name that is shown, depending on the language
inline const char* shownName(const CommandInfo& k) { return i18n::english() ? k.name : k.es; }

// "expulsar" -> "kick": commands typed in Spanish are handled as their English name
inline std::string canonicalCommand(const std::string& c) {
    for (auto& k : kCommands)
        if (c == k.name) return c;
    for (auto& k : kCommands)
        if (c == k.es) return k.name;
    return c;
}

// "/unban" or "/desbanear", depending on the language (to prefill the chat box)
inline std::string slashCommand(const char* name) {
    for (auto& k : kCommands)
        if (std::string(name) == k.name) return std::string("/") + shownName(k);
    return std::string("/") + name;
}

}  // namespace eco
