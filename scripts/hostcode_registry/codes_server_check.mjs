#!/usr/bin/env node
// codes_server_check.mjs -- tests for codes_server.mjs: allocator, keep-alive / expiry / end, the JoinGuard-style resolve
// guard (per-IP, escalating lockout, no signal while locked, quiet-time forgetting), the PovertyCaster key path, and the
// HTTP front end on loopback (source address from the socket, X-Forwarded-For ignored, no address / secret leaks).
//   node codes_server_check.mjs   -> prints PASS/FAIL per check, exit 1 on any failure
import http from 'node:http';
import crypto from 'node:crypto';
import { createRegistry, createMemoryAllocator, createGuard, createServer, createPcAuth, signPc, readPcSecret, CODE_FORMATS } from './codes_server.mjs';

let fails = 0, passes = 0;
const ok = (cond, name) => { if (cond) passes++; else { fails++; console.log('FAIL', name); } };
const section = (s) => console.log('--', s);

let t = 1_000_000;
const clock = () => t;
const mk = (o = {}) => createRegistry({ clock, ...o });
const A = '203.0.113.7', B = '198.51.100.9', C = '192.0.2.44';   // documentation ranges (never real hosts)

section('formats');
for (let i = 0; i < 2000; i++) { const c = CODE_FORMATS.digits4.random(); if (!CODE_FORMATS.digits4.valid(c)) { ok(false, 'digits4 random ' + c); break; } }
ok(CODE_FORMATS.digits4.valid('1000') && CODE_FORMATS.digits4.valid('9999'), 'digits4 bounds');
ok(!CODE_FORMATS.digits4.valid('0999') && !CODE_FORMATS.digits4.valid('12345') && !CODE_FORMATS.digits4.valid('12a4'), 'digits4 rejects');
ok(!CODE_FORMATS.digits4.valid('K7QX'), 'digits4 rejects letters');
{
    const L = CODE_FORMATS.letter4, seen = new Set();
    let bad = '';
    for (let i = 0; i < 20000; i++) { const c = L.random(); seen.add(c); if (!L.valid(c) || /[IO01]/.test(c) || !/^[A-Z]/.test(c)) { bad = c; break; } }
    ok(!bad, 'letter4 random: letter first, no I/O/0/1 ' + bad);
    ok(seen.size > 19000, 'letter4 random spreads (' + seen.size + ' distinct of 20000)');
    const used = new Set([...seen].join(''));
    ok(used.size === 32 && !used.has('I') && !used.has('O') && !used.has('0') && !used.has('1'), 'letter4 uses all 32 symbols');
    ok(L.valid('K7QX') && L.valid('AAAA') && L.valid('Z999') && L.valid('H2NP'), 'letter4 accepts');
    ok(!L.valid('7KQX') && !L.valid('K7Q') && !L.valid('K7QXA') && !L.valid('K7QI') && !L.valid('KOQX') && !L.valid('K0QX')
       && !L.valid('K1QX') && !L.valid('IAAA') && !L.valid('k7qx') && !L.valid('4821') && !L.valid('K7-X'), 'letter4 rejects');
    ok(L.space === 786432, 'letter4 space');
    ok(Object.keys(CODE_FORMATS).every((a) => Object.keys(CODE_FORMATS).every((b) => a === b
       || !CODE_FORMATS[a].valid(CODE_FORMATS[b].random()))), 'formats never overlap');
}

