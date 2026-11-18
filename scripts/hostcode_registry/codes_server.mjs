#!/usr/bin/env node
// codes_server.mjs -- the host-code registry for CCCaster (and PovertyCaster): one small allocator of short codes
// that stand in for a host's IP address. Plain Node (18+) http, no dependencies, memory only.
//   node codes_server.mjs [--port 7720] [--host 127.0.0.1] [--ttl 60] [--trust-proxy] [--quiet]
//   node codes_server_check.mjs        (tests)
// The same file is tools/cccaster_codes/codes_server.mjs in mbaacc-decomp (the copy behind watch.meltyblood.org/hc)
// and scripts/hostcode_registry/codes_server.mjs in CCCaster (open source, anyone can run one); keep them identical.
//
// Codes: CCCaster codes are 4 characters, a letter first, then 3 letters or digits, without I / O / 0 / 1 (K7QX),
// case-insensitive, so a code is never mistaken for a port. PovertyCaster room codes stay 4 digits (1000-9999) until
// PovertyCaster takes the letter format (--pc-format letter4). The two formats never overlap: one code space.
//
//   GET /hc?action=host&proto=cc&port=P                  -> {code, secret, ttl}  a code for the caller's own address
//   GET /hc?action=host&proto=pc&room=R[&stream=pc_K]     -> {code, secret, ttl}  PovertyCaster room (HMAC-signed)
//   GET /hc?action=host&code=C&secret=S[...]              -> {code, ttl}          keep-alive (pc: signed, may set stream)
//   GET /hc?action=end&code=C&secret=S                    -> {ok:true}            (pc: signed)
//   GET /hc?action=resolve&code=C  -> {proto:'cc', addr} | {proto:'pc', room[, stream]}   live codes only, guarded
// A code lives --ttl seconds (60) after its create or last keep-alive; clients keep alive every 20 s, so a host that
// stops refreshing (crash, network loss) loses its code within a minute.
// Website only (requests with X-HC-Site: 1, which a website's own proxy adds; never expose them directly):
//   GET /hc?action=token&acct=A&name=N  -> {tok, ttl}   one use, 5 min: a site's Host button opens cccaster:host/<tok>
//   GET /hc?action=claim&tok=T&acct=A   -> {code} | {pending:true}   the code the game registered with that token
// There is no action that lists codes or addresses: an address is only ever returned by a guarded resolve of one code.
// The game passes tok=T on its create (binds the code to the account that asked) and st=w|m on keep-alives (waiting /
// in a match).
//
// Privacy: the cc host's address comes from the socket (or, with --trust-proxy behind your own proxy, from
// CF-Connecting-IP / X-Forwarded-For), is held in memory only, never logged or persisted, and only resolve returns
// it, to the asker. The log carries the action and status only. Per-code secrets and the PovertyCaster HMAC secret
// are compared in constant time and never logged. The HMAC secret is read from the env var HC_PC_HMAC_SECRET, or
// from the file named by HC_PC_HMAC_SECRET_FILE (outside any repo); without it proto=pc is off.
// The allocator sits behind a small interface (allocate / get / touch / release / sweep / size / countOwnedBy).
// resolve is guarded per source IP (like PovertyCaster's rendezvous JoinGuard): attempt bucket, escalating lockout
// after free misses, no hit/miss signal while locked, strikes forgotten only after quiet time, a hard miss cap per
// window. With 786432 codes and at most ~10 misses a minute per IP, guessing live codes is impractical.
import http from 'node:http';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';

// ---- code formats: the one place the format is defined ----
const LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ', ALNUM = LETTERS + '23456789';   // no I, O, 0, 1
export const CODE_FORMATS = {
    letter4: {   // CCCaster: K7QX (24 * 32^3 = 786432 codes)
        valid: (s) => /^[A-HJ-NP-Z][A-HJ-NP-Z2-9]{3}$/.test(s),
        random: () => LETTERS[crypto.randomInt(24)] + ALNUM[crypto.randomInt(32)] + ALNUM[crypto.randomInt(32)] + ALNUM[crypto.randomInt(32)],
        space: 24 * 32 * 32 * 32,
    },
    digits4: {   // PovertyCaster rooms until they move to letter4
        valid: (s) => /^\d{4}$/.test(s) && s[0] !== '0',
        random: () => String(1000 + crypto.randomInt(9000)),   // 1000-9999
        space: 9000,
    },
};
export const DEFAULT_FORMATS = { cc: 'letter4', pc: 'digits4' };
export function normalizeCode(s) { return typeof s === 'string' ? s.trim().toUpperCase() : ''; }

