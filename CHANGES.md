# Changes vs the original repository

This repository is a fork of
[Finbear2/bullet-time](https://github.com/Finbear2/bullet-time) (fork point:
`bef9291` "V1"). Work was done on separate feature branches; **`main` contains
all of them**.

## `sso` — Sign in with SSO

- Matrix SSO (`m.login.sso`) sign-in from the config page: provider picker,
  `loginToken` → `m.login.token` exchange, and persisted access/refresh tokens
  with automatic refresh.

## `quicker-loading` — Faster startup, smaller sync

- Shrank the initial `/sync` from ~13 MB to ~200 KB (typed filters +
  `event_fields`, excluding ephemeral/presence/account-data); the filter is
  registered server-side so the sync URL stays short.
- Room list cached for instant loads; fixed the stuck "Loading…" screen and a
  runaway room-send loop; trimmed noisy logging; removed hardcoded SDK paths.

## `favourites` — Favourite contacts

- Watch home menu: "Latest messages" plus favourite contacts.
- Config page: pick favourites from recent rooms (10 + Load more) and reorder
  them (up/down); returned to the watch via the settings channel.
- Selecting a favourite dictates and sends a message to that room.

## `better-looks` — UI polish + continuous conversation

- Paged room list (10 per page, "Load more"); loading screen with an optional
  cached view; slide-in animation.
- Follows the system text size; list highlight uses the notification colour.
- Room list shows last-message times.
- Opening a room goes straight into a **continuous conversation** (sender +
  time per message, a coloured header per message, scroll to load older);
  short/long-press Select dictation.

## `sync` — Incremental sync

- Persist the `/sync` `next_batch` token and merge deltas into the cached room
  list, so relaunches are near-instant.

## `security` — Hardening

- No secrets in logs; enforce `https://`; keep tokens/passwords out of browser
  storage on the shared GitHub Pages origin; strip the token from the URL
  fragment; validate config responses; add **Log out** (token revocation);
  address rooms by **index** (fixes wrong-room delivery with duplicate names).
- Added [security.md](security.md) (review + accepted residual risks).

## Other

- Added `.gitignore`; removed the committed `build/` artifacts from tracking.
- Added AppMessage keys for the new features (`ROOM_INDEX`, `TIME`, …).