section('host / keep-alive / expiry / end');
{
    const r = mk();
    const h = r.handle({ action: 'host', port: '46318', proto: 'cc' }, A);
    ok(h.status === 200 && CODE_FORMATS.letter4.valid(h.body.code) && /^[0-9a-f]{32}$/.test(h.body.secret) && h.body.ttl === 60, 'host gives a letter4 code+secret+ttl');
    ok(!('addr' in h.body), 'host reply carries no address');
    const res = r.handle({ action: 'resolve', code: h.body.code }, B);
    ok(res.status === 200 && res.body.proto === 'cc' && res.body.addr === `${A}:46318`, 'resolve -> the host source address + port');
    ok(Object.keys(res.body).sort().join() === 'addr,proto', 'resolve body has proto+addr only (no secret)');
    ok(r.handle({ action: 'resolve', code: ' ' + h.body.code.toLowerCase() + ' ' }, B).status === 200, 'resolve is case-insensitive');
    ok(r.handle({ action: 'host', port: '46318', code: h.body.code.toLowerCase(), secret: h.body.secret }, A).status === 200, 'keep-alive is case-insensitive');
    const ip6 = r.handle({ action: 'host', port: '7000' }, '2001:db8::5');
    ok(r.handle({ action: 'resolve', code: ip6.body.code }, B).body.addr === '[2001:db8::5]:7000', 'IPv6 address bracketed');
    t += 50_000;
    const ka = r.handle({ action: 'host', port: '46318', code: h.body.code, secret: h.body.secret }, A);
    ok(ka.status === 200 && ka.body.code === h.body.code && !('secret' in ka.body), 'keep-alive ok, no secret echoed');
    ok(r.handle({ action: 'host', code: h.body.code, secret: 'f'.repeat(32) }, A).status === 403, 'keep-alive wrong secret refused');
    ok(r.handle({ action: 'host', code: h.body.code, secret: h.body.secret.slice(0, 31) }, A).status === 403, 'keep-alive short secret refused');
    t += 50_000;   // 100 s after host, 50 s after the keep-alive
    ok(r.handle({ action: 'resolve', code: h.body.code }, C).status === 200, 'alive 50 s after keep-alive');
    ok(r.handle({ action: 'resolve', code: ip6.body.code }, C).status === 404, 'not kept alive -> expired after 60 s');
    t += 10_001;
    ok(r.handle({ action: 'resolve', code: h.body.code }, C).status === 404, 'expired 60 s after the last keep-alive');
    ok(r.handle({ action: 'host', code: h.body.code, secret: h.body.secret }, A).status === 403, 'keep-alive of an expired code refused');
    const h2 = r.handle({ action: 'host', port: '7500' }, A);
    ok(r.handle({ action: 'end', code: h2.body.code, secret: 'x' }, A).status === 403, 'end with wrong secret refused');
    ok(r.handle({ action: 'end', code: h2.body.code, secret: h2.body.secret }, A).body.ok === true, 'end ok');
    ok(r.handle({ action: 'resolve', code: h2.body.code }, A).status === 404, 'ended code -> not_found');
    ok(r.handle({ action: 'host', port: '0' }, A).status === 400 && r.handle({ action: 'host', port: '70000' }, A).status === 400
       && r.handle({ action: 'host', port: '1;2' }, A).status === 400, 'bad ports refused');
    ok(r.handle({ action: 'host', port: '1', proto: 'zz' }, A).status === 400 && r.handle({ action: 'nope' }, A).status === 400, 'bad proto/action');
}

section('unique codes, per-IP cap, exhaustion');
{
    const r = mk({ maxCodesPerIp: 100000 });
    const seen = new Set();
    for (let i = 0; i < 3000; i++) seen.add(r.handle({ action: 'host', port: '1000' }, `10.0.${i >> 8}.${i & 255}`).body.code);
    ok(seen.size === 3000, 'codes unique among live ones (3000)');
    const r2 = mk({ maxCodesPerIp: 4 });
    for (let i = 0; i < 4; i++) r2.handle({ action: 'host', port: String(7000 + i) }, A);
    ok(r2.handle({ action: 'host', port: '7004' }, A).status === 429, 'per-IP code cap');
    ok(r2.handle({ action: 'host', port: '7004' }, B).status === 200, 'cap is per IP');
    const r3 = mk({ maxCodesPerIp: 1e9, allocator: createMemoryAllocator({ formats: { cc: 'digits4' } }) });
    let full = 0;
    for (let i = 0; i < 9000; i++) if (r3.handle({ action: 'host', port: '1000' }, A).status === 503) { full = i; break; }
    ok(full > 7000 && full <= 8100, 'exhaustion -> 503 at 90% (' + full + ')');
    ok(CODE_FORMATS.digits4.valid(mk({ allocator: createMemoryAllocator({ formats: { cc: 'digits4' } }) }).handle({ action: 'host', port: '1' }, B).body.code || ''), 'cc format configurable');
}

