var currentRoom = ''
var authToken = ''

var ROOM_CACHE_KEY = "matrix_room_cache"
var SYNC_TOKEN_KEY = "matrix_sync_token"


// Setting functions
function getSettings() {
    var s = localStorage.getItem("matrix_settings");
    if (!s) return null;
    try {
        return JSON.parse(s);
    } catch (err) {
        return null;
    }
}

function saveSettings(settings) {
    localStorage.setItem("matrix_settings", JSON.stringify(settings));
}

function isSecureHost(hostserver) {
    return !!hostserver && /^https:\/\//i.test(hostserver);
}

function getHostServer() {
    var settings = getSettings();
    if (!settings) return null;
    var hostserver = settings['hostserver'];
    if (!isSecureHost(hostserver)) return null;
    return hostserver;
}


// Room cache

function loadRoomCache() {
    var raw = localStorage.getItem(ROOM_CACHE_KEY);
    if (!raw) return null;
    try {
        var rooms = JSON.parse(raw);
        return (rooms && rooms.length) ? rooms : null;
    } catch (err) {
        return null;
    }
}

function saveRoomCache(rooms) {
    try {
        localStorage.setItem(ROOM_CACHE_KEY, JSON.stringify(rooms));
    } catch (err) {
        console.log("Failed to cache rooms", err);
    }
}

function loadSyncToken() {
    var token = localStorage.getItem(SYNC_TOKEN_KEY);
    return token || null;
}

function saveSyncToken(token) {
    if (token) {
        localStorage.setItem(SYNC_TOKEN_KEY, token);
    } else {
        localStorage.removeItem(SYNC_TOKEN_KEY);
    }
}

// Merge a sync response's rooms into the room map. Incremental syncs only
// carry changed rooms, so unchanged rooms keep their cached name/lastTs.
function mergeRooms(roomsById, joinedRooms, leaveRooms) {
    for (var joinedId in joinedRooms) {
        var data = joinedRooms[joinedId];
        var room = roomsById[joinedId] || { id: joinedId, name: '(no name)', lastTs: 0 };

        var stateEvents = (data['state'] || {})['events'] || [];
        for (var i = 0; i < stateEvents.length; i++) {
            if (stateEvents[i]['type'] === 'm.room.name') {
                room.name = (stateEvents[i]['content'] || {})['name'] || '(no name)';
            }
        }

        var timelineEvents = (data['timeline'] || {})['events'] || [];
        for (var i = 0; i < timelineEvents.length; i++) {
            if (timelineEvents[i]['type'] === 'm.room.message') {
                room.lastTs = timelineEvents[i]['origin_server_ts'] || room.lastTs;
            }
        }

        roomsById[joinedId] = room;
    }

    for (var leftId in leaveRooms) {
        delete roomsById[leftId];
    }
}


// HTTP helper. Uses onreadystatechange because the phone's PKJS XHR does not
// reliably fire onload/onerror for network errors.
function httpRequest(method, url, headers, body, timeoutMs, callback) {
    var xhr = new XMLHttpRequest();
    var finished = false;

    var timer = setTimeout(function () {
        console.log("Request timed out");
        finish(0, null);
    }, timeoutMs || 60000);

    function finish(status, text) {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        callback(status, text);
    }

    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        finish(xhr.status || 0, xhr.responseText);
    };

    xhr.open(method, url);

    if (headers) {
        for (var name in headers) {
            if (headers.hasOwnProperty(name)) {
                xhr.setRequestHeader(name, headers[name]);
            }
        }
    }

    xhr.send(body || null);
}


// Api functions

function passwordLogin(hostserver, user, pass, callback) {
    var data = {
        'type': 'm.login.password',
        'identifier': {
            'type': 'm.id.user',
            'user': user
        },
        'password': pass
    };

    httpRequest("POST", hostserver + "/_matrix/client/v3/login",
        { "Content-Type": "application/json" }, JSON.stringify(data), 60000,
        function (status, text) {
            var response = {};
            try {
                response = JSON.parse(text);
            } catch (err) {
                response = {};
            }

            var token = (status >= 200 && status < 300 && response.access_token)
                ? response.access_token : null;
            authToken = token;

            if (token && response.user_id) {
                var settings = getSettings() || {};
                settings['user_id'] = response.user_id;
                saveSettings(settings);
            }

            callback(token);
        });
}

