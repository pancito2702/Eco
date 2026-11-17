# Contributing to Eco

*(En español más abajo.)*

Thanks for wanting to help! Bug reports, ideas and pull requests are welcome.

## Before you start
- For a security problem, don't open an issue: see [SECURITY.md](SECURITY.md).
- For a big change (protocol, encryption, a new dependency), open an issue first to talk about it.

## Building and testing
See *Build* and *Tests* in the [README](README.md). Before sending a pull request, please check that:
- it builds on your platform (`cmake -S . -B build && cmake --build build`);
- `build/eco-test-crypto` passes, and `eco-test-flood` against a local server if you touched the server;
- `python tools/check_translations.py` reports nothing missing.

The GitHub Actions workflow builds and runs the tests on Linux and Windows for every pull request.

## Code style
- C++17. Code, comments and names in English; follow the style of the surrounding code (4 spaces, ~120 columns).
- **Every text the user sees goes through `T("...")`**, written in English, and gets its Spanish translation in
  `common/i18n_es.inc` (`X("English", "Español")`). `tools/check_translations.py` lists the missing ones.
- What the server sends to clients (notices, errors, kick reasons) stays in Spanish on the wire, for compatibility
  with 2.x clients: add its English translation as `S(...)` or, with gaps, `P(...)` in `common/i18n_es.inc`.
- A new chat command goes in `client/client.cpp` and in `client/commands.hpp` (English name, Spanish name,
  arguments and description).
- Changes to the protocol must stay compatible with clients and servers of the same major version, or bump
  `kProtoVersion` and the major version (see [CHANGELOG.md](CHANGELOG.md)).
- Never commit keys, databases (`*.db`) or executables: `.gitignore` already leaves them out.

By contributing you agree that your contribution is released under the [MIT license](LICENSE).

---

## Contribuir (en español)

¡Gracias por querer ayudar! Avisos de fallos, ideas y pull requests son bienvenidos.
- Un fallo de seguridad no va en un issue: mira [SECURITY.md](SECURITY.md).
- Antes de un cambio grande (protocolo, cifrado, una dependencia nueva), abre un issue para hablarlo.
- Antes de mandar un pull request: que compile, que pase `eco-test-crypto` (y `eco-test-flood` si tocaste el
  servidor) y que `python tools/check_translations.py` no avise de nada.
- El código, los comentarios y los nombres van en inglés. Todo texto que ve el usuario pasa por `T("...")`, en
  inglés, con su traducción al español en `common/i18n_es.inc`.
- Al contribuir aceptas que tu aportación se publique con la [licencia MIT](LICENSE).