section('PovertyCaster: signed pc codes in the same code space');
{
    const secret = crypto.randomBytes(32).toString('hex');           // throwaway, generated per run
    const pcAuth = createPcAuth({ secret, clock });
    const r = mk({ pcAuth });
    let n = 0;
    const nonce = () => crypto.randomBytes(12).toString('hex') + String(n++).padStart(4, '0');
    const ts = () => Math.floor(t / 1000);
    const call = (q, ip = A, opt = {}) => {
        const rawQuery = new URLSearchParams(q).toString();
        const header = opt.header ?? signPc(opt.secret ?? secret, { rawQuery: opt.signQuery ?? rawQuery, ts: opt.ts ?? ts(), nonce: opt.nonce ?? nonce() });
        return { res: r.handle(q, ip, { method: 'GET', path: '/hc', rawQuery, header }), header };
    };
    const h = call({ action: 'host', proto: 'pc', room: 'r-9f2c', stream: 'pc_ab12' }).res;
    ok(h.status === 200 && /^\d{4}$/.test(h.body.code) && /^[0-9a-f]{32}$/.test(h.body.secret), 'signed pc host -> code + secret');
    const res = r.handle({ action: 'resolve', code: h.body.code }, B);
    ok(res.status === 200 && JSON.stringify(res.body) === JSON.stringify({ proto: 'pc', room: 'r-9f2c', stream: 'pc_ab12' }), 'pc resolve shape {proto, room, stream}');
    const cc = r.handle({ action: 'host', port: '46318' }, C);
    ok(JSON.stringify(Object.keys(r.handle({ action: 'resolve', code: cc.body.code }, B).body)) === '["proto","addr"]', 'cc resolve shape {proto, addr} (same space)');
    ok(cc.body.code !== h.body.code && CODE_FORMATS.letter4.valid(cc.body.code) && CODE_FORMATS.digits4.valid(h.body.code), 'pc (digits) and cc (letters) share one allocator');
    // keep-alive updates / clears the stream
    ok(call({ action: 'host', code: h.body.code, secret: h.body.secret, stream: '' }).res.status === 200, 'pc keep-alive (signed)');
    ok(JSON.stringify(r.handle({ action: 'resolve', code: h.body.code }, B).body) === JSON.stringify({ proto: 'pc', room: 'r-9f2c' }), 'stream cleared -> only room');
    call({ action: 'host', code: h.body.code, secret: h.body.secret, stream: 'pc_cd34' });
    ok(r.handle({ action: 'resolve', code: h.body.code }, B).body.stream === 'pc_cd34', 'keep-alive sets the stream');
    ok(call({ action: 'host', code: h.body.code, secret: h.body.secret, stream: 'nope' }).res.status === 400, 'bad stream refused');
    // bad signatures
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { secret: 'wrong' }).res.status === 401, 'wrong secret -> 401');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { signQuery: 'action=host&proto=pc&room=r3' }).res.status === 401, 'signature over another query -> 401');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { ts: ts() - 61 }).res.status === 401, 'timestamp 61 s old -> 401');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { ts: ts() + 61 }).res.status === 401, 'timestamp 61 s ahead -> 401');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { ts: ts() - 59 }).res.status === 200, 'timestamp 59 s old ok');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { header: '' }).res.status === 401, 'unsigned pc host -> 401');
    ok(call({ action: 'host', proto: 'pc', room: 'r2' }, A, { header: 'v1 ts=1 nonce=zz sig=00' }).res.status === 401, 'malformed header -> 401');
    // replay: the exact same signed request twice
    const q = { action: 'host', proto: 'pc', room: 'r5' }, rawQuery = new URLSearchParams(q).toString();
    const header = signPc(secret, { rawQuery, ts: ts(), nonce: nonce() });
    ok(r.handle(q, A, { rawQuery, header }).status === 200, 'first use of a signature ok');
    const rep = r.handle(q, A, { rawQuery, header });
    ok(rep.status === 401 && !('code' in rep.body), 'replayed signature -> 401');
    t += 30_000;
    ok(r.handle(q, A, { rawQuery, header }).status === 401, 'replay 30 s later still refused');
    // keep-alive / end of a pc code need the signature too (the per-code secret alone is not enough)
    ok(call({ action: 'host', code: h.body.code, secret: h.body.secret }, A, { header: '' }).res.status === 401, 'unsigned pc keep-alive -> 401');
    ok(call({ action: 'end', code: h.body.code, secret: h.body.secret }, A, { header: '' }).res.status === 401, 'unsigned pc end -> 401');
    ok(call({ action: 'end', code: h.body.code, secret: 'x'.repeat(32) }).res.status === 403, 'signed pc end, wrong code secret -> 403');
    ok(call({ action: 'end', code: h.body.code, secret: h.body.secret }).res.body.ok === true, 'signed pc end');
    ok(r.handle({ action: 'resolve', code: h.body.code }, B).status === 404, 'ended pc code -> not_found');
    ok(call({ action: 'host', proto: 'pc', room: 'bad room!' }).res.status === 400, 'bad room refused');
    // no secret configured: pc is off, cc unaffected
    const off = mk();
    ok(off.handle({ action: 'host', proto: 'pc', room: 'r' }, A, { header: 'v1 ts=1 nonce=00000000000000000 sig=' + '0'.repeat(64) }).status === 503, 'no HMAC secret -> pc disabled (503)');
    ok(off.handle({ action: 'host', port: '7000' }, A).status === 200, 'cc works without the pc secret');
    ok(readPcSecret({ HC_PC_HMAC_SECRET: ' s3cret \n' }) === 's3cret' && readPcSecret({}) === '', 'secret from HC_PC_HMAC_SECRET');
}

