pragma Singleton
import QtQuick 2.15

/**
 * Enterprise Unified API Client for Local UI and Remote Web Frontends
 *
 * Automatically detects in-process execution via NvrBridge. When running inside
 * nvr_gui, requests execute directly against CommandDispatcher and PlaybackEngine
 * with zero network roundtrips and zero socket latency. Media playback URLs resolve
 * directly to local filesystem paths (file://) for instant kernel mmap playback.
 * When running remotely, falls back seamlessly to REST HTTP streaming.
 */
QtObject {
    id: client

    property string baseUrl: "http://127.0.0.1:8080/api/v1"
    property string currentUsername: "admin"
    property string currentUserRole: "ADMIN"
    property string authToken: ""
    property bool isAuthenticated: true

    Component.onCompleted: {
        if (isLocal() && typeof NvrBridge.isCaptureMode === "function" && NvrBridge.isCaptureMode()) {
            login("admin", "Admin@123");
        }
    }

    signal telemetryUpdated(var data)

    function isLocal() {
        return (typeof NvrBridge !== "undefined" && NvrBridge !== null && typeof NvrBridge.isLocal === "function" && NvrBridge.isLocal());
    }

    function parseQueryString(queryString) {
        var params = {};
        if (!queryString || queryString.length === 0) return params;
        var q = queryString;
        if (q.charAt(0) === '?') q = q.substr(1);
        var pairs = q.split('&');
        for (var i = 0; i < pairs.length; i++) {
            var pair = pairs[i].split('=');
            if (pair.length === 2) {
                var key = decodeURIComponent(pair[0]);
                var val = decodeURIComponent(pair[1]);
                if (val === "true") val = true;
                else if (val === "false") val = false;
                else if (/^-?\d+$/.test(val)) val = parseInt(val, 10);
                params[key] = val;
            }
        }
        return params;
    }

    function mapEndpointToAction(method, endpoint, body) {
        if (endpoint === "/dispatch" && body && body.action) {
            return { action: body.action, payload: body.payload || {} };
        }

        var path = endpoint;
        var query = "";
        var qIdx = endpoint.indexOf('?');
        if (qIdx >= 0) {
            path = endpoint.substring(0, qIdx);
            query = endpoint.substring(qIdx + 1);
        }
        var queryParams = parseQueryString(query);

        // System & Diagnostics
        if (path === "/system/status" && method === "GET") {
            return { action: "system.status", payload: {} };
        }
        if (path === "/diagnostics/trace" && method === "GET") {
            return { action: "diagnostics.trace", payload: {} };
        }

        // Cameras
        if (path === "/cameras") {
            if (method === "GET") return { action: "camera.list", payload: {} };
            if (method === "POST") return { action: "camera.add", payload: body || {} };
        }
        var camMatch = path.match(/^\/cameras\/(\d+)$/);
        if (camMatch) {
            var cId = parseInt(camMatch[1], 10);
            if (method === "PUT") {
                var b = body || {};
                b.id = cId;
                return { action: "camera.update", payload: b };
            }
            if (method === "DELETE") {
                return { action: "camera.delete", payload: { id: cId } };
            }
        }
        var camRestartMatch = path.match(/^\/cameras\/(\d+)\/restart$/);
        if (camRestartMatch) {
            return { action: "camera.restart", payload: { id: parseInt(camRestartMatch[1], 10) } };
        }
        var camRecStartMatch = path.match(/^\/cameras\/(\d+)\/record\/start$/);
        if (camRecStartMatch) {
            var pb = body || {};
            pb.channel_id = parseInt(camRecStartMatch[1], 10);
            return { action: "recording.start", payload: pb };
        }
        var camRecStopMatch = path.match(/^\/cameras\/(\d+)\/record\/stop$/);
        if (camRecStopMatch) {
            return { action: "recording.stop", payload: { channel_id: parseInt(camRecStopMatch[1], 10) } };
        }
        var camRecStatusMatch = path.match(/^\/cameras\/(\d+)\/record\/status$/);
        if (camRecStatusMatch) {
            return { action: "recording.status", payload: { channel_id: parseInt(camRecStatusMatch[1], 10) } };
        }

        // ONVIF
        if (path === "/onvif/discover" && method === "GET") {
            return { action: "onvif.discover", payload: {} };
        }
        if (path === "/onvif/probe" && method === "POST") {
            return { action: "onvif.probe", payload: body || {} };
        }
        if (path === "/onvif/encoder") {
            if (method === "GET") return { action: "onvif.encoder.get", payload: queryParams };
            if (method === "POST") return { action: "onvif.encoder.set", payload: body || {} };
        }
        if (path === "/onvif/reboot" && method === "POST") {
            return { action: "onvif.system.reboot", payload: body || {} };
        }
        if (path === "/onvif/datetime" && method === "POST") {
            return { action: "onvif.datetime.set", payload: body || {} };
        }

        // Recordings & Playback
        if (path === "/recordings/dates" && method === "GET") {
            return { action: "recordings.dates", payload: {} };
        }
        if (path === "/recordings" && method === "GET") {
            return { action: "recordings.query", payload: queryParams };
        }
        if (path === "/recordings/lock" && method === "POST") {
            return { action: "recordings.lock", payload: body || {} };
        }
        if (path === "/recordings/export" && method === "POST") {
            return { action: "recordings.export", payload: body || {} };
        }
        if (path === "/recordings/export/status" && method === "GET") {
            return { action: "recordings.export_status", payload: queryParams };
        }
        if (path === "/recordings/export/list" && method === "GET") {
            return { action: "recordings.export_list", payload: {} };
        }

        // Layout
        if (path === "/layout" && method === "POST") {
            return { action: "layout.set", payload: body || {} };
        }
        if (path === "/layout/fullscreen" && method === "POST") {
            return { action: "layout.fullscreen", payload: body || {} };
        }

        // Storage
        if ((path === "/storage" || path === "/storage/status") && method === "GET") {
            return { action: "storage.get", payload: {} };
        }
        if ((path === "/storage/path" || path === "/storage") && method === "POST") {
            return { action: "storage.set_path", payload: body || {} };
        }

        // Authentication & User Management
        if (path === "/auth/login" && method === "POST") {
            return { action: "auth.login", payload: body || {} };
        }
        if (path === "/auth/logout" && method === "POST") {
            return { action: "auth.logout", payload: body || {} };
        }
        if (path === "/auth/me" && method === "GET") {
            return { action: "auth.me", payload: {} };
        }
        if (path === "/users") {
            if (method === "GET") return { action: "user.list", payload: {} };
            if (method === "POST") return { action: "user.add", payload: body || {} };
        }
        var userDeleteMatch = path.match(/^\/users\/([^\/]+)$/);
        if (userDeleteMatch && method === "DELETE") {
            return { action: "user.delete", payload: { username: decodeURIComponent(userDeleteMatch[1]) } };
        }
        if ((path === "/users/change_password" || path === "/users/password") && method === "POST") {
            return { action: "user.change_password", payload: body || {} };
        }
        if (path === "/users/reset_password" && method === "POST") {
            return { action: "user.reset_password", payload: body || {} };
        }

        return null;
    }

    function request(method, endpoint, body, callback, errorCallback) {
        // Fast-path: native in-process dispatch via NvrBridge
        if (isLocal()) {
            var mapping = mapEndpointToAction(method, endpoint, body);
            if (mapping && mapping.action) {
                try {
                    var resStr = NvrBridge.dispatch(mapping.action, JSON.stringify(mapping.payload));
                    var resObj = JSON.parse(resStr);
                    if (resObj && resObj.status_code >= 200 && resObj.status_code < 300) {
                        // Merge top-level envelope fields with data payload for seamless caller compatibility
                        var resultData = resObj;
                        if (resObj.data !== undefined) {
                            if (typeof resObj.data === "object" && resObj.data !== null && !Array.isArray(resObj.data)) {
                                resultData = Object.assign({}, resObj.data, resObj);
                            } else {
                                resultData = resObj;
                            }
                        }
                        if (callback) callback(resultData);
                        return;
                    } else {
                        var errMsg = (resObj && (resObj.error || resObj.message)) ? (resObj.error || resObj.message) : ("Error " + (resObj ? resObj.status_code : ""));
                        if (errorCallback) errorCallback(errMsg);
                        return;
                    }
                } catch (e) {
                    console.warn("NvrBridge in-process dispatch error for " + mapping.action + ": " + e + ", falling back to HTTP");
                }
            }
        }

        // HTTP fallback for web remote access or unmapped routes
        var xhr = new XMLHttpRequest();
        var url = baseUrl + endpoint;
        xhr.open(method, url, true);
        xhr.setRequestHeader("Content-Type", "application/json");
        xhr.setRequestHeader("Accept", "application/json");
        if (authToken && authToken.length > 0) {
            xhr.setRequestHeader("Authorization", "Bearer " + authToken);
        }

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                var resp = null;
                try {
                    if (xhr.responseText && xhr.responseText.length > 0) {
                        resp = JSON.parse(xhr.responseText);
                    }
                } catch (e) {
                    resp = { raw: xhr.responseText };
                }

                if (xhr.status >= 200 && xhr.status < 300) {
                    if (callback) callback(resp);
                } else {
                    var errMsg = (resp && (resp.error || resp.message)) ? (resp.error || resp.message) : ("HTTP " + xhr.status);
                    if (errorCallback) errorCallback(errMsg);
                }
            }
        };

        if (body) {
            xhr.send(typeof body === "string" ? body : JSON.stringify(body));
        } else {
            xhr.send();
        }
    }

    // Generic REST Verbs
    function get(endpoint, callback, errorCallback) {
        request("GET", endpoint, null, callback, errorCallback);
    }

    function post(endpoint, body, callback, errorCallback) {
        request("POST", endpoint, body, callback, errorCallback);
    }

    function put(endpoint, body, callback, errorCallback) {
        request("PUT", endpoint, body, callback, errorCallback);
    }

    function del(endpoint, callback, errorCallback) {
        request("DELETE", endpoint, null, callback, errorCallback);
    }

    // Authentication Operations
    function login(username, password, callback, errorCallback) {
        post("/auth/login", { username: username, password: password }, function(res) {
            var data = (res && res.data) ? res.data : res;
            if (data && data.token) {
                client.authToken = data.token;
                client.currentUsername = data.username || username;
                client.currentUserRole = data.role || "OPERATOR";
                client.isAuthenticated = true;
                if (isLocal() && typeof NvrBridge.setAuthToken === "function") {
                    NvrBridge.setAuthToken(data.token);
                }
                if (callback) callback(data);
            } else {
                var msg = (res && (res.message || res.error)) ? (res.message || res.error) : "Invalid credentials";
                if (errorCallback) errorCallback(msg);
            }
        }, function(err) {
            if (errorCallback) errorCallback(err);
        });
    }

    function logout(callback) {
        post("/auth/logout", {}, function() {
            client.authToken = "";
            client.currentUsername = "";
            client.currentUserRole = "";
            client.isAuthenticated = false;
            if (isLocal() && typeof NvrBridge.setAuthToken === "function") {
                NvrBridge.setAuthToken("");
            }
            if (callback) callback();
        }, function() {
            client.authToken = "";
            client.currentUsername = "";
            client.currentUserRole = "";
            client.isAuthenticated = false;
            if (isLocal() && typeof NvrBridge.setAuthToken === "function") {
                NvrBridge.setAuthToken("");
            }
            if (callback) callback();
        });
    }

    // User Management Operations
    function getUsers(callback, errorCallback) {
        get("/users", function(res) {
            var list = Array.isArray(res) ? res : (Array.isArray(res.data) ? res.data : (res.users || []));
            if (callback) callback(list);
        }, errorCallback);
    }

    function addUser(userData, callback, errorCallback) {
        post("/users", userData, callback, errorCallback);
    }

    function deleteUser(username, callback, errorCallback) {
        del("/users/" + encodeURIComponent(username), callback, errorCallback);
    }

    function resetUserPassword(payload, callback, errorCallback) {
        post("/users/reset_password", payload, callback, errorCallback);
    }

    function changeUserPassword(payload, callback, errorCallback) {
        post("/users/change_password", payload, callback, errorCallback);
    }

    // Storage Management
    function getStorage(callback, errorCallback) {
        request("GET", "/storage", null, callback, errorCallback);
    }

    function setStoragePath(path, callback, errorCallback) {
        request("POST", "/storage/path", { path: path }, callback, errorCallback);
    }

    // System Telemetry
    function getSystemStatus(callback, errorCallback) {
        request("GET", "/system/status", null, function(res) {
            var data = (res && res.data) ? res.data : res;
            telemetryUpdated(data);
            if (callback) callback(data);
        }, errorCallback);
    }

    // Cameras
    function getCameras(callback, errorCallback) {
        request("GET", "/cameras", null, function(res) {
            var list = (res && res.cameras) ? res.cameras : ((res && res.data) ? res.data : []);
            if (callback) callback(list);
        }, errorCallback);
    }

    function addCamera(config, callback, errorCallback) {
        request("POST", "/cameras", config, callback, errorCallback);
    }

    function updateCamera(id, config, callback, errorCallback) {
        request("PUT", "/cameras/" + id, config, callback, errorCallback);
    }

    function restartCamera(id, callback, errorCallback) {
        request("POST", "/cameras/" + id + "/restart", {}, callback, errorCallback);
    }

    function deleteCamera(id, callback, errorCallback) {
        request("DELETE", "/cameras/" + id, null, callback, errorCallback);
    }

    function getSnapshotUrl(channelId) {
        if (isLocal()) {
            return "image://live/ch/" + channelId;
        }
        return baseUrl + "/cameras/" + channelId + "/snapshot";
    }

    // ONVIF Discovery & Video Encoder
    function discoverOnvif(timeoutSec, callback, errorCallback) {
        request("GET", "/onvif/discover", null, callback, errorCallback);
    }

    function probeOnvif(payload, callback, errorCallback) {
        request("POST", "/onvif/probe", payload, callback, errorCallback);
    }

    function getEncoder(params, callback, errorCallback) {
        var queryStr = "";
        if (params) {
            var parts = [];
            for (var k in params) {
                if (params.hasOwnProperty(k) && params[k] !== undefined && params[k] !== "") {
                    parts.push(encodeURIComponent(k) + "=" + encodeURIComponent(params[k]));
                }
            }
            if (parts.length > 0) queryStr = "?" + parts.join("&");
        }
        request("GET", "/onvif/encoder" + queryStr, null, callback, errorCallback);
    }

    function setEncoder(payload, callback, errorCallback) {
        request("POST", "/onvif/encoder", payload, callback, errorCallback);
    }

    function rebootCamera(payload, callback, errorCallback) {
        request("POST", "/onvif/reboot", payload, callback, errorCallback);
    }

    function setDateTime(payload, callback, errorCallback) {
        request("POST", "/onvif/datetime", payload, callback, errorCallback);
    }

    // Recording Controls & 24x7 Schedule
    function getSchedule(channelId, callback, errorCallback) {
        request("POST", "/dispatch", { action: "recording.get_schedule", payload: { channel_id: channelId } }, callback, errorCallback);
    }

    function setSchedule(payload, callback, errorCallback) {
        request("POST", "/dispatch", { action: "recording.set_schedule", payload: payload }, callback, errorCallback);
    }

    function startRecording(channelId, mode, callback, errorCallback) {
        request("POST", "/cameras/" + channelId + "/record/start", { channel_id: channelId, mode: mode || "continuous" }, callback, errorCallback);
    }

    function stopRecording(channelId, callback, errorCallback) {
        request("POST", "/cameras/" + channelId + "/record/stop", { channel_id: channelId }, callback, errorCallback);
    }

    function getRecordingStatus(channelId, callback, errorCallback) {
        request("GET", "/cameras/" + channelId + "/record/status", null, callback, errorCallback);
    }

    // Recordings & Playback
    function queryRecordingDates(callback, errorCallback) {
        request("GET", "/recordings/dates", null, function(res) {
            var dates = (res && res.data) ? res.data : (res && res.dates ? res.dates : []);
            if (callback) callback(dates);
        }, errorCallback);
    }

    function queryRecordings(filter, callback, errorCallback) {
        var queryStr = "";
        if (filter) {
            var parts = [];
            for (var key in filter) {
                if (filter.hasOwnProperty(key) && filter[key] !== undefined && filter[key] !== "") {
                    parts.push(encodeURIComponent(key) + "=" + encodeURIComponent(filter[key]));
                }
            }
            if (parts.length > 0) {
                queryStr = "?" + parts.join("&");
            }
        }
        request("GET", "/recordings" + queryStr, null, function(res) {
            var segs = (res && res.segments) ? res.segments : ((res && res.data && res.data.segments) ? res.data.segments : ((res && res.data) ? res.data : []));
            var reviews = (res && res.review_items) ? res.review_items : ((res && res.data && res.data.review_items) ? res.data.review_items : []);
            if (callback) callback(segs, reviews);
        }, errorCallback);
    }

    function lockRecording(segmentId, locked, filePath, callback, errorCallback) {
        var payload = { locked: locked };
        if (filePath) payload.path = filePath;
        if (segmentId) payload.id = segmentId;
        request("POST", "/recordings/lock", payload, callback, errorCallback);
    }

    function exportClip(payload, callback, errorCallback) {
        request("POST", "/recordings/export", payload, callback, errorCallback);
    }

    function getExportStatus(jobId, callback, errorCallback) {
        request("GET", "/recordings/export/status?job_id=" + encodeURIComponent(jobId), null, callback, errorCallback);
    }

    function listExportJobs(callback, errorCallback) {
        request("GET", "/recordings/export/list", null, callback, errorCallback);
    }

    function getPlaybackUrl(segment) {
        if (!segment) return "";
        // 1. Direct local file playback for in-process Qt UI (zero-latency kernel mmap)
        if (isLocal()) {
            var pathOrId = "";
            if (typeof segment === "object") {
                if (segment.file_path) pathOrId = segment.file_path;
                else if (segment.first_segment && segment.first_segment.file_path) pathOrId = segment.first_segment.file_path;
                else if (segment.id) pathOrId = segment.id;
            } else if (typeof segment === "string") {
                pathOrId = segment;
            }
            if (pathOrId.length > 0) {
                var localUrl = NvrBridge.getPlaybackUrl(pathOrId);
                if (localUrl && localUrl.length > 0) {
                    return localUrl;
                }
            }
        }

        // 2. HTTP streaming fallback for remote web clients
        if (typeof segment === "object") {
            if (segment.video_url) return baseUrl.replace("/api/v1", "") + segment.video_url;
            if (segment.file_path) return baseUrl + "/recordings/video?path=" + encodeURIComponent(segment.file_path);
            if (segment.first_segment && segment.first_segment.file_path) {
                return baseUrl + "/recordings/video?path=" + encodeURIComponent(segment.first_segment.file_path);
            }
            if (segment.id) return baseUrl + "/recordings/video?id=" + encodeURIComponent(segment.id);
        }
        return baseUrl + "/recordings/video?path=" + encodeURIComponent(segment);
    }

    function getThumbnailUrl(item) {
        if (!item) return "";
        if (typeof item === "object") {
            if (isLocal()) {
                if (item.thumbnail_url) {
                    var localThumb = NvrBridge.getLocalFilePath(item.thumbnail_url);
                    if (localThumb && localThumb.length > 0) return "file://" + localThumb;
                }
                if (item.file_path) {
                    var bmpPath = item.file_path.replace(/\.mp4$/, ".bmp");
                    var localBmp = NvrBridge.getLocalFilePath(bmpPath);
                    if (localBmp && localBmp.length > 0) return "file://" + localBmp;
                }
                if (item.channel_id) return "image://live/ch/" + item.channel_id;
            }
            if (item.thumbnail_url) return baseUrl.replace("/api/v1", "") + item.thumbnail_url;
            if (item.channel_id) return baseUrl + "/cameras/" + item.channel_id + "/snapshot";
        }
        return "";
    }

    // System Diagnostics
    function getDiagnostics(callback, errorCallback) {
        request("GET", "/diagnostics/trace", null, callback, errorCallback);
    }

    // Layout
    function setLayout(count, callback) {
        request("POST", "/layout", { count: count, layout_count: count }, callback);
    }

    function setFullscreen(channelId, callback) {
        request("POST", "/layout/fullscreen", { channel_id: channelId }, callback);
    }

    function exitFullscreen(callback) {
        request("POST", "/layout/fullscreen", { exit: true, enabled: false }, callback);
    }
}