// Exchange a stored refresh token for a fresh access token (used by SSO logins).
function refreshAccessToken(settings, hostserver, callback) {
    var refreshToken = settings['refresh_token'];
    if (!refreshToken) {
        callback(null);
        return;
    }

    httpRequest("POST", hostserver + "/_matrix/client/v3/refresh",
        { "Content-Type": "application/json" },
        JSON.stringify({ refresh_token: refreshToken }), 60000,
        function (status, text) {
            var response = {};
            try {
                response = JSON.parse(text);
            } catch (err) {
                response = {};
            }

            if (status >= 200 && status < 300 && response.access_token) {
                settings['access_token'] = response.access_token;
                if (response.refresh_token) settings['refresh_token'] = response.refresh_token;
                if (response.expires_in_ms) settings['expires_in_ms'] = response.expires_in_ms;
                saveSettings(settings);
                authToken = response.access_token;
                callback(response.access_token);
            } else {
                console.log("Token refresh failed", status);
                callback(null);
            }
        });
}

// Confirm the stored access token is still valid, refreshing it if needed.
function validateToken(settings, hostserver, callback) {
    httpRequest("GET", hostserver + "/_matrix/client/v3/account/whoami",
        { "Authorization": "Bearer " + settings['access_token'] }, null, 30000,
        function (status, text) {
            if (status >= 200 && status < 300) {
                try {
                    var response = JSON.parse(text);
                    if (response.user_id && settings['user_id'] !== response.user_id) {
                        settings['user_id'] = response.user_id;
                        saveSettings(settings);
                    }
                } catch (err) {
                }
                callback(settings['access_token']);
            } else if (status === 401) {
                refreshAccessToken(settings, hostserver, callback);
            } else {
                callback(settings['access_token']);
            }
        });
}

function login(callback) {

    var settings = getSettings();
    if (!settings) {
        callback(null);
        return;
    }

    var hostserver = settings['hostserver'];
    if (!isSecureHost(hostserver)) {
        console.log("Missing or non-https homeserver");
        callback(null);
        return;
    }

    // SSO logins persist an access token instead of a password.
    if (settings['access_token']) {
        authToken = settings['access_token'];
        validateToken(settings, hostserver, callback);
        return;
    }

    var user = settings['user'];
    var pass = settings['pass'];

    if (!user || !pass) {
        console.log("Missing config values");
        callback(null);
        return;
    }

    passwordLogin(hostserver, user, pass, callback);

}

// Only pull the room list (names + the latest message for ordering) up front;
// messages and display names are fetched per-room when opened.
var SYNC_FILTER = {
    event_fields: ["type", "content.name", "origin_server_ts", "state_key", "room_id"],
    room: {
        state: {
            types: ["m.room.name"]
        },
        timeline: {
            limit: 1,
            types: ["m.room.message"]
        },
        ephemeral: { not_types: ["*"] },
        account_data: { not_types: ["*"] }
    },
    presence: { not_types: ["*"] },
    account_data: { not_types: ["*"] }
};

// Register the filter server-side so the sync URL stays short. The phone's
// PKJS does not reliably send long query strings.
function registerFilter(token, userId, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback(null);
        return;
    }

    var url = hostserver + "/_matrix/client/v3/user/" +
        encodeURIComponent(userId) + "/filter";

    httpRequest("POST", url,
        {
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json"
        },
        JSON.stringify(SYNC_FILTER), 30000,
        function (status, text) {
            if (status >= 200 && status < 300) {
                try {
                    var response = JSON.parse(text);
                    if (response.filter_id) {
                        console.log("Filter registered:", response.filter_id);
                        callback(response.filter_id);
                        return;
                    }
                } catch (err) {
                }
            }
            console.log("Filter registration failed:", status);
            callback(null);
        });
}