section('resolve guard (JoinGuard behaviour)');
{
    const r = mk();
    const live = r.handle({ action: 'host', port: '46318' }, A).body.code;
    let miss = 'AAAA'; while (miss === live) miss = 'AAAB';
    // 3 free misses (spaced to stay inside the attempt bucket)
    for (let i = 0; i < 3; i++) { ok(r.handle({ action: 'resolve', code: miss }, B).status === 404, 'free miss ' + i); t += 400; }
    ok(r.handle({ action: 'resolve', code: live }, B).status === 200, 'hit still allowed after free misses');
    t += 400;
    ok(r.handle({ action: 'resolve', code: miss }, B).status === 404, '4th miss reported, starts the lockout');
    const locked = r.handle({ action: 'resolve', code: live }, B);
    ok(locked.status === 429 && locked.body.error === 'throttled' && !('addr' in locked.body), 'locked: a LIVE code is refused');
    const lockedMiss = r.handle({ action: 'resolve', code: miss }, B);
    ok(JSON.stringify(Object.keys(lockedMiss.body)) === JSON.stringify(Object.keys(locked.body)) && lockedMiss.status === locked.status,
       'locked: a hit and a miss look the same (no signal)');
    ok(locked.body.retryAfterMs > 0 && locked.body.retryAfterMs <= 1000, 'first lockout ~1 s');
    ok(r.handle({ action: 'resolve', code: live }, C).status === 200, 'lockout is per source IP');
    // escalation: each further miss doubles the lockout
    let last = 0, escal = true;
    for (let k = 0; k < 5; k++) {
        t += 1000;      // wait each lockout out in small steps; the lockouts grow 2, 4, 8 s ...
        while (r.handle({ action: 'resolve', code: live }, B).status === 429) t += 250;
        r.handle({ action: 'resolve', code: miss }, B);
        const v = r.guard.admit(B, t);
        if (!(v.allow === false && v.retryAfterMs > last)) escal = false;
        last = v.retryAfterMs;
    }
    ok(escal && last >= 16000, 'lockout escalates (' + last + ' ms)');
    ok(r.guard.strikes(B) >= 9, 'strikes counted');
    // a hit does not clear strikes
    while (r.handle({ action: 'resolve', code: live }, B).status === 429) t += 1000;
    ok(r.guard.strikes(B) >= 9, 'a hit does not clear strikes');
    // quiet time forgets: 10 min without a miss, then the next miss is a free one again
    t += 600_001 + 300_000;
    ok(r.handle({ action: 'resolve', code: miss }, B).status === 404 && r.guard.strikes(B) === 1, 'strikes forgotten after quiet time');
    { const live2 = r.handle({ action: 'host', port: '46318' }, C).body.code; const x = r.handle({ action: 'resolve', code: live2 }, B); ok(x.status === 200, 'no lockout after the forgotten strikes ' + JSON.stringify(x.body)); }
    // attempt bucket: a burst of 5 then throttled even for hits
    const g = createGuard();
    let allowed = 0; for (let i = 0; i < 8; i++) if (g.admit('x', t).allow) allowed++;
    ok(allowed === 5, 'attempt burst of 5');
    ok(g.admit('x', t + 400).allow, 'bucket refills (3/s)');
    // hard miss cap per window: 10 misses in a minute lock to the window end even with a huge free allowance
    const g2 = createGuard({ freeMisses: 100, attemptBurst: 1000, attemptsPerSec: 1000 });
    const t0 = t;
    for (let i = 0; i < 10; i++) { g2.admit('y', t0 + i); g2.recordMiss('y', t0 + i); }
    const v2 = g2.admit('y', t0 + 20);
    ok(!v2.allow && v2.retryAfterMs > 59000, 'miss cap locks to the window end');
    // table cap: unknown IPs refused, never evicting a tracked one
    const g3 = createGuard({ maxTracked: 2 });
    g3.admit('a', t); g3.admit('b', t);
    ok(!g3.admit('c', t).allow && g3.admit('a', t + 1000).allow, 'table cap refuses unknown ips only');
}

