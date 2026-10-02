var syncData = {}


var currentRoom = ''
var authToken = ''

var ROOM_CACHE_KEY = "matrix_room_cache"


// Setting functions
function getSettings() {
    var s = localStorage.getItem("matrix_settings");
    if (!s) return null;
    return JSON.parse(s);
}

function saveSettings(settings) {
    localStorage.setItem("matrix_settings", JSON.stringify(settings));
}

function getHostServer() {
    var settings = getSettings();
    if (!settings) return null;
    var hostserver = settings['hostserver'];
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

function roomsToSyncData(rooms) {
    var result = {};
    for (var i = 0; i < rooms.length; i++) {
        result[rooms[i].name] = rooms[i];
    }
    return result;
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
        function (status) {
            if (status >= 200 && status < 300) {
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
    if (!hostserver) {
        console.log("Missing homeserver");
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

function getSyncData(token, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback(null);
        return;
    }

    var url = hostserver + "/_matrix/client/v3/sync?timeout=30000&filter=" +
        encodeURIComponent(JSON.stringify(SYNC_FILTER));

    httpRequest("GET", url, { "Authorization": "Bearer " + token }, null, 120000,
        function (status, text) {
            console.log("Sync status:", status);

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

            var ordered = [];
            var rooms = response["rooms"] || {};
            var joinedRooms = rooms["join"] || {};

            for (var roomId in joinedRooms) {
                var roomData = joinedRooms[roomId];

                var name = '(no name)';
                var lastTs = 0;

                var stateEvents = (roomData["state"] || {})["events"] || [];
                for (var i = 0; i < stateEvents.length; i++) {
                    if (stateEvents[i]["type"] === 'm.room.name') {
                        name = (stateEvents[i]["content"] || {})["name"] || '(no name)';
                    }
                }

                var timelineEvents = (roomData["timeline"] || {})["events"] || [];
                for (var i = 0; i < timelineEvents.length; i++) {
                    if (timelineEvents[i]["type"] === 'm.room.message') {
                        lastTs = timelineEvents[i]["origin_server_ts"] || 0;
                    }
                }

                ordered.push({
                    name: name,
                    id: roomId,
                    lastTs: lastTs
                });
            }

            ordered.sort(function (a, b) {
                return b.lastTs - a.lastTs;
            });

            saveRoomCache(ordered);
            syncData = roomsToSyncData(ordered);
            console.log("Rooms loaded:", ordered.length);

            callback(syncData);
        });
}

function matrixSendMessage(message) {
    var room = syncData[currentRoom];
    if (!room) return;

    var id = room["id"];

    var txnId = Date.now().toString();

    var hostserver = getHostServer();
    if (!hostserver) return;

    var url = hostserver + "/_matrix/client/v3/rooms/" +
        encodeURIComponent(id) + "/send/m.room.message/" + txnId;

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

// Send functions

function sendRooms(i) {
    var ids = Object.keys(syncData);

    if (i >= ids.length) return;

    var id = ids[i];

    Pebble.sendAppMessage(
        {'TYPE': 'ROOMS', 'ROOM_NAME': id},
        function() {
        },
        function(e) {
            console.log('Issue sending room: ', e);
        }
    );

    setTimeout( function() {
            sendRooms(i+1);
    }, 100);
}

function sendRoomList() {
    Pebble.sendAppMessage(
        {'TYPE': 'CLEAR_ROOMS'},
        function() {
            sendRooms(0);
        },
        function(e) {
            console.log('Issue clearing rooms: ', e);
            sendRooms(0);
        }
    );
}

function sendMessage(messages, i) {

    var ids = Object.keys(messages).sort(function(a, b) {
        return a - b;
    });

    if (i >= ids.length) return;

    var id = ids[i];
    var message = messages[id];

    Pebble.sendAppMessage(
        {
            'TYPE': 'MESSAGE',
            'SENDER': message["sender"] || '(no sender)',
            'TEXT': message["text"] || '(no content)'
        },
        function() {
            console.log('Sending message ', message["text"] || '(no content)')
        },
        function(e) {
            console.log('Error sending message ', e);
        }
    );

    setTimeout( function() {
        sendMessage(messages, i+1);
    }, 100);

}

function getRoomMessages(roomId, callback) {
    var hostserver = getHostServer();
    if (!hostserver) {
        callback([]);
        return;
    }

    var url = hostserver + "/_matrix/client/v3/rooms/" +
        encodeURIComponent(roomId) + "/messages?dir=b&limit=20";

    httpRequest("GET", url, { "Authorization": "Bearer " + authToken }, null, 30000,
        function (status, text) {
            if (status < 200 || status >= 300) {
                console.log("Failed to load messages:", status);
                callback([]);
                return;
            }

            var response;
            try {
                response = JSON.parse(text);
            } catch (err) {
                console.log("Failed to parse messages", err);
                callback([]);
                return;
            }

            callback(response["chunk"] || []);
        });
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

function sendMessages(room) {
    console.log('Checking messages for ', room);

    var roomData = syncData[room] || {};
    var id = roomData["id"];
    if (!id) return;

    var pending = 2;
    var events = [];
    var names = {};

    function finish() {
        pending--;
        if (pending > 0) return;

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

        sendMessage(messages, 0);
    }

    getRoomMessages(id, function (list) {
        events = list;
        finish();
    });

    getJoinedMembers(id, function (map) {
        names = map;
        finish();
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

function init() {
    login(function(token) {
        if (!token) {
            console.log('Login failed');
            sendNotConf();
            return;
        }

        console.log('Logged in');

        var cached = loadRoomCache();
        if (cached) {
            console.log('Rendering', cached.length, 'cached rooms');
            syncData = roomsToSyncData(cached);
            sendRoomList();
        }

        syncWithRetry(token, 0, !!cached);
    });
}

function syncWithRetry(token, attempt, hasCache) {
    getSyncData(token, function(data) {
        if (data && Object.keys(data).length > 0) {
            sendRoomList();
        } else if (attempt < 1) {
            console.log('Sync failed, retrying');
            setTimeout(function() {
                syncWithRetry(token, attempt + 1, hasCache);
            }, 2000);
        } else if (hasCache) {
            console.log('Sync failed, keeping cached rooms');
        } else {
            console.log('Sync failed after retries');
            sendNotConf();
        }
    });
}

Pebble.addEventListener('appmessage', function(e) {

    var type = e.payload.TYPE;

    if (type == 'ROOM_MESSAGES') {
        var room = e.payload.ROOM_NAME;
        currentRoom = room;
        sendMessages(room);
    } else if (type == 'SEND_MESSAGE') {
        var text = e.payload.TEXT;
        matrixSendMessage(text);
    }

});        

Pebble.addEventListener("showConfiguration", function() {
    console.log("Opening config page");
    Pebble.openURL("https://carunga.github.io/bullet-time/src/pkjs/config.html");
});

Pebble.addEventListener("webviewclosed", function(e) {
    if (!e.response) return;

    try {
        var settings = JSON.parse(decodeURIComponent(e.response));
        localStorage.setItem("matrix_settings", JSON.stringify(settings));
        localStorage.removeItem(ROOM_CACHE_KEY);
        console.log("Settings saved", settings);
        init();
    } catch (err) {
        console.log("Failed to parse settings", err);
    }
});
