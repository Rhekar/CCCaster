# Host code registry

The small HTTP service behind CCCaster's host codes (`K7QX` instead of `IP:port`). CCCaster asks the registry in
`code_list.txt` (default `http://watch.meltyblood.org/hc`); put your own URL there to use your own registry.

```
node codes_server.mjs --port 7720 --host 0.0.0.0     # Node 18+, no dependencies
node codes_server_check.mjs                          # tests
```

Options: `--ttl 60` (seconds a code lives without a keep-alive), `--trust-proxy` (take the client address from
`CF-Connecting-IP` / `X-Forwarded-For`; only behind your own proxy), `--quiet`, `--cc-format letter4`.
CCCaster's HTTP client has no TLS, so the registry must answer plain `http://`.

## API (GET, JSON replies)

| request | reply |
|---|---|
| `?action=host&proto=cc&port=P` | `{"code":"K7QX","secret":"<32 hex>","ttl":60}`: a code for the **caller's own address** (taken from the connection; the client never sends one) |
| `?action=host&proto=cc&code=C&secret=S` | `{"code","ttl"}`: keep-alive (every 20 s); `403` once the code is gone, and the host asks for a new one |
| `?action=end&code=C&secret=S` | `{"ok":true}`: the code is gone at once |
| `?action=resolve&code=C` | `{"proto":"cc","addr":"ip:port"}`, `404` unknown, `429 {"retryAfterMs"}` throttled |

- Codes: a letter, then 3 letters or digits, no `I O 0 1`, any case (786432 codes). A code lives only while its host
  keeps it alive (60 s after the last keep-alive).
- **No list.** There is no request that returns more than one code or any address other than the one asked for.
- `resolve` is rate-limited per caller IP (burst 5, 3/s; after 3 misses each further miss locks the caller out for
  1 s, 2 s, 4 s ... up to 5 min; at most 10 misses a minute). While locked, every answer is the same `429`, so a
  guesser learns nothing.
- At most 4 live codes per address. Addresses, codes and secrets are kept in memory only and never logged.
- `proto=pc` (PovertyCaster rooms, off without `HC_PC_HMAC_SECRET`) and the `token` / `claim` actions (a website's
  "Host" button; answered only with an `X-HC-Site: 1` header) are optional; CCCaster itself doesn't need them.
