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

_Status: H1, H2, M1–M4, L1, L2, L3, L4, L6 addressed (branch `security`).
Accepted: M5 (document in README), L5._

_Decision: the config page stays on the shared `carunga.github.io` origin (no
own hosting). Accordingly, no secrets are persisted there (see M1/M2/M3); the
residual risk is documented under "Posture"._

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

- [x] **M1 — Token in config URL fragment**: the token is still passed in the
  `openURL` fragment (never sent to a server), but the config page now strips it
  from the URL/webview history immediately after reading it
  (`history.replaceState`).
- [x] **M2 — Credentials on a shared origin**: the config page no longer writes
  tokens or passwords to `carunga.github.io` `localStorage`; only the homeserver
  and the favourites list are persisted. Secrets stored by older versions are
  scrubbed on load.
- [x] **M3 — Plaintext password at rest**: the config page no longer stores the
  password; it logs in, keeps the token in memory for the page's lifetime, and
  returns it to PKJS.
- [x] **M4 — Wrong-room mis-delivery**: the watch now sends a room **index**
  (`ROOM_INDEX`) instead of a name; the phone resolves it against the ordered
  room list (index → room id), so duplicate names can no longer mis-deliver.
- [ ] **M5 — No end-to-end encryption** (accepted): only `m.room.message`
  plaintext is handled; encrypted rooms are unreadable and outgoing content is
  stored plaintext. To be documented in the README (no code change).

## Low / hardening

- [x] **L1** — `webviewclosed` now validates the config response before storing
  it (drops a non-https hostserver, sanitizes favourites to `{id,name}`);
  it still assumes the config page itself is trusted.
- [x] **L2** — `getSettings` `JSON.parse` wrapped in try/catch
  (`index.js`); a corrupted entry no longer breaks init.
- [x] **L3** — `getFragmentParams`/`getQueryParam` use a safe decoder
  (`config.js`); malformed encodings no longer throw.
- [x] **L4** — "Log out" added to the config page: revokes the token
  (`POST /_matrix/client/v3/logout`) and returns tokenless settings to PKJS.
- [ ] **L5** — Config page has no CSP (accepted; low value — only same-origin
  assets, no inline scripts, runs in the phone webview).
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

## Posture / accepted residual risk

Decision: keep the config page on the shared `carunga.github.io` origin (no
dedicated hosting). Mitigations applied: no secrets are persisted in the
browser (only homeserver + favourites), the token fragment is removed from
history, and passwords are not stored.

Accepted residual risks:
- `carunga.github.io` is shared by all GitHub Pages projects of the account;
  another same-origin page could read the **non-secret** homeserver URL and
  favourites list, and could run `localStorage`-based attacks on the config page.
- The token is briefly present in the config webview URL (fragment) when
  Settings opens; it is not sent to any server.
- Whatever code is served at the Pages URL is trusted. A compromised
  account/repo could use or exfiltrate the token it receives. This is inherent
  to any hosted web client and cannot be removed without a dedicated origin.

## Open decisions

- Config page origin: dedicated domain/host vs keep `carunga.github.io`.
- E2EE: send/read plaintext (documented) vs block encrypted rooms.
- Add logout/revoke?
