# Monocypher 4.0.3 (vendored, unmodified)

GC uses Monocypher's Ed25519 (RFC 8032, SHA-512) to check the signature on RotorLab's offline activation record (G89). GC calls one function for that, `crypto_ed25519_check`, through `custom/src/license/Ed25519.cc`. Monocypher's signing functions are used only by the unit tests.

## Source

| Item | Value |
|---|---|
| Release | Monocypher 4.0.3 (changelog date 2026-06-15) |
| Tarball | https://monocypher.org/download/monocypher-4.0.3.tar.gz (940390 bytes) |
| Tarball SHA-512 | `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965` (matches the published https://monocypher.org/download/monocypher-4.0.3.tar.gz.sha512) |
| Tarball SHA-256 | `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66` |
| Signature | Monocypher publishes no release signature. Instead, the files were cross-checked against GitHub tag `4.0.3` (https://github.com/LoupVaillant/Monocypher/tree/4.0.3), below. |
| Vendored | 2026-11-17, for GC 5.3.0 (G89) |
| License | Dual: BSD-2-Clause or CC0-1.0, at your choice (`LICENCE.md`, kept as shipped; also installed as `licenses/components/Monocypher-4.0.3-LICENCE.md.txt`) |

## Files

Copied byte for byte from the tarball; nothing edited. Hashes are of the files as committed (LF line endings).

| File here | From the tarball | SHA-256 |
|---|---|---|
| `monocypher.c` | `src/monocypher.c` | `57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be` |
| `monocypher.h` | `src/monocypher.h` | `c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | `60fce3578fb00b00da96490653d993c4cb427b1e1be38183285c66e04d22cc18` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | `abc4fad381879f5c29176ebe014b9189956b3dfe0a3e36459b6990bc57212380` |
| `LICENCE.md` | `LICENCE.md` | `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3` |

## Cross-check against GitHub tag 4.0.3

Read through the GitHub API (`repos/LoupVaillant/Monocypher/contents/<path>?ref=4.0.3`). Every file is identical to the tag except:

- each of the four source files: line 1 reads `// Monocypher version 4.0.3` in the tarball and `// Monocypher version __git__` at the tag. The release script stamps the version into that comment; no code differs.
- `LICENCE.md`: the tag adds a "Special notes" paragraph about `tests/externals/`, which the release tarball does not ship. Both licence texts are the same.

## Build

Built as its own C99 static library, `rlgc_monocypher` (custom/CMakeLists.txt), outside the main target so Qt's C++ precompiled headers never reach these C files. Warnings are silenced on that library only. No optimization or math flags are changed for it.

## Updating

1. Download the new release tarball into a new empty folder. Run nothing from it.
2. Check its SHA-512 against the published `.sha512` file and its files against the matching GitHub tag.
3. Replace the four source files and `LICENCE.md`. Update this README and `licenses/components/`.
4. Run `LicenseRecordTest` (RFC 8032 vectors, the RotorLab server fixture, the flipped-bit and non-canonical S vectors).

Do not go back to 4.0.2 or earlier: the download page warns that they can leak timing on public key signatures, depending on compiler settings.
