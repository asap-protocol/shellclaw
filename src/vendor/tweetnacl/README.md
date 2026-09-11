# TweetNaCl (vendored)

Upstream: [TweetNaCl](https://tweetnacl.cr.yp.to/) — public-domain C implementation of NaCl
primitives (including Ed25519 via `crypto_sign` / `crypto_sign_open`).

## Pinned version

| Field | Value |
|-------|-------|
| Release | **20140427** |
| Source URLs | `https://tweetnacl.cr.yp.to/20140427/tweetnacl.c`, `https://tweetnacl.cr.yp.to/20140427/tweetnacl.h` |
| Lines (`tweetnacl.c`) | 809 |

## SHA256 (vendored files)

| File | SHA256 |
|------|--------|
| `tweetnacl.c` | `02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4` |
| `tweetnacl.h` | `43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287` |

Verify after updates:

```bash
sha256sum src/vendor/tweetnacl/tweetnacl.c src/vendor/tweetnacl/tweetnacl.h
```

## License

TweetNaCl is placed in the **public domain** by the authors (Daniel J. Bernstein et al.).
See the [TweetNaCl paper](https://cryptojedi.org/papers/tweetnacl-20131229.pdf) and
[software page](https://tweetnacl.cr.yp.to/software.html). No copyright restrictions; copy
and integrate freely.

## Integration notes (ShellClaw)

- **Integer-only:** TweetNaCl uses fixed-width integer arithmetic only (no floating point).
  Suitable for edge builds without an FPU.
- **Ed25519 API:** `tweetnacl.h` maps `crypto_sign_ed25519`, `crypto_sign_ed25519_open`, and
  `crypto_sign_ed25519_keypair` to the `_tweet` symbols defined in `tweetnacl.c` (via include-time
  macros). ShellClaw `src/crypto/crypto.c` will call these in task 5.3.
- **`randombytes`:** Not defined in `tweetnacl.c`; the integrator must supply OS CSPRNG
  (ShellClaw: `crypto_read_urandom` in task 5.3).
- **Build:** `make test_tweetnacl_smoke` compiles with `TWEETNACL_CFLAGS` (project `CFLAGS` plus
  `-Wno-error=sign-compare` and `-Wno-error=unterminated-string-initialization` so unmodified
  upstream stays warning-clean under `CI=true` / `-Werror`).
- **UBSan left-shift-of-negative (`tweetnacl.c:281,685`):** the 20140427 pin uses signed shifts
  that trip UBSan under `-fsanitize=undefined`. This is expected upstream behavior; `-fwrapv` is
  included in `TWEETNACL_CFLAGS` (Makefile) to make signed overflow well-defined (wraparound),
  which silences the warnings and matches the algorithm's intent.
