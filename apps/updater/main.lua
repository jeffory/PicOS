-- PicOS System Updater
-- Downloads and applies firmware updates from GitHub Releases

local display = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local net = picocalc.network
local config = picocalc.config
local fs = picocalc.fs

-- GitHub Releases API configuration
local GH_API_HOST  = "api.github.com"
local GH_REPO      = "jeffory/picOS"
local GH_API_PATH  = "/repos/" .. GH_REPO .. "/releases/latest"
local BIN_FILENAME  = "picocalc_os.bin"
local HASH_FILENAME = "picocalc_os.sha256"
local BIN_PATH      = "/system/update.bin"
local HASH_PATH     = "/system/update.sha256"
local MAX_REDIRECTS = 3

-- Screen dimensions
local W, H = display.getWidth(), display.getHeight()

-- Colors
local BLACK = 0x0000
local WHITE = 0xFFFF
local RED = 0xF800
local GREEN = 0x07E0
local YELLOW = 0xFFE0
local CYAN = 0x07FF
local GRAY = 0x8410
local DKGRAY = 0x4208

-- Screens
local SCR_MAIN = 1
local SCR_CHECKING = 2
local SCR_AVAILABLE = 3
local SCR_DOWNLOADING = 4
local SCR_DONE = 5
local SCR_ERROR = 6
local SCR_UP_TO_DATE = 7
local SCR_CONFIRM = 8

local current_screen = SCR_MAIN
local error_msg = ""
local current_conn = nil

-- Update info from server
local remote_version = nil
local remote_tag = nil
local remote_size = 0
local remote_changelog = nil
local remote_bin_url = nil
local remote_hash_url = nil

-- Download state
local download_received = 0
local download_total = 0
local download_file = nil

-- ── Helpers ─────────────────────────────────────────────────────────────────

local function draw_centered(y, text, fg, bg)
    local tw = display.textWidth(text)
    display.drawText(math.floor((W - tw) / 2), y, text, fg, bg or BLACK)
end

local function version_newer(remote, local_ver)
    -- Simple semver comparison: "1.0.1" > "1.0.0"
    local function parse(v)
        local parts = {}
        for n in v:gmatch("(%d+)") do
            parts[#parts + 1] = tonumber(n)
        end
        return parts
    end
    local r = parse(remote)
    local l = parse(local_ver)
    for i = 1, math.max(#r, #l) do
        local rv = r[i] or 0
        local lv = l[i] or 0
        if rv > lv then return true end
        if rv < lv then return false end
    end
    return false
end

-- Simple JSON value extraction (no full parser needed)
local function json_get(json, key)
    -- Try string value first
    local val = json:match('"' .. key .. '"%s*:%s*"([^"]*)"')
    if val then return val end
    -- Try number value
    val = json:match('"' .. key .. '"%s*:%s*(%d+)')
    if val then return val end
    return nil
end

-- Parse a URL into host, port, ssl, path
local function parse_url(url)
    local host, path
    local ssl = false
    local port = 80

    if url:match("^https://") then
        ssl = true
        port = 443
        local rest = url:sub(9)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    elseif url:match("^http://") then
        local rest = url:sub(8)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    else
        return nil
    end

    -- Extract port from host if present
    local h, p = host:match("^(.+):(%d+)$")
    if h then
        host = h
        port = tonumber(p)
    end

    return host, port, ssl, path
end

-- ── Network: Check for updates ──────────────────────────────────────────────

local function check_for_update()
    current_screen = SCR_CHECKING

    local status = net.getStatus()
    if status ~= net.kStatusConnected then
        error_msg = "WiFi not connected.\nConnect via Settings first."
        current_screen = SCR_ERROR
        return
    end

    local conn = net.http.new(GH_API_HOST, 443, true)
    if not conn then
        error_msg = "Cannot connect to GitHub API."
        current_screen = SCR_ERROR
        return
    end

    conn:setConnectTimeout(15)
    conn:setReadTimeout(15)
    conn:setReadBufferSize(8192)
    current_conn = conn

    local body = ""

    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            body = body .. conn:read()
        end
    end)

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()
        conn:close()
        current_conn = nil

        if http_status ~= 200 then
            error_msg = "GitHub API returned HTTP " .. tostring(http_status)
            current_screen = SCR_ERROR
            return
        end

        -- Parse release info
        local tag = json_get(body, "tag_name")
        if not tag then
            error_msg = "Invalid response from GitHub API."
            current_screen = SCR_ERROR
            return
        end

        -- Strip leading 'v' for version comparison
        remote_tag = tag
        remote_version = tag:match("^v(.+)") or tag

        -- Extract changelog from release body
        remote_changelog = json_get(body, "body")

        -- Extract binary size from assets array
        local size_str = body:match(
            '"name"%s*:%s*"picocalc_os%.bin".-"size"%s*:%s*(%d+)')
        remote_size = tonumber(size_str or "0")

        -- Construct download URLs from tag (deterministic pattern)
        local dl_base = "https://github.com/" .. GH_REPO ..
            "/releases/download/" .. tag .. "/"
        remote_bin_url = dl_base .. BIN_FILENAME
        remote_hash_url = dl_base .. HASH_FILENAME

        local local_ver = sys.getVersion()
        if version_newer(remote_version, local_ver) then
            current_screen = SCR_AVAILABLE
        else
            current_screen = SCR_UP_TO_DATE
        end
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if current_screen == SCR_CHECKING then
            error_msg = "Connection closed unexpectedly."
            current_screen = SCR_ERROR
        end
    end)

    local headers = {
        ["User-Agent"] = "PicOS-Updater/1.0",
        ["Accept"] = "application/vnd.github+json",
    }

    if not conn:get(GH_API_PATH, headers) then
        error_msg = "Failed to send request."
        current_screen = SCR_ERROR
        current_conn = nil
    end