section('website: token / claim; no list action');
{
    const r = mk(), S = { site: true };
    ok(r.handle({ action: 'token', acct: 'a1', name: 'Neco' }, A).status === 404 && r.handle({ action: 'claim', tok: 'x', acct: 'a1' }, A).status === 404, 'site actions refused without X-HC-Site');
    ok(r.handle({ action: 'list' }, A).status === 400 && r.handle({ action: 'list' }, A, S).status === 400, 'no list action at all (not even for the site)');
    ok(r.handle({ action: 'token', acct: 'a1', name: '<b>' }, A, S).status === 400, 'token: bad name refused');
    const tk = r.handle({ action: 'token', acct: 'a1', name: 'Neco#0001' }, A, S);
    ok(tk.status === 200 && /^[0-9a-f]{32}$/.test(tk.body.tok), 'token issued');
    ok(r.handle({ action: 'claim', tok: tk.body.tok, acct: 'a1' }, A, S).body.pending === true, 'claim pending before the game hosts');
    ok(r.handle({ action: 'claim', tok: tk.body.tok, acct: 'other' }, A, S).status === 404, 'claim by another account refused');
    const h = r.handle({ action: 'host', port: '46318', tok: tk.body.tok }, B);
    ok(h.status === 200, 'host with token');
    ok(r.handle({ action: 'claim', tok: tk.body.tok, acct: 'a1' }, A, S).body.code === h.body.code, 'claim gives the code');
    const h2 = r.handle({ action: 'host', port: '46319', tok: tk.body.tok }, C);
    ok(h2.status === 200, 'a used token still hosts');
    t += 1000;
    ok(r.handle({ action: 'host', code: h.body.code, secret: h.body.secret, st: 'm' }, B).status === 200, 'keep-alive with st');
    t += 61_000;
    ok(r.handle({ action: 'claim', tok: tk.body.tok, acct: 'a1' }, A, S).body.pending === true, 'claim of an expired code: pending again');
    t += 5 * 60_000;
    ok(r.handle({ action: 'claim', tok: tk.body.tok, acct: 'a1' }, A, S).status === 404, 'token expires');
}

