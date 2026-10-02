var SETTINGS_KEY = "matrix_settings";

function getStoredSettings() {
    var raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return {};
    try {
        return JSON.parse(raw) || {};
    } catch (e) {
        return {};
    }
}

function saveStoredSettings(settings) {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
}

function normalizeHost(host) {
    host = (host || "").trim();
    if (!host) return "";
    if (!/^https?:\/\//i.test(host)) {
        host = "https://" + host;
    }
    return host.replace(/\/+$/, "");
}

function configPageUrl() {
    var origin = window.location.origin ||
        (window.location.protocol + "//" + window.location.host);
    return origin + window.location.pathname;
}

function getQueryParam(name) {
    var match = new RegExp("[?&]" + name + "=([^&]*)").exec(window.location.search);
    if (!match) return null;
    return decodeURIComponent(match[1].replace(/\+/g, " "));
}

function setStatus(message, isError) {
    var el = document.getElementById("status");
    if (!el) return;
    el.textContent = message || "";
    el.className = "status" + (isError ? " error" : "");
}

function returnToPebble(settings) {
    saveStoredSettings(settings);
    document.location = "pebblejs://close#" + encodeURIComponent(JSON.stringify(settings));
}

// Exchange the SSO loginToken returned by the homeserver for an access token.
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

        var settings = getStoredSettings();
        settings.hostserver = host;
        settings.access_token = response.access_token;
        settings.refresh_token = response.refresh_token || "";
        settings.expires_in_ms = response.expires_in_ms || 0;
        settings.user_id = response.user_id || "";
        settings.device_id = response.device_id || "";
        settings.auth = "sso";
        delete settings.user;
        delete settings.pass;

        returnToPebble(settings);
    };

    xhr.onerror = function () {
        setStatus("Network error while completing sign in.", true);
    };

    xhr.send(JSON.stringify({ type: "m.login.token", token: token }));
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

// Look up the homeserver's login flows and offer SSO providers if any exist.
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

var stored = getStoredSettings();

var hostInput = document.getElementById("hostserver");
var userInput = document.getElementById("user");
var passInput = document.getElementById("pass");

hostInput.value = stored.hostserver || "";
userInput.value = stored.user || "";
passInput.value = stored.pass || "";

var loginToken = getQueryParam("loginToken");
var queryHost = getQueryParam("host");

if (loginToken) {
    var ssoHost = normalizeHost(queryHost || stored.hostserver || "");
    if (!ssoHost) {
        setStatus("Missing homeserver for SSO sign in.", true);
    } else {
        exchangeLoginToken(ssoHost, loginToken);
    }
}

document.getElementById("sso").addEventListener("click", function () {
    var host = normalizeHost(hostInput.value);
    if (!host) {
        setStatus("Enter your homeserver URL first.", true);
        return;
    }
    hostInput.value = host;

    var settings = getStoredSettings();
    settings.hostserver = host;
    saveStoredSettings(settings);

    discoverSsoProviders(host);
});

document.getElementById("save").addEventListener("click", function () {
    var host = normalizeHost(hostInput.value);
    if (!host) {
        setStatus("Enter your homeserver URL first.", true);
        return;
    }

    var settings = {
        hostserver: host,
        user: userInput.value.trim(),
        pass: passInput.value.trim(),
        auth: "password"
    };

    returnToPebble(settings);
});