end

-- ── Network: Download with redirect following ───────────────────────────────

local function download_hash_file(url, redirect_count)
    redirect_count = redirect_count or 0
    if redirect_count >= MAX_REDIRECTS then
        -- Hash is optional, just skip
        current_screen = SCR_DONE
        return
    end

    local host, port, ssl, path = parse_url(url)
    if not host then
        current_screen = SCR_DONE
        return
    end

    local conn = net.http.new(host, port, ssl)
    if not conn then
        current_screen = SCR_DONE
        return
    end

    conn:setConnectTimeout(15)
    conn:setReadTimeout(15)
    conn:setReadBufferSize(512)
    current_conn = conn

    local body = ""

    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            body = body .. conn:read()
        end
    end)

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()
        local resp_headers = conn:getResponseHeaders()
        conn:close()
        current_conn = nil

        if http_status and http_status >= 300 and http_status < 400 then
            -- Follow redirect
            local location = resp_headers and resp_headers["location"]
            if location then
                download_hash_file(location, redirect_count + 1)
                return
            end
        end

        if http_status == 200 and #body > 0 then
            -- Extract just the hex hash (first 64 chars)
            local hash = body:match("^(%x+)")
            if hash then
                local hf = fs.open(HASH_PATH, "w")
                if hf then
                    fs.write(hf, hash)
                    fs.close(hf)
                end
            end
        end

        current_screen = SCR_DONE
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if current_screen == SCR_DOWNLOADING then
            -- Hash download failed, non-fatal
            current_screen = SCR_DONE
        end
    end)

    if not conn:get(path, {["User-Agent"] = "PicOS-Updater/1.0"}) then
        current_screen = SCR_DONE
    end
end

local function fetch_firmware(url, redirect_count)
    redirect_count = redirect_count or 0
    if redirect_count >= MAX_REDIRECTS then
        error_msg = "Too many redirects."
        current_screen = SCR_ERROR
        return
    end

    local host, port, ssl, path = parse_url(url)
    if not host then
        error_msg = "Invalid download URL."
        current_screen = SCR_ERROR
        return
    end

    local conn = net.http.new(host, port, ssl)
    if not conn then
        error_msg = "Cannot connect to download server."
        current_screen = SCR_ERROR
        return
    end

    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    -- Allocate large buffer for the binary (or redirect response)
    conn:setReadBufferSize(2 * 1024 * 1024)
    current_conn = conn

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()
        local resp_headers = conn:getResponseHeaders()
        conn:close()
        current_conn = nil

        -- Handle redirects (GitHub sends 302 to objects.githubusercontent.com)
        if http_status and http_status >= 300 and http_status < 400 then
            local location = resp_headers and resp_headers["location"]
            if location then
                fetch_firmware(location, redirect_count + 1)
                return
            end
            error_msg = "Redirect without Location header."
            current_screen = SCR_ERROR
            return
        end

        if http_status ~= 200 then
            error_msg = "Download failed: HTTP " .. tostring(http_status)
            if download_file then
                fs.close(download_file)
                download_file = nil
            end
            fs.remove(BIN_PATH)
            current_screen = SCR_ERROR
            return
        end

        -- Write body data to file
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            local data = conn:read()
            if data and #data > 0 then
                if not download_file then
                    download_file = fs.open(BIN_PATH, "w")
                end
                if download_file then
                    fs.write(download_file, data)
                    download_received = download_received + #data
                end
            end
        end

        if download_file then
            fs.close(download_file)
            download_file = nil
        end

        -- Now download the hash file
        if remote_hash_url then
            download_hash_file(remote_hash_url)
        else
            current_screen = SCR_DONE
        end
    end)

    conn:setRequestCallback(function()
        -- Accumulate data during download for progress display
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            local data = conn:read()
            if data and #data > 0 then
                if not download_file then
                    download_file = fs.open(BIN_PATH, "w")
                end
                if download_file then
                    fs.write(download_file, data)
                    download_received = download_received + #data
                end
            end
        end
    end)

    conn:setHeadersReadCallback(function()
        local hdrs = conn:getResponseHeaders()
        if hdrs and hdrs["content-length"] then
            local cl = tonumber(hdrs["content-length"])
            if cl and cl > 0 then
                download_total = cl
            end
        end
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if download_file then
            fs.close(download_file)
            download_file = nil
        end
        if current_screen == SCR_DOWNLOADING then
            error_msg = "Download interrupted."
            current_screen = SCR_ERROR
        end
    end)

    if not conn:get(path, {["User-Agent"] = "PicOS-Updater/1.0"}) then
        error_msg = "Failed to start download."
        current_screen = SCR_ERROR
        current_conn = nil
    end
