# Bullet Time (fork)

> **This repository is a fork** of
> [Finbear2/bullet-time](https://github.com/Finbear2/bullet-time) and lives at
> [Carunga/bullet-time](https://github.com/Carunga/bullet-time).
> See [CHANGES.md](CHANGES.md) for a branch-by-branch summary of what changed
> compared to the original repository.
>
> This README and the changes in this fork were made with the help of
> [opencode](https://opencode.ai).

Bullet Time is a small **Matrix client for Pebble watches**. It talks to a
Matrix homeserver directly from the phone's PebbleKit JS runtime — no companion
app required — and lets you read and send messages from the watch using voice
dictation.

## Features

- **Sign in with SSO** (or password) on any `https://` Matrix homeserver.
- **Favourite contacts** on the watch home menu; tap one to dictate a message.
- **Room list** sorted by recent activity, each showing its last-message time.
- **Continuous conversation view**: every message has a coloured header with
  sender + time; scroll down to load older messages.
- **Voice sending**: short-press Select in a conversation to reply; long-press
  Select on a room to dictate to it.
- **Watch-native look**: follows the system text size and uses the notification
  highlight colour.
- **Fast loads**: the room list is cached and synced incrementally.
- **Web configuration** from the Pebble phone app (homeserver, favourites,
  log out).

## Usage

1. Build the app with `pebble build` (output: `build/bullet-time-fork.pbw`) and
   install it on your watch.
2. Open the app's **Settings** from the Pebble phone app.
3. Enter your homeserver (must be `https://`), then sign in with **SSO** or a
   username/password.
4. Optionally pick **favourite contacts** and press **Save**.
5. On the watch:
   - choose a favourite, or **Latest messages** to browse rooms;
   - open a room to read the conversation;
   - **short-press Select** to dictate a reply;
   - **long-press Select** on a room to dictate to it.
6. Use **Log out** in Settings to revoke the session.

## Security limitations

- **No end-to-end encryption.** The app reads and writes unencrypted
  `m.room.message` events: encrypted rooms are not readable, and messages sent
  from the watch are stored as **plaintext** on the homeserver. Do not use it
  for content that requires E2EE.
- The access token is stored in the phone app's local storage. The config page
  keeps tokens in memory (never in browser storage) and can revoke them via
  **Log out**.
- See [security.md](security.md) for the full review and the accepted residual
  risks.