// ---- allocator interface (in-memory implementation for now) ----
// allocate(record) -> code | null (space exhausted; the format is the one of record.proto); get(code) -> record |
// undefined; touch(code, expiresAt); release(code); sweep(now) -> number dropped; size(); countOwnedBy(ip).
export function createMemoryAllocator({ formats = DEFAULT_FORMATS, maxTries = 64 } = {}) {
    const fmts = {}, used = {};
    for (const [proto, name] of Object.entries(formats)) {
        if (!CODE_FORMATS[name]) throw new Error('unknown code format ' + name);
        fmts[proto] = { name, ...CODE_FORMATS[name] };
        used[name] = 0;
    }
    const live = new Map();
    const drop = (code) => { const r = live.get(code); if (!r) return false; live.delete(code); used[fmts[r.proto].name]--; return true; };
    return {
        formats: { ...formats },
        valid: (c) => Object.values(fmts).some((f) => f.valid(c)),
        allocate(record) {
            const fmt = fmts[record.proto];
            if (!fmt) return null;
            if (used[fmt.name] >= fmt.space * 0.9) return null;     // keep the space sparse enough to stay random
            for (let i = 0; i < maxTries; i++) {
                const code = fmt.random();
                if (!live.has(code)) { live.set(code, { ...record, code }); used[fmt.name]++; return code; }
            }
            return null;
        },
        get: (code) => live.get(code),
        touch(code, expiresAt) { const r = live.get(code); if (r) r.expiresAt = expiresAt; },
        release: (code) => drop(code),
        sweep(now) { let n = 0; for (const [c, r] of live) if (now >= r.expiresAt) { drop(c); n++; } return n; },
        size: () => live.size,
        countOwnedBy(ip) { let n = 0; for (const r of live.values()) if (r.ownerIp === ip) n++; return n; },
    };
}

// ---- resolve guard: a JS port of pc_rendezvous' JoinGuard (targets/pc_rendezvous/JoinGuard.hpp) ----
export const DEFAULT_GUARD = {
    attemptsPerSec: 3.0, attemptBurst: 5.0,
    freeMisses: 3, backoffBaseMs: 1000, maxBackoffMs: 300000, missForgetMs: 600000,
    windowMs: 60000, maxMissesPerWindow: 10,
    maxTracked: 65536, idleDropMs: 900000,
};
export function createGuard(cfg = {}) {
    const c = { ...DEFAULT_GUARD, ...cfg };
    const map = new Map();
    const entry = (ip, now) => {
        let e = map.get(ip);
        if (!e) {
            if (map.size >= c.maxTracked) return null;              // never evict a live one; refuse unknown ips
            e = { tokens: 0, lastMs: 0, touched: 0, strikes: 0, lastMissMs: 0, windowStart: now, windowMisses: 0, lockedUntil: 0 };
            map.set(ip, e);
        }
        e.touched = now;
        return e;
    };
    return {
        // Call BEFORE looking the code up. {allow:true} or {allow:false, retryAfterMs}.
        admit(ip, now) {
            const e = entry(ip, now);
            if (!e) return { allow: false, retryAfterMs: 1000 };
            if (now < e.lockedUntil) return { allow: false, retryAfterMs: e.lockedUntil - now };
            e.tokens = e.lastMs === 0 ? c.attemptBurst
                : Math.min(c.attemptBurst, e.tokens + c.attemptsPerSec * (now - e.lastMs) / 1000);
            e.lastMs = now || 1;
            if (e.tokens < 1) return { allow: false, retryAfterMs: Math.floor((1 - e.tokens) / c.attemptsPerSec * 1000) + 1 };
            e.tokens -= 1;
            return { allow: true };
        },
        recordMiss(ip, now) {
            const e = entry(ip, now);
            if (!e) return;
            if (e.lastMissMs && now - e.lastMissMs >= c.missForgetMs) e.strikes = 0;
            e.lastMissMs = now || 1;
            e.strikes++;
            if (now - e.windowStart >= c.windowMs) { e.windowStart = now; e.windowMisses = 0; }
            e.windowMisses++;
            let lock = 0;
            if (e.strikes > c.freeMisses) {
                const shift = Math.min(e.strikes - c.freeMisses - 1, 20);
                lock = Math.min(c.backoffBaseMs * 2 ** shift, c.maxBackoffMs);
            }
            if (e.windowMisses >= c.maxMissesPerWindow) lock = Math.max(lock, e.windowStart + c.windowMs - now);
            if (lock) e.lockedUntil = Math.max(e.lockedUntil, now + lock);
        },
        recordHit() {},                                             // deliberately inert (see JoinGuard.hpp)
        sweep(now) { for (const [ip, e] of map) if (now >= e.lockedUntil && now - e.touched > c.idleDropMs) map.delete(ip); },
        tracked: () => map.size,
        strikes: (ip) => map.get(ip)?.strikes ?? 0,
    };
}