end

local function start_download()
    current_screen = SCR_DOWNLOADING
    download_received = 0
    download_total = remote_size
    download_file = nil

    if not remote_bin_url then
        error_msg = "No download URL available."
        current_screen = SCR_ERROR
        return
    end

    fetch_firmware(remote_bin_url)
end

-- ── Drawing ─────────────────────────────────────────────────────────────────

local function draw_header()
    display.fillRect(0, 0, W, 20, DKGRAY)
    draw_centered(4, "System Update", WHITE, DKGRAY)
end

local function draw_main()
    display.clear(BLACK)
    draw_header()

    local ver = sys.getVersion()
    display.drawText(8, 40, "Current firmware:", GRAY, BLACK)
    display.drawText(8, 56, "  Version " .. ver, WHITE, BLACK)

    draw_centered(100, "Press Enter to check", CYAN, BLACK)
    draw_centered(116, "for updates", CYAN, BLACK)

    display.drawText(8, H - 16, "ESC: Exit", GRAY, BLACK)
    display.flush()
end

local function draw_checking()
    display.clear(BLACK)
    draw_header()
    draw_centered(140, "Checking for updates...", WHITE, BLACK)
    display.drawText(8, H - 16, "ESC: Cancel", GRAY, BLACK)
    display.flush()
end

local function draw_up_to_date()
    display.clear(BLACK)
    draw_header()
    draw_centered(120, "Firmware is up to date!", GREEN, BLACK)
    local ver = sys.getVersion()
    draw_centered(140, "Version " .. ver, GRAY, BLACK)
    display.drawText(8, H - 16, "ESC: Back", GRAY, BLACK)
    display.flush()
end

local function draw_available()
    display.clear(BLACK)
    draw_header()

    display.drawText(8, 40, "Update available!", GREEN, BLACK)

    local ver = sys.getVersion()
    display.drawText(8, 64, "Current: " .. ver, GRAY, BLACK)
    display.drawText(8, 80, "New:     " .. (remote_version or "?"), WHITE, BLACK)

    if remote_size > 0 then
        local size_kb = math.floor(remote_size / 1024)
        display.drawText(8, 100, "Size: " .. size_kb .. " KB", GRAY, BLACK)
    end

    if remote_changelog then
        display.drawText(8, 124, "Changes:", YELLOW, BLACK)
        -- Simple word-wrap for changelog
        local y = 140
        local line = ""
        for word in remote_changelog:gmatch("%S+") do
            if display.textWidth(line .. " " .. word) > W - 16 then
                display.drawText(8, y, line, WHITE, BLACK)
                y = y + 12
                line = word
                if y > H - 40 then break end
            else
                line = (line == "") and word or (line .. " " .. word)
            end
        end
        if line ~= "" and y <= H - 40 then
            display.drawText(8, y, line, WHITE, BLACK)
        end
    end

    draw_centered(H - 32, "Enter: Download & Install", CYAN, BLACK)
    display.drawText(8, H - 16, "ESC: Cancel", GRAY, BLACK)
    display.flush()
end

