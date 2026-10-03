# Security Review — Bullet Time

Reviewed: `src/c/test.c`, `src/pkjs/index.js`, `src/pkjs/config.js`,
`src/pkjs/config.html`, `src/pkjs/styles.css`, `package.json`, `wscript`,
`readme.md`. No hardcoded secrets are committed; `build/` is untracked.

## Threat model / trust assumptions

- **Phone (Pebble app + PKJS)** is trusted. Anything stored in PKJS
  `localStorage` (access/refresh token, password) is readable by the phone app
  and visible to `pebble logs`.
- **Config web page** runs in the phone app's webview on a hosted origin
  (`https://carunga.github.io/bullet-time/...`). It is trusted, but that origin
  is **shared** with every other GitHub Pages project of the same account.
- **Homeserver** is user-supplied and assumed honest for its own account; a
  malicious/`http` homeserver can capture tokens/passwords.
- **Watch ⇄ phone AppMessage** is trusted (paired link).

_Status: H1, H2, L2, L3, L6, M4 addressed (branch `security`). The rest are open._

## High

- [x] **H1 — Secrets in logs** (`src/pkjs/index.js`):
  `console.log("Settings saved", settings)` logs `access_token`,
  `refresh_token`, and (password auth) `user`/`pass`. Visible via `pebble logs`.
  → Log non-sensitive fields only (host, auth type, favourites count).
- [x] **H2 — No HTTPS enforcement** (`src/pkjs/config.js:32-39`,
  `src/pkjs/index.js:21-26`): `normalizeHost` accepts `http://`, so the bearer
  token and password can be sent in cleartext.
  → Reject non-`https` homeservers in both the config page and PKJS.

## Medium

- [ ] **M1 — Token in config URL fragment** (`index.js:839-844`,
  `config.js:53-62,537-543`): the access token is appended to the `openURL`
  fragment. Fragments aren't sent to servers, but they land in webview history
  and are readable by any same-origin JS.
- [ ] **M2 — Credentials on a shared origin** (`config.js:29`, `index.js:18`):
  settings (access/refresh token, and password for password auth) are stored in
  `carunga.github.io` `localStorage`, which is shared across all project pages
  of that account. Any other page there could read them.
- [ ] **M3 — Plaintext password at rest** (`config.js:157-158`): password auth
  stores `user`/`pass` in `localStorage` (browser and phone).
  → Drop the password after obtaining a token; prefer SSO.
- [x] **M4 — Wrong-room mis-delivery**: the watch now sends a room **index**
  (`ROOM_INDEX`) instead of a name; the phone resolves it against the ordered
  room list (index → room id), so duplicate names can no longer mis-deliver.
- [ ] **M5 — No end-to-end encryption**: only `m.room.message` plaintext is
  handled; encrypted rooms are unreadable and outgoing content is stored
  plaintext on the homeserver.
  → Decide stance: refuse encrypted rooms, or document the limitation.

## Low / hardening

- [ ] **L1** — `webviewclosed` (`index.js:847-858`) blindly trusts the config
  page's response to rewrite host/token (compounds M1/M2).
- [x] **L2** — `getSettings` `JSON.parse` wrapped in try/catch
  (`index.js`); a corrupted entry no longer breaks init.
- [x] **L3** — `getFragmentParams`/`getQueryParam` use a safe decoder
  (`config.js`); malformed encodings no longer throw.
- [ ] **L4** — No logout/revocation; tokens persist indefinitely.
  → Add "Log out" that revokes the token server-side.
- [ ] **L5** — Config page has no CSP (low; runs in the phone webview).
- [x] **L6** — `save()` only overwrites `access_token` when the new token is
  non-empty (`config.js`).

## C app — memory safety

Reviewed all buffers/indices. Every `strncpy`/`snprintf` is bounded and
null-terminated (`rooms[100][32]`, `messages[12][356]`, `senders[12][128]`,
`favourites[20][32]`, `view_message[356]`, `buffer[201]`, `type[16]`), writes
are guarded (`>= 100`, `>= 12`, `>= 20`), and all `dict_find` results are
null-checked. **No memory-safety issues found.** Minor: some tiny
`dict_write_*` calls ignore return values.

## Positives

- No hardcoded secrets; `build/` untracked.
- `encodeURIComponent` used for room ids/filter; DOM uses `textContent` (only
  `innerHTML = ""` to clear) → no DOM XSS from room/contact names.
- No `eval`/`document.write`/third-party scripts; config page loads only
  same-origin assets.
- HTTPS chosen by default.

## Remediation plan (prioritized)

1. **H1** remove secret logging (trivial).
2. **H2** enforce https.
3. **M1/M2** fix config hosting: give the config page its own origin, or stop
   passing the token in the fragment and have it establish its own session.
4. **M3** stop persisting passwords.
5. **M4** key rooms by id.
6. **L2/L3** harden parsing; **L4** add logout/revoke.
7. **M5** decide E2EE handling.

## Open decisions

- Config page origin: dedicated domain/host vs keep `carunga.github.io`.
- E2EE: send/read plaintext (documented) vs block encrypted rooms.
- Add logout/revoke?