section('HTTP front end (loopback)');
{
    const lines = [];
    const HTTP_SECRET = crypto.randomBytes(32).toString('hex');
    const registry = mk({ pcAuth: createPcAuth({ secret: HTTP_SECRET, clock }) });
    const server = createServer({ registry, log: (m) => lines.push(m) });
    await new Promise((res) => server.listen(0, '127.0.0.1', res));
    const port = server.address().port;
    const get = (q, headers = {}) => new Promise((res, rej) => {
        http.get({ host: '127.0.0.1', port, path: '/hc?' + q, headers }, (r) => {
            let d = ''; r.on('data', (c) => (d += c)); r.on('end', () => res({ status: r.statusCode, headers: r.headers, body: JSON.parse(d) }));
        }).on('error', rej);
    });
    const h = await get('action=host&port=46318&proto=cc', { 'X-Forwarded-For': '1.2.3.4' });
    ok(h.status === 200 && h.headers['content-length'] && h.headers.connection === 'close', 'HTTP host: Content-Length + Connection: close');
    const res = await get('action=resolve&code=' + h.body.code);
    ok(res.body.addr === '127.0.0.1:46318', 'HTTP: source address from the socket, X-Forwarded-For ignored');
    ok((await get('action=resolve&code=abc')).status === 404, 'HTTP: malformed code -> not_found');
    ok((await get('action=resolve&code=' + h.body.code.toLowerCase())).body.addr === '127.0.0.1:46318', 'HTTP: lowercase code resolves');
    ok((await get('action=list')).status === 400, 'HTTP: no list action');
    {
        const rawQuery = 'action=host&proto=pc&room=r-http&stream=pc_ff00';
        const pcH = await get(rawQuery, { 'X-HC-Auth': signPc(HTTP_SECRET, { rawQuery, ts: Math.floor(t / 1000), nonce: crypto.randomBytes(16).toString('hex') }) });
        ok(pcH.status === 200 && CODE_FORMATS.digits4.valid(pcH.body.code), 'HTTP: signed pc host over the wire');
        const pr = await get('action=resolve&code=' + pcH.body.code);
        ok(pr.body.room === 'r-http' && pr.body.stream === 'pc_ff00', 'HTTP: pc resolve');
        ok((await get('action=host&proto=pc&room=r-x', { 'X-HC-Auth': 'v1 ts=1 nonce=0000000000000000 sig=' + '0'.repeat(64) })).status === 401, 'HTTP: bad signature 401');
        lines.push('');   // (the header is never logged; checked below with the secret)
    }
    ok((await get('action=end&code=' + h.body.code + '&secret=' + h.body.secret)).body.ok === true, 'HTTP end');
    ok((await new Promise((r) => http.get({ host: '127.0.0.1', port, path: '/other' }, (x) => { x.resume(); r(x.statusCode); }))) === 404, 'other paths 404');
    const all = lines.join('\n');
    ok(!all.includes('127.0.0.1') && !all.includes(h.body.secret) && !all.includes(h.body.code) && !all.includes(HTTP_SECRET), 'server log: no address, secret or code');
    server.close();
}

console.log(`${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