function ensureFilter(token, callback) {
    var settings = getSettings() || {};
    var userId = settings['user_id'];

    if (userId) {
        registerFilter(token, userId, callback);
        return;
    }

    var hostserver = getHostServer();
    if (!hostserver) {
        callback(null);
        return;
    }

    httpRequest("GET", hostserver + "/_matrix/client/v3/account/whoami",
        { "Authorization": "Bearer " + token }, null, 30000,
        function (status, text) {
            if (status >= 200 && status < 300) {
                try {
                    var response = JSON.parse(text);
                    if (response.user_id) {
                        settings['user_id'] = response.user_id;
                        saveSettings(settings);
                        registerFilter(token, response.user_id, callback);
                        return;
                    }
                } catch (err) {
                }
            }
            callback(null);
        });
}

function getSyncData(token, filterId, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback(null);
        return;
    }

    // Reuse the cached room list + sync token for a fast incremental sync.
    var cached = loadRoomCache();
    var since = loadSyncToken();
    if (since && !cached) {
        since = null;
        saveSyncToken("");
    }

    var url = hostserver + "/_matrix/client/v3/sync?";
    url += since
        ? "since=" + encodeURIComponent(since) + "&timeout=0"
        : "timeout=30000";

    if (filterId) {
        url += "&filter=" + encodeURIComponent(filterId);
    } else {
        url += "&filter=" + encodeURIComponent(JSON.stringify(SYNC_FILTER));
    }

    httpRequest("GET", url, { "Authorization": "Bearer " + token }, null, 120000,
        function (status, text) {
            console.log("Sync status:", status, since ? "(incremental)" : "(full)");

            if ((status < 200 || status >= 300) && since) {
                // Stale/invalid since token: drop it and do a full sync.
                console.log("Incremental sync rejected, retrying full");
                saveSyncToken("");
                getSyncData(token, filterId, callback);
                return;
            }

            if (status < 200 || status >= 300) {
                console.log("Sync failed:", status);
                callback(null);
                return;
            }

            var response;
            try {
                response = JSON.parse(text);
            } catch (err) {
                console.log("Failed to parse sync response", err);
                callback(null);
                return;
            }

            if (response["next_batch"]) {
                saveSyncToken(response["next_batch"]);
            }

            var rooms = response["rooms"] || {};
            var joinedRooms = rooms["join"] || {};
            var leaveRooms = rooms["leave"] || {};

            var roomsById = {};
            if (since && cached) {
                for (var i = 0; i < cached.length; i++) {
                    roomsById[cached[i].id] = cached[i];
                }
            }

            mergeRooms(roomsById, joinedRooms, leaveRooms);

            var ordered = [];
            for (var id in roomsById) {
                ordered.push(roomsById[id]);
            }

            ordered.sort(function (a, b) {
                return b.lastTs - a.lastTs;
            });

            saveRoomCache(ordered);
            setRoomOrder(ordered, 'fresh');
            console.log("Rooms loaded:", ordered.length);

            callback(ordered);
        });
}

function sendMessageToRoom(roomId, message) {
    var hostserver = getHostServer();
    if (!hostserver || !roomId) return;

    var txnId = Date.now().toString();

    var url = hostserver + "/_matrix/client/v3/rooms/" +
        encodeURIComponent(roomId) + "/send/m.room.message/" + txnId;

    var data = {
        msgtype: "m.text",
        body: message
    };

    httpRequest("PUT", url,
        {
            "Authorization": "Bearer " + authToken,
            "Content-Type": "application/json"
        },
        JSON.stringify(data), 30000,
        function (status) {
            console.log("Message send status:", status);
        });
}

// Send to the room at a given index in the current (ordered) room list.
function sendMessageToIndex(index, message) {
    var entry = roomOrder[index];
    if (!entry || !entry.id) return;

    sendMessageToRoom(entry.id, message);
}

// Favourites

var MAX_FAVOURITES = 20;

