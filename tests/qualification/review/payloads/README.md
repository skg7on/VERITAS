# Registered corpus payloads

These raw files are the operator-registered concrete inputs for the
`lb-libuv-scoped-ipv6` case. They contain no executable code and no newline
terminator; the trusted harness reads the bytes, appends one NUL, and passes
the string to the captured `uv_inet_pton` entry point.

| File | Bytes | Meaning | SHA-256 |
| --- | ---: | --- | --- |
| `libuv_inet_pton_n.bin` | 20 | `::1.2.3.4.5.6.7.89%x`; scope-id prefix exactly 16 bytes | `15a2f92e63107dcabf30ea4b793a64ba85e0aa898b353d3b6b21d5a562f2d201` |
| `libuv_inet_pton_n_plus_1.bin` | 21 | `::1.2.3.4.5.6.7.8.9%x`; scope-id prefix 17 bytes | `de7dc2340a1cf510be44a905f0d799cf2277ec4773303d2faa238478594581cc` |
| `libuv_inet_pton_short.bin` | 9 | `::1.2.3%x`; short-source control | `8936fa7b62d9b20424f8ff26208da560de5366fa8d156ae85c2b7c2d077b5df0` |

The N+1 prefix is the first length that exceeds the base revision's
`char tmp[sizeof "255.255.255.255"]` (16 bytes). N must not produce a finding;
the short payload is refused before a matching execution because its readable
extent is shorter than the requested length.

The files have no native comment syntax, so the repository license notice lives
here: these are VERITAS-authored test inputs licensed under Apache-2.0, like
the rest of the VERITAS-authored corpus tooling. The pinned upstream libuv
artifacts remain MIT licensed and carry their own license digest in
`../corpus.json`.
