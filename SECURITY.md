# Security policy

*(En español más abajo.)*

## Reporting a vulnerability

Please **don't open a public issue** for a security problem. Use GitHub's private reporting instead:
**Security > Report a vulnerability** in this repository. Include what you found, how to reproduce it and, if you
can, which version (`eco-server --version`, `eco-client --version`).

You'll get an answer as soon as possible. Once there is a fix, the report can be made public, with credit if you want.

## Scope

Everything in this repository: the server, the clients (console and GUI), the protocol, the end-to-end encryption,
the signed updates and the build scripts. Third-party libraries (`third_party/` and the ones downloaded at build
time) are only in scope regarding how Eco uses them; bugs in them should go to their own projects.

What is known and accepted is documented in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) (internal review, in Spanish)
and in the *Security* section of the [README](README.md): for example, the server sees metadata (who talks, when
and in which channel), and until you compare safety codes a malicious server could hand out fake keys.

## Supported versions

Only the latest version gets fixes. Each server owner builds and signs their own releases, so please update from
source.

---

## Avisar de un fallo de seguridad

Por favor, **no abras un issue público** para un problema de seguridad. Usa el aviso privado de GitHub:
**Security > Report a vulnerability** en este repositorio. Cuenta qué encontraste, cómo reproducirlo y, si puedes,
la versión (`eco-server --version`, `eco-client --version`).

Se contestará lo antes posible. Cuando esté arreglado, el aviso se puede hacer público, con tu nombre si quieres.
Lo que ya se conoce y se acepta está en [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) y en la sección *Seguridad* del
[README en español](README.es.md). Solo la última versión recibe arreglos.