function getFavourites() {
    var settings = getSettings();
    var favourites = settings && settings['favourites'];
    return (favourites && favourites.length) ? favourites : [];
}

function sendFavouriteItems(favourites, i) {
    if (i >= favourites.length) {
        Pebble.sendAppMessage(
            {'TYPE': 'FAVOURITES_DONE'},
            function() {},
            function(e) {
                console.log('Issue sending favourites done: ', e);
            }
        );
        return;
    }

    Pebble.sendAppMessage(
        {'TYPE': 'FAVOURITE', 'ROOM_NAME': favourites[i].name || favourites[i].id},
        function() {},
        function(e) {
            console.log('Issue sending favourite: ', e);
        }
    );

    setTimeout(function() {
        sendFavouriteItems(favourites, i + 1);
    }, 100);
}

function sendFavourites() {
    var favourites = getFavourites().slice(0, MAX_FAVOURITES);

    Pebble.sendAppMessage(
        {'TYPE': 'CLEAR_FAVOURITES'},
        function() {
            sendFavouriteItems(favourites, 0);
        },
        function(e) {
            console.log('Issue clearing favourites: ', e);
            sendFavouriteItems(favourites, 0);
        }
    );
}

function sendFavouriteMessage(index, text) {
    var favourites = getFavourites();
    var favourite = favourites[index];
    if (!favourite) {
        console.log('No favourite at index', index);
        return;
    }

    sendMessageToRoom(favourite.id, text);
}

// Send functions

// The watch shows rooms 10 at a time (max 100). Paging keeps AppMessage
// traffic small and lets the watch ask for more.
var MAX_ROOMS = 100;
var PAGE_SIZE = 10;
var roomOrder = [];
var roomPageOffset = 0;
var roomSource = 'fresh';
var pageSendInProgress = false;
var pendingPage = null;

function setRoomOrder(rooms, source) {
    roomOrder = rooms.slice(0, MAX_ROOMS);
    roomPageOffset = 0;
    roomSource = source;
}

function sendRoomsSequential(page, i, done) {
    if (i >= page.length) {
        if (done) done();
        return;
    }

    Pebble.sendAppMessage(
        {
            'TYPE': 'ROOMS',
            'ROOM_NAME': page[i].name,
            'TIME': Math.floor((page[i].lastTs || 0) / 1000)
        },
        function() {
        },
        function(e) {
            console.log('Issue sending room: ', e);
        }
    );

    setTimeout(function() {
        sendRoomsSequential(page, i + 1, done);
    }, 100);
}

function sendRoomPage(clear) {
    if (pageSendInProgress) {
        pendingPage = { clear: clear };
        return;
    }

    pageSendInProgress = true;

    var start = roomPageOffset;
    var page = roomOrder.slice(start, start + PAGE_SIZE);
    roomPageOffset = start + page.length;

    var fromCache = roomSource === 'cache' ? 1 : 0;
    var hasMore = roomPageOffset < roomOrder.length ? 1 : 0;

    function finish() {
        pageSendInProgress = false;

        if (pendingPage) {
            var pending = pendingPage;
            pendingPage = null;
            sendRoomPage(pending.clear);
        }
    }

    function afterClear() {
        sendRoomsSequential(page, 0, function() {
            Pebble.sendAppMessage(
                {'TYPE': 'ROOMS_DONE', 'HAS_MORE': hasMore, 'FROM_CACHE': fromCache},
                function() {
                    finish();
                },
                function(e) {
                    console.log('Issue sending page end: ', e);
                    finish();
                }
            );
        });
    }

    if (clear) {
        Pebble.sendAppMessage(
            {'TYPE': 'CLEAR_ROOMS'},
            function() {
                afterClear();
            },
            function(e) {
                console.log('Issue clearing rooms: ', e);
                afterClear();
            }
        );
    } else {
        afterClear();
    }
}

function showCachedRooms() {
    var cached = loadRoomCache();
    if (!cached) {
        console.log('No cached rooms to show');
        return;
    }

    console.log('Showing', cached.length, 'cached rooms');
    setRoomOrder(cached, 'cache');
    sendRoomPage(true);
}