// ---- constant-time secret check ----
function secretEq(a, b) {
    if (typeof a !== 'string' || typeof b !== 'string') return false;
    const ha = crypto.createHash('sha256').update(a).digest();
    const hb = crypto.createHash('sha256').update(b).digest();
    return crypto.timingSafeEqual(ha, hb) && a.length === b.length;
}

// ---- PovertyCaster server-to-server auth (HMAC-SHA256, REGISTRY_SPEC.md "Auth") ----
// Header  X-HC-Auth: v1 ts=<unix seconds> nonce=<16-64 hex chars> sig=<64 hex chars>
// sig = hex(HMAC-SHA256(secret, METHOD + "\n" + PATH + "\n" + RAW_QUERY + "\n" + ts + "\n" + nonce))
// RAW_QUERY is the query exactly as sent, without the leading '?'. |now - ts| <= 60 s; a nonce is accepted once.
export function readPcSecret(env = process.env) {
    if (env.HC_PC_HMAC_SECRET) return env.HC_PC_HMAC_SECRET.trim();
    if (env.HC_PC_HMAC_SECRET_FILE) return fs.readFileSync(env.HC_PC_HMAC_SECRET_FILE, 'utf8').trim();
    return '';
}
export function signPc(secret, { method = 'GET', path = '/hc', rawQuery, ts, nonce }) {
    const sig = crypto.createHmac('sha256', secret).update(`${method}\n${path}\n${rawQuery}\n${ts}\n${nonce}`).digest('hex');
    return `v1 ts=${ts} nonce=${nonce} sig=${sig}`;
}
export function createPcAuth({ secret = '', skewSec = 60, clock = () => Date.now() } = {}) {
    const seen = new Map();   // nonce -> expiry ms
    return {
        enabled: !!secret,
        verify({ method = 'GET', path = '/hc', rawQuery = '', header = '' } = {}) {
            if (!secret) return { ok: false, status: 503, error: 'pc disabled' };
            const m = /^v1 ts=(\d{1,12}) nonce=([0-9a-f]{16,64}) sig=([0-9a-f]{64})$/.exec(String(header).trim());
            if (!m) return { ok: false, status: 401, error: 'unauthorized' };
            const now = clock(), ts = Number(m[1]);
            if (Math.abs(now / 1000 - ts) > skewSec) return { ok: false, status: 401, error: 'unauthorized', reason: 'skew' };
            const want = crypto.createHmac('sha256', secret).update(`${method}\n${path}\n${rawQuery}\n${m[1]}\n${m[2]}`).digest();
            const got = Buffer.from(m[3], 'hex');
            if (got.length !== want.length || !crypto.timingSafeEqual(got, want)) return { ok: false, status: 401, error: 'unauthorized', reason: 'sig' };
            for (const [n, exp] of seen) if (exp <= now) seen.delete(n);
            if (seen.has(m[2])) return { ok: false, status: 401, error: 'unauthorized', reason: 'replay' };
            seen.set(m[2], now + 2 * skewSec * 1000 + 1000);   // kept past the skew window, so a replay is never accepted
            return { ok: true };
        },
        nonces: () => seen.size,
    };
}

