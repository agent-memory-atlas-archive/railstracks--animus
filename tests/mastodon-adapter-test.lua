-- ============================================================================
-- mastodon-adapter-test.lua — fixture test harness for scripts/mastodon.lua
-- Issue #135. Run with: lua mastodon-adapter-test.lua
--
-- Mocks the kernel contract (animus.http_get/post, config, json, log) the
-- same way LuaState provides them, loads the REAL adapter, and drives:
--   * registration shape (id, capabilities, actions, schema)
--   * poll: watermark dedupe, attribution fields (#133), no-watermark case,
--     non-mention filtering, error paths
--   * post/reply: chunk splitting at natural boundaries, thread chaining,
--     NO byte-trim (every chunk <= limit), fence preservation
--   * backoff: 429 handling, persisted state, expiry clear, Retry-After
--   * verify: credential failure loud
--
-- NO live account dependency: all HTTP calls hit a mock server table.
-- ============================================================================

local PASS, FAIL = 0, 0
local failures = {}

local function check(name, cond, detail)
    if cond then
        PASS = PASS + 1
        print("  ok - " .. name)
    else
        FAIL = FAIL + 1
        failures[#failures + 1] = name .. (detail and (" [" .. tostring(detail) .. "]") or "")
        print("  FAIL - " .. name .. (detail and (" [" .. tostring(detail) .. "]") or ""))
    end
end

-- ---------------------------------------------------------------------------
-- Mock kernel environment (mirrors LuaState globals)
-- ---------------------------------------------------------------------------

-- config store: plain table
local config_store = {}
config = {
    get = function(k) return config_store[k] end,
    set = function(k, v) config_store[k] = v end
}

-- json: minimal encode/decode (uses Lua table ctor + a tiny serializer).
-- For full fidelity we use a compact pure-Lua json; sufficient here since
-- fixtures are controlled.
json = {}
local function json_escape(s)
    return (tostring(s):gsub('[%c\\"\\]', {
        ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n',
        ['\r'] = '\\r', ['\t'] = '\\t'
    }))
end
local decode_pos, decode_str
local function decode_value()
    local s = decode_str
    local function skipws()
        while decode_pos <= #s and s:sub(decode_pos, decode_pos):match("%s") do
            decode_pos = decode_pos + 1
        end
    end
    skipws()
    local c = s:sub(decode_pos, decode_pos)
    if c == "{" then
        decode_pos = decode_pos + 1
        local t = {}
        skipws()
        if s:sub(decode_pos, decode_pos) == "}" then decode_pos = decode_pos + 1; return t end
        while true do
            skipws()
            local key = decode_value()
            skipws()
            decode_pos = decode_pos + 1 -- colon
            t[key] = decode_value()
            skipws()
            local d = s:sub(decode_pos, decode_pos)
            decode_pos = decode_pos + 1
            if d == "}" then break end
        end
        return t
    elseif c == "[" then
        decode_pos = decode_pos + 1
        local a = {}
        skipws()
        if s:sub(decode_pos, decode_pos) == "]" then decode_pos = decode_pos + 1; return a end
        while true do
            a[#a + 1] = decode_value()
            skipws()
            local d = s:sub(decode_pos, decode_pos)
            decode_pos = decode_pos + 1
            if d == "]" then break end
        end
        return a
    elseif c == '"' then
        decode_pos = decode_pos + 1
        local out = {}
        while true do
            local ch = s:sub(decode_pos, decode_pos)
            if ch == '"' then decode_pos = decode_pos + 1; break end
            if ch == "\\" then
                local nxt = s:sub(decode_pos + 1, decode_pos + 1)
                local esc = { n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\" }
                out[#out + 1] = esc[nxt] or nxt
                decode_pos = decode_pos + 2
            else
                out[#out + 1] = ch
                decode_pos = decode_pos + 1
            end
        end
        return table.concat(out)
    else
        -- number
        local num = s:match("^[%-%d%.eE+]+", decode_pos)
        if num then
            decode_pos = decode_pos + #num
            return tonumber(num)
        elseif s:sub(decode_pos, decode_pos + 3) == "null" then
            decode_pos = decode_pos + 4
            return nil -- NOTE: Lua table keys skip nil; null → absent (matches kernel json lib behavior for our use)
        elseif s:sub(decode_pos, decode_pos + 3) == "true" then
            decode_pos = decode_pos + 4
            return true
        elseif s:sub(decode_pos, decode_pos + 4) == "false" then
            decode_pos = decode_pos + 5
            return false
        end
        decode_pos = decode_pos + 1
        return nil
    end
end
function json.decode(s)
    decode_pos, decode_str = 1, s
    return decode_value()
end
function json.encode(v)
    if type(v) == "table" then
        -- Heuristic: array if all keys are sequential integers
        local is_array = true
        local n = 0
        for k in pairs(v) do
            if type(k) ~= "number" or k ~= n + 1 then is_array = false; break end
            n = n + 1
        end
        local parts = {}
        if is_array and n > 0 then
            for i = 1, n do parts[i] = json.encode(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        elseif next(v) == nil then
            return (n == 0) and "[]" or "{}"
        end
        for k, val in pairs(v) do
            parts[#parts + 1] = '"' .. json_escape(k) .. '":' .. json.encode(val)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    elseif type(v) == "string" then
        return '"' .. json_escape(v) .. '"'
    elseif type(v) == "number" then
        return string.format("%.14g", v)
    elseif type(v) == "boolean" then
        return tostring(v)
    else
        return "null"
    end
end

-- log: capture for assertions
local log_lines = {}
log = {
    info = function(msg) log_lines[#log_lines + 1] = "INFO: " .. msg end,
    warn = function(msg) log_lines[#log_lines + 1] = "WARN: " .. msg end,
    error = function(msg) log_lines[#log_lines + 1] = "ERROR: " .. msg end
}

-- ---------------------------------------------------------------------------
-- Mock HTTP server
-- ---------------------------------------------------------------------------

-- Each route entry: { matcher = function(url, headers, body) -> bool,
--                     respond = function(url, headers, body) -> resp }
-- LAST MATCH WINS: later-added routes override earlier ones (models
-- "re-setup between scenarios" without needing mock_clear everywhere).
local routes = {}

local function mock_add(route)
    routes[#routes + 1] = route
end

local function mock_clear()
    routes = {}
end

-- URL capture: assert request shape (types[] filter etc.)
local all_urls = {}

local function mock_dispatch(url, headers, body)
    all_urls[#all_urls + 1] = url
    local matched = nil
    for _, r in ipairs(routes) do
        if r.matcher(url, headers, body) then
            matched = r
        end
    end
    if matched then
        return matched.respond(url, headers, body)
    end
    return { status = 404, body = '{"error":"Record not found"}', headers = {} }
end

animus = {}
function animus.http_get(url, opts)
    return mock_dispatch(url, (opts and opts.headers) or {}, nil)
end
function animus.http_post(url, opts)
    return mock_dispatch(url, (opts and opts.headers) or {}, (opts and opts.body) or nil)
end

-- Capture outbound POST bodies for thread-chain assertions
local posted = {}
mock_add({
    matcher = function(url) return url:find("/api/v1/statuses", 1, true) ~= nil
        and not url:find("/context", 1, true) end,
    respond = function(url, headers, body)
        posted[#posted + 1] = { url = url, headers = headers, body = body }
        local data = body and json.decode(body) or {}
        return {
            status = 200,
            headers = {},
            body = json.encode({
                id = tostring(1000 + #posted),
                url = "https://mastodon.social/@agent/" .. tostring(1000 + #posted),
                content = data.status or "",
                visibility = data.visibility or "public"
            })
        }
    end
})

-- ---------------------------------------------------------------------------
-- Load the REAL adapter (capture its registration)
-- ---------------------------------------------------------------------------

local registration = nil
animus.register_channel = function(spec)
    registration = spec
end

local PID = "mastodon:TEST"
local function reset_config()
    config_store = {}
    config_store["channels." .. PID .. ".access_token"] = "test-token-123"
    config_store["channels." .. PID .. ".instance_url"] = "https://mastodon.social"
end
reset_config()

dofile("scripts/mastodon.lua")

print("== registration ==")
check("adapter registered", registration ~= nil)
check("id = mastodon", registration.id == "mastodon")
check("name present", registration.name == "Mastodon")
check("capabilities read+write", #registration.capabilities == 2)
check("actions declared", #registration.actions == 6,
    table.concat(registration.actions, ","))

local function call(action, args)
    args = args or {}
    args.action = action
    args.platform_id = args.platform_id or PID
    return registration.handler(args)
end

-- ---------------------------------------------------------------------------
print("== poll: happy path + watermark ==")
-- Two mentions, one already seen (id 105 <= watermark 110)
config_store["channels." .. PID .. ".last_notif_id"] = "110"
local notif_body = json.encode({
    { id = "120", type = "mention", created_at = "2026-10-10T15:00:00.000Z",
      status = { id = "9002", created_at = "2026-10-10T14:59:00.000Z",
        content = "<p><span class=\"h-card\"><a class=\"u-url mention\" href=\"https://mastodon.social/@agent\">@<span>agent</span></a></span> hello there</p>",
        url = "https://mastodon.social/@peer/9002",
        visibility = "public", in_reply_to_id = nil,
        account = { acct = "peer@inst.example", display_name = "Peer Person",
          url = "https://inst.example/@peer" } } },
    { id = "105", type = "mention", created_at = "2026-10-10T13:00:00.000Z",
      status = { id = "9001", created_at = "2026-10-10T13:00:00.000Z",
        content = "<p>already seen</p>", url = "https://mastodon.social/@peer/9001",
        visibility = "public",
        account = { acct = "peer@inst.example", display_name = "Peer Person" } } },
    { id = "115", type = "favourite", created_at = "2026-10-10T14:00:00.000Z" } -- non-mention: ignored by types[] server-side; harness sends anyway
})
mock_add({
    matcher = function(url) return url:find("/api/v1/notifications", 1, true) ~= nil end,
    respond = function() return { status = 200, headers = {
        ["x-ratelimit-limit"] = "300", ["x-ratelimit-remaining"] = "299"
    }, body = notif_body } end
})
local r = call("poll")
check("poll success", r.success == true, r.error)
check("rate-limit headers tracked (299/300)",
    config_store["channels." .. PID .. ".rate_limit_remaining"] == "299",
    tostring(config_store["channels." .. PID .. ".rate_limit_remaining"]))
check("request carries types[]=mention filter",
    all_urls[#all_urls]:find("types%[%]=mention", 1, false) ~= nil,
    all_urls[#all_urls])
check("min_id server cursor sent when watermark set",
    all_urls[#all_urls]:find("min_id=110", 1, true) ~= nil,
    all_urls[#all_urls])
check("poll count 1 (watermark dedupe + type filter)", r.count == 1, tostring(r.count))
check("watermark advanced to 120",
    config_store["channels." .. PID .. ".last_notif_id"] == "120",
    tostring(config_store["channels." .. PID .. ".last_notif_id"]))
local m = r.mentions[1]
check("author acct surfaced", m.author == "peer@inst.example", m.author)
check("author_display surfaced", m.author_display == "Peer Person")
check("created_at surfaced", m.created_at == "2026-10-10T14:59:00.000Z", m.created_at)
check("status url surfaced", m.status_url == "https://mastodon.social/@peer/9002")
check("author_url surfaced", m.author_url == "https://inst.example/@peer")
check("mention text plain (mention span unwrapped to @agent)",
    m.text == "@agent hello there", m.text)
check("raw HTML preserved alongside (content_html)",
    m.content_html ~= nil and m.content_html:find("h-card", 1, true) ~= nil,
    tostring(m.content_html))
check("in_reply_to_id empty (top-level)", m.in_reply_to_id == "")

-- Re-poll: same feed → zero new (watermark)
config_store["channels." .. PID .. ".last_notif_id"] = "110"
r = call("poll")
check("re-poll surfaces 1 again (watermark was reset)", r.count == 1)
r = call("poll")
check("third poll surfaces 0 (watermark now 120)", r.count == 0)

-- Empty notifications array
mock_add({
    matcher = function(url) return url:find("/api/v1/notifications", 1, true) ~= nil end,
    respond = function() return { status = 200, headers = {}, body = "[]" } end
})
r = call("poll")
check("poll empty array", r.success == true and r.count == 0)

-- Error paths
mock_add({
    matcher = function(url) return url:find("/api/v1/notifications", 1, true) ~= nil end,
    respond = function() return { status = 401, headers = {}, body = '{"error":"The access token is invalid"}' } end
})
r = call("poll")
check("poll 401 fails loud", r.success == false and r.error:find("401") ~= nil, r.error)

-- No first watermark: everything surfaced
config_store["channels." .. PID .. ".last_notif_id"] = nil
mock_add({
    matcher = function(url) return url:find("/api/v1/notifications", 1, true) ~= nil end,
    respond = function() return { status = 200, headers = {}, body = notif_body } end
})
r = call("poll")
check("no-watermark first poll surfaces both mentions", r.count == 2, tostring(r.count))

-- ---------------------------------------------------------------------------
print("== post/reply: chunk discipline (no byte-trim) ==")
mock_clear()
posted = {}
mock_add({
    matcher = function(url) return url:find("/api/v1/statuses", 1, true) ~= nil
        and not url:find("/context", 1, true) end,
    respond = function(url, headers, body)
        posted[#posted + 1] = { url = url, headers = headers, body = body }
        local data = body and json.decode(body) or {}
        return {
            status = 200, headers = {},
            body = json.encode({
                id = tostring(2000 + #posted),
                url = "https://mastodon.social/@agent/" .. tostring(2000 + #posted),
                content = data.status or ""
            })
        }
    end
})
-- Char limit fetch: both endpoints 404 → adapter defaults 500 (loud, not fatal)
mock_add({
    matcher = function(url) return url:find("/api/v%d/instance", 1, false) ~= nil end,
    respond = function() return { status = 404, headers = {}, body = '{}' } end
})

-- 1200-char post with paragraphs, lines, and a fenced code block spanning the split
local para = string.rep("abcdefgh", 8) .. " "  -- 65 chars
local long = "Intro paragraph.\n\n" .. string.rep(para .. "\n", 12) .. "\n```lua\nlocal x = 1\nlocal y = 2\nlocal z = 3\n```\n\nFinal."
r = call("post", { content = long })
check("long post success", r.success == true, r.error)
check("long post split into >1 toots", #posted > 1, tostring(#posted))
local total, all_within = 0, true
local fence_opens = 0
for i, p in ipairs(posted) do
    local data = json.decode(p.body)
    local status_text = data.status
    total = total + #status_text
    if #status_text > 500 then all_within = false end
    -- count fences across the whole thread
    local n = 0
    for _ in status_text:gmatch("```") do n = n + 1 end
    fence_opens = fence_opens + n
end
check("every chunk within 500 limit", all_within)
check("no content lost (concat length modulo fence repairs)",
    math.abs(total - #long) < 60, "total=" .. total .. " orig=" .. #long)
check("code fences preserved (even count across thread)", fence_opens % 2 == 0,
    tostring(fence_opens))
-- Thread chaining: 2nd+ chunks reply to the 1st
local chained = true
local first_id = json.decode(posted[1].body) and (json.decode(posted[1].body))
for i = 2, #posted do
    local data = json.decode(posted[i].body)
    -- in_reply_to_id of posted[i] should equal id of posted[1] (mock assigns 2001, 2002...)
end
-- (Mock returns ids 2001..; body fields asserted below via raw body decode)
local b1, b2 = json.decode(posted[1].body), json.decode(posted[2].body)
check("first chunk has no in_reply_to_id", b1.in_reply_to_id == nil)
check("second chunk chains to first (id 2001)", b2.in_reply_to_id == "2001",
    tostring(b2.in_reply_to_id))

-- Short post: single toot, no split
posted = {}
r = call("post", { content = "just a quick toot" })
check("short post single chunk", #posted == 1, tostring(#posted))

-- Reply: first chunk carries in_reply_to_id
posted = {}
r = call("reply", { status_id = "7777", content = "a reply" })
local b = json.decode(posted[1].body)
check("reply chains to status 7777", b.in_reply_to_id == "7777", tostring(b.in_reply_to_id))

-- Visibility validation
r = call("post", { content = "x", visibility = "banana" })
check("invalid visibility rejected loud", r.success == false and r.error:find("invalid visibility") ~= nil)

-- ---------------------------------------------------------------------------
print("== backoff: 429 handling ==")
mock_clear()
config_store["channels." .. PID .. ".backoff_until"] = nil
local backoff_calls = {}
mock_add({
    matcher = function(url) return true end,
    respond = function()
        backoff_calls[#backoff_calls + 1] = 1
        if #backoff_calls <= 1 then
            return { status = 429, headers = { ["retry-after"] = "120" },
                body = '{"error":"Too many requests"}' }
        end
        return { status = 200, headers = {}, body = "[]" }
    end
})
r = call("poll")
check("429 poll fails loud", r.success == false and r.error:find("429") ~= nil, r.error)
local backoff_stored = config_store["channels." .. PID .. ".backoff_until"]
check("backoff persisted after 429", backoff_stored ~= nil and backoff_stored ~= "",
    tostring(backoff_stored))
check("Retry-After honored (120s)",
    config_store["channels." .. PID .. ".backoff_seconds"] == "120",
    tostring(config_store["channels." .. PID .. ".backoff_seconds"]))
-- Subsequent call blocked by backoff (no HTTP hit)
local calls_before = #backoff_calls
r = call("poll")
check("in-backoff call short-circuits (no HTTP)",
    r.success == false and #backoff_calls == calls_before,
    "calls " .. calls_before .. "→" .. #backoff_calls)
check("in-backoff error mentions backoff", r.error ~= nil and r.error:find("backoff") ~= nil, r.error)

-- Backoff expiry: force past
config_store["channels." .. PID .. ".backoff_until"] = "2000-01-01T00:00:00Z"
r = call("poll")
check("expired backoff clears and proceeds", r.success == true, r.error)
check("backoff_until cleared", config_store["channels." .. PID .. ".backoff_until"] == "")

-- ---------------------------------------------------------------------------
print("== verify: credentials ==")
mock_clear()
mock_add({
    matcher = function(url) return url:find("/api/v1/accounts/verify_credentials", 1, true) ~= nil end,
    respond = function()
        return { status = 200, headers = {},
            body = json.encode({ id = "42", acct = "agent@mastodon.social",
                display_name = "Test Agent", url = "https://mastodon.social/@agent",
                created_at = "2026-01-01T00:00:00.000Z" }) }
    end
})
r = call("verify")
check("verify success", r.success == true, r.error)
check("verify returns acct", r.output:find("agent@mastodon.social") ~= nil)

-- Missing token
config_store["channels." .. PID .. ".access_token"] = ""
r = call("verify")
check("missing token fails loud", r.success == false and r.error:find("access_token") ~= nil, r.error)
reset_config()

-- ---------------------------------------------------------------------------
print("== handler contract ==")
r = registration.handler({})
check("missing action rejected", r.success == false and r.error:find("action is required") ~= nil)
r = call("frobnicate")
check("unknown action rejected", r.success == false and r.error:find("unknown action") ~= nil)

-- ---------------------------------------------------------------------------
print("")
print(("RESULT: %d passed, %d failed"):format(PASS, FAIL))
if #failures > 0 then
    print("Failures:")
    for _, f in ipairs(failures) do print("  - " .. f) end
    os.exit(1)
end
os.exit(0)