local function draw_downloading()
    display.clear(BLACK)
    draw_header()

    draw_centered(120, "Downloading firmware...", WHITE, BLACK)

    -- Progress bar
    local bar_x, bar_y, bar_w, bar_h = 40, 150, 240, 16
    display.fillRect(bar_x, bar_y, bar_w, bar_h, DKGRAY)
    if download_total > 0 then
        local fill = math.floor(bar_w * download_received / download_total)
        if fill > 0 then
            display.fillRect(bar_x, bar_y, fill, bar_h, GREEN)
        end
    end
    display.drawRect(bar_x, bar_y, bar_w, bar_h, WHITE)

    local pct = 0
    if download_total > 0 then
        pct = math.floor(download_received * 100 / download_total)
    end
    local kb_done = math.floor(download_received / 1024)
    local kb_total = math.floor(download_total / 1024)
    local status = string.format("%d%%  (%dK / %dK)", pct, kb_done, kb_total)
    draw_centered(bar_y + bar_h + 8, status, WHITE, BLACK)

    display.drawText(8, H - 16, "ESC: Cancel", GRAY, BLACK)
    display.flush()
end

local function draw_done()
    display.clear(BLACK)
    draw_header()

    draw_centered(100, "Download complete!", GREEN, BLACK)
    draw_centered(120, "Ready to install.", WHITE, BLACK)

    draw_centered(160, "The device will reboot", YELLOW, BLACK)
    draw_centered(176, "to apply the update.", YELLOW, BLACK)

    draw_centered(H - 32, "Enter: Install Now", CYAN, BLACK)
    display.drawText(8, H - 16, "ESC: Cancel", GRAY, BLACK)
    display.flush()
end

local function draw_error()
    display.clear(BLACK)
    draw_header()

    display.drawText(8, 80, "Error:", RED, BLACK)
    -- Word-wrap error message
    local y = 100
    for line in error_msg:gmatch("[^\n]+") do
        display.drawText(8, y, line, WHITE, BLACK)
        y = y + 14
    end

    display.drawText(8, H - 16, "ESC: Back", GRAY, BLACK)
    display.flush()
end

local function draw_confirm()
    display.clear(BLACK)
    draw_header()

    draw_centered(100, "Apply firmware update?", YELLOW, BLACK)
    draw_centered(120, "Version " .. (remote_version or "?"), WHITE, BLACK)

    draw_centered(160, "The device will reboot.", GRAY, BLACK)
    draw_centered(176, "Do not power off during", GRAY, BLACK)
    draw_centered(192, "the update process.", GRAY, BLACK)

    draw_centered(240, "Enter: Yes, Install", GREEN, BLACK)
    draw_centered(260, "ESC: No, Cancel", RED, BLACK)
    display.flush()
end

-- ── Main loop ───────────────────────────────────────────────────────────────

local last_screen = nil
local frame = 0

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    -- ESC exits or goes back
    if pressed & input.BTN_ESCAPE ~= 0 then
        if current_conn then
            current_conn:close()
            current_conn = nil
        end
        if download_file then
            fs.close(download_file)
            download_file = nil
        end

        if current_screen == SCR_MAIN then
            return -- exit app
        else
            current_screen = SCR_MAIN
        end
    end

    -- Enter actions per screen
    if pressed & input.BTN_ENTER ~= 0 then
        if current_screen == SCR_MAIN then
            check_for_update()
        elseif current_screen == SCR_AVAILABLE then
            start_download()
        elseif current_screen == SCR_DONE then
            current_screen = SCR_CONFIRM
        elseif current_screen == SCR_CONFIRM then
            -- Apply the update (this reboots on success)
            local ok, err = sys.applyUpdate(BIN_PATH)
            if not ok then
                error_msg = "Failed to apply update:\n" .. (err or "unknown error")
                current_screen = SCR_ERROR
            end
        elseif current_screen == SCR_UP_TO_DATE or current_screen == SCR_ERROR then
            current_screen = SCR_MAIN
        end
    end

    -- Redraw on screen change or periodically during downloads
    local needs_draw = (current_screen ~= last_screen)
    if current_screen == SCR_DOWNLOADING then needs_draw = true end
    if current_screen == SCR_CHECKING then
        frame = frame + 1
        if frame % 10 == 0 then needs_draw = true end
    end

    if needs_draw then
        if current_screen == SCR_MAIN then draw_main()
        elseif current_screen == SCR_CHECKING then draw_checking()
        elseif current_screen == SCR_AVAILABLE then draw_available()
        elseif current_screen == SCR_DOWNLOADING then draw_downloading()
        elseif current_screen == SCR_DONE then draw_done()
        elseif current_screen == SCR_ERROR then draw_error()
        elseif current_screen == SCR_UP_TO_DATE then draw_up_to_date()
        elseif current_screen == SCR_CONFIRM then draw_confirm()
        end
        last_screen = current_screen
    end

    sys.sleep(33) -- ~30 FPS
end
