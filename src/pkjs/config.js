var SETTINGS_KEY = "matrix_settings";
var MAX_FAVOURITES = 20;
var ROOM_PAGE_SIZE = 10;

// Same filter the watch uses: room names plus the latest message for ordering.
var SYNC_FILTER = {
    event_fields: ["type", "content.name", "origin_server_ts", "state_key", "room_id"],
    room: {
        state: { types: ["m.room.name"] },
        timeline: { limit: 1, types: ["m.room.message"] },
        ephemeral: { not_types: ["*"] },
        account_data: { not_types: ["*"] }
    },
    presence: { not_types: ["*"] },
    account_data: { not_types: ["*"] }
};

function getStoredSettings() {
    var raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return {};
    try {
        return JSON.parse(raw) || {};
    } catch (e) {
        return {};
    }
}

// Only non-sensitive data is persisted on the (shared GitHub Pages) origin.
// Tokens and passwords are kept in memory only, never written to the browser.
function saveStoredSettings(value) {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify({
        hostserver: value.hostserver || "",
        favourites: value.favourites || []
    }));
}

function normalizeHost(host) {
    host = (host || "").trim();
    if (!host) return "";

    // Prepend https:// when no scheme is given.
    if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(host)) {
        host = "https://" + host;
    }

    // Only https homeservers are allowed (never send tokens in cleartext).
    if (!/^https:\/\//i.test(host)) {
        return "";
    }

    return host.replace(/\/+$/, "");
}

function configPageUrl() {
    var origin = window.location.origin ||
        (window.location.protocol + "//" + window.location.host);
    return origin + window.location.pathname;
}

function safeDecode(value) {
    try {
        return decodeURIComponent(value);
    } catch (err) {
        return value;
    }
}

function getQueryParam(name) {
    var match = new RegExp("[?&]" + name + "=([^&]*)").exec(window.location.search);
    if (!match) return null;
    return safeDecode(match[1].replace(/\+/g, " "));
}