// ---- the registry (transport-free: handle(params, ip, now) -> {status, body}) ----
export function createRegistry({ allocator = createMemoryAllocator(), guard = createGuard(), ttlSec = 60,
                                 maxCodesPerIp = 4, maxPcCodes = 4000, pcAuth = createPcAuth(), clock = () => Date.now() } = {}) {
    const ttlMs = ttlSec * 1000;
    const stats = { host: 0, keepalive: 0, end: 0, resolveHit: 0, resolveMiss: 0, throttled: 0, bad: 0 };
    const bad = (error, status = 400) => { stats.bad++; return { status, body: { error } }; };
    const isV6 = (ip) => ip.includes(':');
    const ROOM = /^[A-Za-z0-9_-]{1,64}$/, STREAM = /^pc_[A-Za-z0-9_-]{1,64}$/;
    // req: {method, path, rawQuery, header} for the pc signature (the HTTP layer passes it; tests build it)
    const tokens = new Map();   // tok -> {acct, name, exp, code?}
    const TOK_MS = 5 * 60_000, NAME = /^[^\x00-\x1f<>]{1,40}$/;
    const pcCheck = (req) => { const v = pcAuth.verify(req); return v.ok ? null : { status: v.status, body: { error: v.error } }; };
    function handle(p, ip, req = {}) {
        const now = clock();
        allocator.sweep(now);
        const action = p.action;
        for (const [k, v] of tokens) if (now >= v.exp) tokens.delete(k);
        if (action === 'token' || action === 'claim') {
            if (!req.site) return bad('not found', 404);
            if (action === 'token') {
                if (!/^[\w-]{1,64}$/.test(p.acct || '') || !NAME.test(p.name || '')) return bad('bad account');
                if (tokens.size >= 10000) return bad('busy', 503);
                const tok = crypto.randomBytes(16).toString('hex');
                tokens.set(tok, { acct: p.acct, name: p.name, exp: now + TOK_MS });
                return { status: 200, body: { tok, ttl: TOK_MS / 1000 } };
            }
            const v = tokens.get(p.tok || '');                              // claim
            if (!v || v.acct !== p.acct) return bad('not found', 404);
            const r = v.code && allocator.get(v.code);
            return { status: 200, body: r ? { code: v.code } : { pending: true } };
        }
        if (action === 'host') {
            if (p.code !== undefined || p.secret !== undefined) {           // keep-alive
                const code = normalizeCode(p.code);
                const r = allocator.valid(code) ? allocator.get(code) : undefined;
                if (r && r.proto === 'pc') { const e = pcCheck(req); if (e) return e; }
                if (!r || !secretEq(p.secret, r.secret)) return bad('unknown code or wrong secret', 403);
                if (r.proto === 'pc' && p.stream !== undefined) {
                    if (p.stream !== '' && !STREAM.test(p.stream)) return bad('bad stream');
                    r.stream = p.stream || undefined;                      // empty clears it (stream ended)
                }
                if (p.st === 'w' || p.st === 'm') r.state = p.st;
                allocator.touch(code, now + ttlMs);
                stats.keepalive++;
                return { status: 200, body: { code, ttl: ttlSec } };
            }
            const proto = p.proto || 'cc';
            let record;
            if (proto === 'cc') {
                const port = Number(p.port);
                if (!/^\d{1,5}$/.test(p.port || '') || port < 1 || port > 65535) return bad('bad port');
                if (!ip) return bad('no source address');
                record = { proto, addr: isV6(ip) ? `[${ip}]:${port}` : `${ip}:${port}` };
            } else if (proto === 'pc') {
                const e = pcCheck(req); if (e) return e;
                if (!ROOM.test(p.room || '')) return bad('bad room');
                if (p.stream !== undefined && p.stream !== '' && !STREAM.test(p.stream)) return bad('bad stream');
                record = { proto, room: p.room, stream: p.stream || undefined };
            } else return bad('bad proto');
            if (allocator.countOwnedBy(ip) >= (proto === 'pc' ? maxPcCodes : maxCodesPerIp)) return bad('too many codes from this address', 429);
            const secret = crypto.randomBytes(16).toString('hex');
            // (from the site's Host button: the token names the account; a bad or used token still hosts, unnamed)
            const tv = p.tok ? tokens.get(p.tok) : undefined;
            if (tv && !tv.code) record.name = tv.name;
            const code = allocator.allocate({ ...record, secret, ownerIp: ip, expiresAt: now + ttlMs, createdAt: now });
            if (!code) return bad('no free codes', 503);
            if (tv && !tv.code) tv.code = code;
            stats.host++;
            return { status: 200, body: { code, secret, ttl: ttlSec } };
        }
        if (action === 'end') {
            const code = normalizeCode(p.code);
            const r = allocator.valid(code) ? allocator.get(code) : undefined;
            if (r && r.proto === 'pc') { const e = pcCheck(req); if (e) return e; }
            if (!r || !secretEq(p.secret, r.secret)) return bad('unknown code or wrong secret', 403);
            allocator.release(code);
            stats.end++;
            return { status: 200, body: { ok: true } };
        }
        if (action === 'resolve') {
            const v = guard.admit(ip, now);
            if (!v.allow) { stats.throttled++; return { status: 429, body: { error: 'throttled', retryAfterMs: v.retryAfterMs } }; }
            const code = normalizeCode(p.code);
            const r = allocator.valid(code) ? allocator.get(code) : undefined;
            if (!r) { guard.recordMiss(ip, now); stats.resolveMiss++; return { status: 404, body: { error: 'not_found' } }; }
            guard.recordHit(ip, now);
            stats.resolveHit++;
            if (r.proto === 'pc') return { status: 200, body: r.stream ? { proto: 'pc', room: r.room, stream: r.stream } : { proto: 'pc', room: r.room } };
            return { status: 200, body: { proto: 'cc', addr: r.addr } };
        }
        return bad('bad action');
    }
    return { handle, stats, allocator, guard, sweep: () => { allocator.sweep(clock()); guard.sweep(clock()); } };
}