function loadMoreRooms() {
    if (roomPageOffset >= roomOrder.length) return;
    sendRoomPage(false);
}

function sendMessage(messages, i) {

    // Newest first so the watch shows the latest message at the top.
    var ids = Object.keys(messages).sort(function(a, b) {
        return b - a;
    });

    if (i >= ids.length) return;

    var id = ids[i];
    var message = messages[id];

    Pebble.sendAppMessage(
        {
            'TYPE': 'MESSAGE',
            'SENDER': message["sender"] || '(no sender)',
            'TEXT': message["text"] || '(no content)',
            'TIME': Math.floor(parseInt(id, 10) / 1000)
        },
        function() {
        },
        function(e) {
            console.log('Error sending message ', e);
        }
    );

    setTimeout( function() {
        sendMessage(messages, i+1);
    }, 100);

}

var currentRoomId = '';
var currentRoomNames = {};
var roomPrevBatch = null;

function getRoomMessages(roomId, from, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback([], null);
        return;
    }

    var url = hostserver + "/_matrix/client/v3/rooms/" +
        encodeURIComponent(roomId) + "/messages?dir=b&limit=20";
    if (from) {
        url += "&from=" + encodeURIComponent(from);
    }

    httpRequest("GET", url, { "Authorization": "Bearer " + authToken }, null, 30000,
        function (status, text) {
            if (status < 200 || status >= 300) {
                console.log("Failed to load messages:", status);
                callback([], null);
                return;
            }

            var response;
            try {
                response = JSON.parse(text);
            } catch (err) {
                console.log("Failed to parse messages", err);
                callback([], null);
                return;
            }

            callback(response["chunk"] || [], response["end"] || null);
        });
}

function buildMessages(events, names) {
    var messages = {};

    for (var i = 0; i < events.length; i++) {
        var event = events[i];

        if (event["type"] !== 'm.room.message') continue;

        var content = event["content"] || {};
        var timeMili = event["origin_server_ts"] || 1000;

        var sender = event["sender"] || null;
        if (sender) {
            sender = names[sender] || sender;
        }

        messages[timeMili] = {
            'time': new Date(timeMili).toString(),
            'text': content["body"] || 'Error Getting Text',
            'sender': sender
        };
    }

    return messages;
}

function getJoinedMembers(roomId, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback({});
        return;
    }

    var url = hostserver + "/_matrix/client/v3/rooms/" +
        encodeURIComponent(roomId) + "/joined_members";

    httpRequest("GET", url, { "Authorization": "Bearer " + authToken }, null, 30000,
        function (status, text) {
            if (status < 200 || status >= 300) {
                callback({});
                return;
            }

            var response;
            try {
                response = JSON.parse(text);
            } catch (err) {
                callback({});
                return;
            }

            var joined = response["joined"] || {};
            var names = {};
            for (var userId in joined) {
                names[userId] = joined[userId]["display_name"] || userId;
            }
            callback(names);
        });
}

function openRoom(index) {
    var entry = roomOrder[index];
    if (!entry || !entry.id) return;

    var id = entry.id;
    console.log('Checking messages for ', entry.name);

    currentRoom = entry.name;
    currentRoomId = id;
    currentRoomNames = {};
    roomPrevBatch = null;

    var pending = 2;
    var events = [];
    var names = {};

    function finish() {
        pending--;
        if (pending > 0) return;

        currentRoomNames = names;
        sendMessage(buildMessages(events, names), 0);
    }

    getRoomMessages(id, null, function (list, end) {
        events = list;
        roomPrevBatch = end;
        finish();
    });

    getJoinedMembers(id, function (map) {
        names = map;
        finish();
    });
}