function getFragmentParams() {
    var params = {};
    var hash = window.location.hash.replace(/^#/, "");
    hash.split("&").forEach(function (pair) {
        if (!pair) return;
        var parts = pair.split("=");
        params[safeDecode(parts[0])] = safeDecode(parts[1] || "");
    });
    return params;
}

function setStatus(message, isError) {
    var el = document.getElementById("status");
    if (!el) return;
    el.textContent = message || "";
    el.className = "status" + (isError ? " error" : "");
}

function returnToPebble(value) {
    saveStoredSettings(value);
    document.location = "pebblejs://close#" + encodeURIComponent(JSON.stringify(value));
}

// State

var settings = getStoredSettings();

// Scrub any secrets persisted by older versions on this shared origin.
saveStoredSettings(settings);

var currentHost = "";
var currentToken = "";
var allRooms = [];
var roomsShown = 0;
var favourites = (settings.favourites || []).slice(0, MAX_FAVOURITES);

function setAuth(host, response) {
    currentHost = normalizeHost(host);
    currentToken = response.access_token;

    settings.hostserver = currentHost;
    settings.access_token = response.access_token;
    if (response.refresh_token) settings.refresh_token = response.refresh_token;
    if (response.expires_in_ms) settings.expires_in_ms = response.expires_in_ms;
    if (response.user_id) settings.user_id = response.user_id;
    if (response.device_id) settings.device_id = response.device_id;

    saveStoredSettings(settings);
}

// Sign in

function exchangeLoginToken(host, token) {
    setStatus("Completing sign in...");

    var xhr = new XMLHttpRequest();
    xhr.open("POST", host + "/_matrix/client/v3/login");
    xhr.setRequestHeader("Content-Type", "application/json");

    xhr.onload = function () {
        var response;
        try {
            response = JSON.parse(xhr.responseText);
        } catch (e) {
            setStatus("Unexpected response from homeserver.", true);
            return;
        }

        if (xhr.status < 200 || xhr.status >= 300 || !response.access_token) {
            setStatus("SSO login failed: " + (response.error || xhr.status), true);
            return;
        }

        settings.auth = "sso";
        delete settings.user;
        delete settings.pass;
        setAuth(host, response);
        showFavourites();
    };

    xhr.onerror = function () {
        setStatus("Network error while completing sign in.", true);
    };

    xhr.send(JSON.stringify({ type: "m.login.token", token: token }));
}

function passwordLogin(host, user, pass) {
    setStatus("Signing in...");

    var xhr = new XMLHttpRequest();
    xhr.open("POST", host + "/_matrix/client/v3/login");
    xhr.setRequestHeader("Content-Type", "application/json");

    xhr.onload = function () {
        var response;
        try {
            response = JSON.parse(xhr.responseText);
        } catch (e) {
            response = {};
        }

        if (xhr.status < 200 || xhr.status >= 300 || !response.access_token) {
            setStatus("Login failed: " + (response.error || xhr.status), true);
            return;
        }

        settings.auth = "password";
        setAuth(host, response);
        showFavourites();
    };

    xhr.onerror = function () {
        setStatus("Network error while signing in.", true);
    };

    xhr.send(JSON.stringify({
        type: "m.login.password",
        identifier: { type: "m.id.user", user: user },
        password: pass
    }));
}

function renderProviders(host, providers) {
    setStatus("Choose a sign in provider:");

    var container = document.getElementById("providers");
    container.innerHTML = "";

    providers.forEach(function (provider) {
        var button = document.createElement("button");
        button.textContent = provider.name || provider.id;
        button.addEventListener("click", function () {
            startSso(host, provider.id);
        });
        container.appendChild(button);
    });
}

function startSso(host, providerId) {
    var redirectUrl = configPageUrl() + "?host=" + encodeURIComponent(host);
    var url = host + "/_matrix/client/v3/login/sso/redirect" +
        (providerId ? "/" + encodeURIComponent(providerId) : "") +
        "?redirectUrl=" + encodeURIComponent(redirectUrl) +
        "&action=login";

    setStatus("Redirecting to sign in...");
    window.location.href = url;
}

function discoverSsoProviders(host) {
    setStatus("Checking sign in options...");

    var xhr = new XMLHttpRequest();
    xhr.open("GET", host + "/_matrix/client/v3/login");

    xhr.onload = function () {
        var response;
        try {
            response = JSON.parse(xhr.responseText);
        } catch (e) {
            startSso(host, null);
            return;
        }

        var flows = response.flows || [];
        var ssoFlow = null;
        for (var i = 0; i < flows.length; i++) {
            if (flows[i].type === "m.login.sso") {
                ssoFlow = flows[i];
                break;
            }
        }

        if (!ssoFlow) {
            setStatus("This server does not support SSO. Use a username and password.", true);
            return;
        }

        var providers = ssoFlow.identity_providers || [];
        if (providers.length === 0) {
            startSso(host, null);
            return;
        }

        renderProviders(host, providers);
    };

    xhr.onerror = function () {
        setStatus("Could not reach homeserver.", true);
    };

    xhr.send();
}

// Favourites

function fetchRooms(callback) {
    var url = currentHost + "/_matrix/client/v3/sync?timeout=0&filter=" +
        encodeURIComponent(JSON.stringify(SYNC_FILTER));

    var xhr = new XMLHttpRequest();
    xhr.open("GET", url);
    xhr.setRequestHeader("Authorization", "Bearer " + currentToken);

    xhr.onload = function () {
        if (xhr.status === 401) {
            callback("Session expired");
            return;
        }
        if (xhr.status < 200 || xhr.status >= 300) {
            callback("Failed to load rooms (" + xhr.status + ")");
            return;
        }

        var response;
        try {
            response = JSON.parse(xhr.responseText);
        } catch (e) {
            callback("Unexpected response from homeserver");
            return;
        }

        var joined = (response.rooms || {}).join || {};
        var rooms = [];

        for (var id in joined) {
            var data = joined[id];
            var name = "(no name)";
            var lastTs = 0;

            var stateEvents = (data.state || {}).events || [];
            for (var i = 0; i < stateEvents.length; i++) {
                if (stateEvents[i].type === "m.room.name") {
                    name = (stateEvents[i].content || {}).name || "(no name)";
                }
            }

            var timelineEvents = (data.timeline || {}).events || [];
            for (var i = 0; i < timelineEvents.length; i++) {
                if (timelineEvents[i].type === "m.room.message") {
                    lastTs = timelineEvents[i].origin_server_ts || 0;
                }
            }

            rooms.push({ id: id, name: name, lastTs: lastTs });
        }

        rooms.sort(function (a, b) {
            return b.lastTs - a.lastTs;
        });

        callback(null, rooms);
    };

    xhr.onerror = function () {
        callback("Network error");
    };

    xhr.send();
}

function renderRoomList() {
    var container = document.getElementById("room-list");
    container.innerHTML = "";

    var favouriteIds = {};
    favourites.forEach(function (favourite) {
        favouriteIds[favourite.id] = true;
    });

    for (var i = 0; i < roomsShown && i < allRooms.length; i++) {
        var room = allRooms[i];

        var row = document.createElement("div");
        row.className = "room-row";

        var label = document.createElement("span");
        label.className = "room-name";
        label.textContent = room.name;
        row.appendChild(label);

        if (favouriteIds[room.id]) {
            var added = document.createElement("span");
            added.className = "added";
            added.textContent = "Added";
            row.appendChild(added);
        } else {
            var add = document.createElement("button");
            add.className = "small";
            add.textContent = "Add";
            add.addEventListener("click", (function (id, name) {
                return function () {
                    addFavourite(id, name);
                };
            })(room.id, room.name));
            row.appendChild(add);
        }

        container.appendChild(row);
    }

    document.getElementById("load-more").hidden = roomsShown >= allRooms.length;
}

function renderFavourites() {
    var container = document.getElementById("favourites");
    container.innerHTML = "";

    if (favourites.length === 0) {
        var empty = document.createElement("p");
        empty.className = "hint";
        empty.textContent = "No favourites yet. Add some from the list above.";
        container.appendChild(empty);
        return;
    }

    favourites.forEach(function (favourite, index) {
        var row = document.createElement("div");
        row.className = "fav-row";

        var label = document.createElement("span");
        label.className = "room-name";
        label.textContent = favourite.name;
        row.appendChild(label);

        var up = document.createElement("button");
        up.className = "small";
        up.textContent = "▲";
        up.disabled = index === 0;
        up.addEventListener("click", function () {
            moveFavourite(index, -1);
        });
        row.appendChild(up);

        var down = document.createElement("button");
        down.className = "small";
        down.textContent = "▼";
        down.disabled = index === favourites.length - 1;
        down.addEventListener("click", function () {
            moveFavourite(index, 1);
        });
        row.appendChild(down);

        var remove = document.createElement("button");
        remove.className = "small";
        remove.textContent = "✕";
        remove.addEventListener("click", function () {
            removeFavourite(index);
        });
        row.appendChild(remove);

        container.appendChild(row);
    });
}

function addFavourite(id, name) {
    if (favourites.length >= MAX_FAVOURITES) {
        setStatus("Maximum " + MAX_FAVOURITES + " favourites.", true);
        return;
    }

    for (var i = 0; i < favourites.length; i++) {
        if (favourites[i].id === id) return;
    }

    favourites.push({ id: id, name: name });
    renderRoomList();
    renderFavourites();
}

function removeFavourite(index) {
    favourites.splice(index, 1);
    renderRoomList();
    renderFavourites();
}

function moveFavourite(index, delta) {
    var target = index + delta;
    if (target < 0 || target >= favourites.length) return;

    var temp = favourites[index];
    favourites[index] = favourites[target];
    favourites[target] = temp;

    renderFavourites();
}

function showSignInHint() {
    updateAuthUi();

    var container = document.getElementById("room-list");
    container.innerHTML = "";

    var hint = document.createElement("p");
    hint.className = "hint";
    hint.textContent = "Sign in above to load your rooms.";
    container.appendChild(hint);

    document.getElementById("load-more").hidden = true;
}

function showFavourites() {
    updateAuthUi();
    setStatus("Loading rooms...");

    fetchRooms(function (error, rooms) {
        if (error) {
            setStatus(error + ". Please sign in to load rooms.", true);
            showSignInHint();
            return;
        }

        allRooms = rooms;
        roomsShown = Math.min(ROOM_PAGE_SIZE, allRooms.length);
        setStatus("");
        renderRoomList();
        renderFavourites();
    });
}

function save() {
    settings.hostserver = currentHost;
    if (currentToken) {
        settings.access_token = currentToken;
    }
    settings.favourites = favourites;
    returnToPebble(settings);
}

function updateAuthUi() {
    var el = document.getElementById("logout");
    if (el) el.hidden = !currentToken;
}

// Revoke the token server-side, then return tokenless settings to Pebble.
function logout() {
    function finish() {
        returnToPebble({ hostserver: currentHost, favourites: favourites });
    }

    if (!currentHost || !currentToken) {
        finish();
        return;
    }

    setStatus("Logging out...");

    var xhr = new XMLHttpRequest();
    xhr.open("POST", currentHost + "/_matrix/client/v3/logout");
    xhr.setRequestHeader("Authorization", "Bearer " + currentToken);
    xhr.onload = function () { finish(); };
    xhr.onerror = function () { finish(); };
    xhr.send();
}

// Wiring

var hostInput = document.getElementById("hostserver");
var userInput = document.getElementById("user");
var passInput = document.getElementById("pass");

hostInput.value = settings.hostserver || "";
userInput.value = settings.user || "";
passInput.value = settings.pass || "";

document.getElementById("sso").addEventListener("click", function () {
    var host = normalizeHost(hostInput.value);
    if (!host) {
        setStatus("Enter an https:// homeserver URL.", true);
        return;
    }
    hostInput.value = host;
    settings.hostserver = host;
    saveStoredSettings(settings);
    discoverSsoProviders(host);
});

document.getElementById("save-password").addEventListener("click", function () {
    var host = normalizeHost(hostInput.value);
    if (!host) {
        setStatus("Enter an https:// homeserver URL.", true);
        return;
    }
    hostInput.value = host;

    var user = userInput.value.trim();
    var pass = passInput.value.trim();
    if (!user || !pass) {
        setStatus("Enter your username and password.", true);
        return;
    }

    passwordLogin(host, user, pass);
});

document.getElementById("load-more").addEventListener("click", function () {
    roomsShown = Math.min(roomsShown + ROOM_PAGE_SIZE, allRooms.length);
    renderRoomList();
});

document.getElementById("save").addEventListener("click", save);
document.getElementById("logout").addEventListener("click", logout);

renderFavourites();

var loginToken = getQueryParam("loginToken");
var queryHost = getQueryParam("host");
var fragment = getFragmentParams();

// Don't leave the token fragment in the URL / webview history.
if (window.location.hash) {
    try {
        history.replaceState(null, "", window.location.pathname + window.location.search);
    } catch (e) {
    }
}

if (loginToken) {
    var ssoHost = normalizeHost(queryHost || settings.hostserver || "");
    if (!ssoHost) {
        setStatus("Missing homeserver for SSO sign in.", true);
        showSignInHint();
    } else {
        exchangeLoginToken(ssoHost, loginToken);
    }
} else if (fragment.token && fragment.host) {
    currentHost = normalizeHost(fragment.host);
    if (!currentHost) {
        setStatus("This homeserver must use https://.", true);
        showSignInHint();
    } else {
        currentToken = fragment.token;
        settings.hostserver = currentHost;
        settings.access_token = currentToken;
        saveStoredSettings(settings);
        showFavourites();
    }
} else if (settings.access_token && settings.hostserver) {
    currentHost = normalizeHost(settings.hostserver);
    if (!currentHost) {
        setStatus("This homeserver must use https://.", true);
        showSignInHint();
    } else {
        currentToken = settings.access_token;
        showFavourites();
    }
} else {
    showSignInHint();
}