// ---- HTTP front end ----
function sourceIp(req, trustProxy) {
    let ip = req.socket.remoteAddress || '';
    if (trustProxy) {
        const h = req.headers['cf-connecting-ip'] || (req.headers['x-forwarded-for'] || '').split(',')[0].trim();
        if (h) ip = h;
    }
    return ip.startsWith('::ffff:') ? ip.slice(7) : ip;
}
export function createServer({ registry = createRegistry(), path = '/hc', trustProxy = false, log = () => {} } = {}) {
    const server = http.createServer((req, res) => {
        const u = new URL(req.url, 'http://x');
        let out;
        if (req.method !== 'GET' || u.pathname !== path) out = { status: 404, body: { error: 'not found' } };
        else {
            const q = req.url.indexOf('?');
            out = registry.handle(Object.fromEntries(u.searchParams), sourceIp(req, trustProxy),
                                  { method: req.method, path: u.pathname, rawQuery: q < 0 ? '' : req.url.slice(q + 1),
                                    header: req.headers['x-hc-auth'] || '', site: req.headers['x-hc-site'] === '1' });
        }
        const body = JSON.stringify(out.body);
        res.writeHead(out.status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body),
                                    'Cache-Control': 'no-store', Connection: 'close' });
        res.end(body);
        log(`${u.searchParams.get('action') || '-'} ${out.status}`);   // action + status only: no codes, ips or secrets
    });
    const sweeper = setInterval(() => registry.sweep(), 5000);
    sweeper.unref();
    server.on('close', () => clearInterval(sweeper));
    return server;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
    const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
    const port = Number(arg('--port', '7720')), host = arg('--host', '127.0.0.1');
    const pcSecret = readPcSecret();
    const formats = { cc: arg('--cc-format', DEFAULT_FORMATS.cc), pc: arg('--pc-format', DEFAULT_FORMATS.pc) };
    const registry = createRegistry({ allocator: createMemoryAllocator({ formats }),
                                      ttlSec: Number(arg('--ttl', '60')), pcAuth: createPcAuth({ secret: pcSecret }) });
    console.log(`proto=pc ${pcSecret ? 'enabled' : 'disabled (no HC_PC_HMAC_SECRET / HC_PC_HMAC_SECRET_FILE)'}`);
    const quiet = process.argv.includes('--quiet');
    const server = createServer({ registry, trustProxy: process.argv.includes('--trust-proxy'),
                                  log: quiet ? () => {} : (m) => console.log(new Date().toISOString(), m) });
    server.listen(port, host, () => console.log(`codes_server listening on ${host}:${port}/hc`));
    setInterval(() => !quiet && console.log('stats', JSON.stringify(registry.stats), 'live', registry.allocator.size()), 60000).unref();
}