function loadOlder() {
    if (!currentRoomId) return;

    if (!roomPrevBatch) {
        Pebble.sendAppMessage(
            {'TYPE': 'NO_MORE'},
            function() {},
            function(e) { console.log('Issue sending no more: ', e); }
        );
        return;
    }

    var from = roomPrevBatch;
    roomPrevBatch = null;

    getRoomMessages(currentRoomId, from, function (list, end) {
        roomPrevBatch = end;
        sendMessage(buildMessages(list, currentRoomNames), 0);

        if (!end) {
            Pebble.sendAppMessage(
                {'TYPE': 'NO_MORE'},
                function() {},
                function(e) { console.log('Issue sending no more: ', e); }
            );
        }
    });
}


// Pebble Listeners

Pebble.addEventListener('ready', function(e) {
    console.log('PebbleKit JS ready!');

    if (!getSettings()) {
        console.log("Couldn't find conf");
        sendNotConf();
    } else {
        init();
    }
});

function sendNotConf() {
    Pebble.sendAppMessage(
        { 'TYPE': 'NOT_CONF' },
        function() {},
        function(e) {
            console.log('Issue sending configuration message to pebble ', e);
        }
    );
}

function sendCacheState() {
    var cached = loadRoomCache();

    Pebble.sendAppMessage(
        {'TYPE': 'CACHE_STATE', 'HAS_CACHE': cached ? 1 : 0},
        function() {},
        function(e) {
            console.log('Issue sending cache state: ', e);
        }
    );
}

function init() {
    sendFavourites();

    login(function(token) {
        if (!token) {
            console.log('Login failed');
            sendNotConf();
            return;
        }

        console.log('Logged in');

        sendCacheState();

        ensureFilter(token, function(filterId) {
            syncWithRetry(token, filterId, 0);
        });
    });
}

function syncWithRetry(token, filterId, attempt) {
    getSyncData(token, filterId, function(data) {
        if (data && Object.keys(data).length > 0) {
            sendRoomPage(true);
        } else if (attempt < 2) {
            console.log('Sync failed, retrying');
            setTimeout(function() {
                syncWithRetry(token, filterId, attempt + 1);
            }, 2000 * (attempt + 1));
        } else {
            console.log('Sync failed after retries');
        }
    });
}

Pebble.addEventListener('appmessage', function(e) {

    var type = e.payload.TYPE;

    if (type == 'ROOM_MESSAGES') {
        openRoom(e.payload.ROOM_INDEX);
    } else if (type == 'SEND_MESSAGE') {
        var text = e.payload.TEXT;
        if (typeof e.payload.ROOM_INDEX === 'number') {
            sendMessageToIndex(e.payload.ROOM_INDEX, text);
        } else if (currentRoomId) {
            sendMessageToRoom(currentRoomId, text);
        }
    } else if (type == 'LOAD_OLDER') {
        loadOlder();
    } else if (type == 'LOAD_MORE') {
        loadMoreRooms();
    } else if (type == 'SHOW_CACHE') {
        showCachedRooms();
    } else if (type && type.indexOf('SEND_FAV') === 0) {
        var favouriteIndex = parseInt(type.substring(8), 10);
        sendFavouriteMessage(favouriteIndex, e.payload.TEXT);
    }

});        

Pebble.addEventListener("showConfiguration", function() {
    console.log("Opening config page");

    var settings = getSettings() || {};
    var fragment = "";

    if (settings['access_token'] && settings['hostserver']) {
        fragment = "#token=" + encodeURIComponent(settings['access_token']) +
            "&host=" + encodeURIComponent(settings['hostserver']);
    }

    Pebble.openURL("https://carunga.github.io/bullet-time/src/pkjs/config.html" + fragment);
});

Pebble.addEventListener("webviewclosed", function(e) {
    if (!e.response) return;

    try {
        var settings = JSON.parse(decodeURIComponent(e.response));
        localStorage.setItem("matrix_settings", JSON.stringify(settings));
        localStorage.removeItem(ROOM_CACHE_KEY);
        localStorage.removeItem(SYNC_TOKEN_KEY);
        console.log("Settings saved", settings.hostserver, settings.auth,
                    (settings.favourites || []).length);
        init();
    } catch (err) {
        console.log("Failed to parse settings", err);
    }
});
