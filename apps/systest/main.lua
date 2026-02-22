-- System Test App for PicoCalc OS
-- Tests all exposed system functions

local pc = picocalc
local disp = pc.display
local input = pc.input
local sys = pc.sys
local fs = pc.fs
local wifi = pc.wifi

-- Colors
local BG     = disp.BLACK
local FG     = disp.WHITE
local WARN   = disp.rgb(255, 100, 0)
local GOOD   = disp.rgb(0, 255, 100)
local DIM    = disp.GRAY
local TITLE  = disp.CYAN

-- Test results
local tests = {
    battery = {name = "Battery", status = "WAIT", value = ""},
    usb = {name = "USB Power", status = "WAIT", value = ""},
    time = {name = "Time", status = "WAIT", value = ""},
    log = {name = "Logging", status = "WAIT", value = ""},
    fs_read = {name = "FS Read", status = "WAIT", value = ""},
    fs_write = {name = "FS Write", status = "WAIT", value = ""},
    fs_list = {name = "FS List", status = "WAIT", value = ""},
    fs_mkdir = {name = "FS Mkdir", status = "WAIT", value = ""},
    wifi_avail = {name = "WiFi Avail", status = "WAIT", value = ""},
    wifi_status = {name = "WiFi Status", status = "WAIT", value = ""},
}

local test_order = {"battery", "usb", "time", "log", "fs_read", "fs_mkdir", "fs_write", "fs_list", "wifi_avail", "wifi_status"}

local mode = "tabs"  -- Always use tabbed interface now
local start_time = 0
local data_dir = "/data/" .. APP_ID
local test_file = data_dir .. "/systest.txt"
local button_state = 0

-- Tab demo state
local active_tab = 1
local tab_labels = {"Tests", "Input", "Display", "Network", "System"}
local nav_keys = {prev = input.BTN_LEFT, next = input.BTN_RIGHT}

-- Input test state (from keytest)
local MAX_HISTORY = 16
local history = {}
local last_raw = 0
local last_char = nil

local function push_history(line)
    table.insert(history, 1, line)
    if #history > MAX_HISTORY then table.remove(history) end
end

local BTN_LABELS = {
    { input.BTN_UP,    "UP" },
    { input.BTN_DOWN,  "DOWN" },
    { input.BTN_LEFT,  "LEFT" },
    { input.BTN_RIGHT, "RIGHT" },
    { input.BTN_ENTER, "Enter" },
    { input.BTN_ESC,   "Esc" },
    { input.BTN_MENU,  "Sym" },
    { input.BTN_TAB,   "Tab" },
}

local function btn_names(mask)
    if mask == 0 then return nil end
    local t = {}
    for _, b in ipairs(BTN_LABELS) do
        if mask & b[1] ~= 0 then t[#t + 1] = b[2] end
    end
    return #t > 0 and table.concat(t, "+") or nil
end

-- ── Test Functions ────────────────────────────────────────────────────────────

local function run_battery_test()
    local bat = sys.getBattery()
    if bat >= 0 then
        tests.battery.status = "PASS"
        tests.battery.value = bat .. "%"
    else
        tests.battery.status = "FAIL"
        tests.battery.value = "Unknown"
    end
end

local function run_usb_test()
    local usb = sys.isUSBPowered()
    tests.usb.status = "INFO"
    tests.usb.value = usb and "Yes" or "No"
end

local function run_time_test()
    local t1 = sys.getTimeMs()
    sys.sleep(10)
    local t2 = sys.getTimeMs()
    local diff = t2 - t1
    if diff >= 8 and diff <= 15 then
        tests.time.status = "PASS"
        tests.time.value = diff .. "ms"
    else
        tests.time.status = "FAIL"
        tests.time.value = diff .. "ms (expect ~10)"
    end
end

local function run_log_test()
    sys.log("System test log message")
    tests.log.status = "PASS"
    tests.log.value = "Check USB serial"
end

local function run_fs_read_test()
    -- Try to read our own app.json
    local data = fs.readFile(APP_DIR .. "/app.json")
    if data and #data > 0 then
        tests.fs_read.status = "PASS"
        tests.fs_read.value = #data .. " bytes"
    else
        tests.fs_read.status = "FAIL"
        tests.fs_read.value = "Failed"
    end
end

local function run_fs_mkdir_test()
    -- Create our data directory
    local success = fs.mkdir(data_dir)
    if success then
        tests.fs_mkdir.status = "PASS"
        tests.fs_mkdir.value = APP_ID
    else
        tests.fs_mkdir.status = "FAIL"
        tests.fs_mkdir.value = "Failed"
    end
end

local function run_fs_write_test()
    -- Write a test file to our data directory
    local f = fs.open(test_file, "w")
    if f then
        local written = fs.write(f, "PicoCalc OS Test: " .. sys.getTimeMs())
        fs.close(f)
        if written > 0 then
            tests.fs_write.status = "PASS"
            tests.fs_write.value = written .. " bytes"
        else
            tests.fs_write.status = "FAIL"
            tests.fs_write.value = "Write failed"
        end
    else
        tests.fs_write.status = "FAIL"
        tests.fs_write.value = "Open failed"
    end
end

local function run_fs_list_test()
    local entries = fs.listDir(APP_DIR)
    if entries and #entries > 0 then
        tests.fs_list.status = "PASS"
        tests.fs_list.value = #entries .. " entries"
    else
        tests.fs_list.status = "FAIL"
        tests.fs_list.value = "No entries"
    end
end

local function run_wifi_avail_test()
    local avail = wifi.isAvailable()
    if avail then
        tests.wifi_avail.status = "PASS"
        tests.wifi_avail.value = "Hardware OK"
    else
        tests.wifi_avail.status = "INFO"
        tests.wifi_avail.value = "Not available"
    end
end

local function run_wifi_status_test()
    if not wifi.isAvailable() then
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "N/A"
        return
    end
    
    local status = wifi.getStatus()
    if status == wifi.STATUS_CONNECTED then
        local ip = wifi.getIP()
        local ssid = wifi.getSSID()
        tests.wifi_status.status = "PASS"
        tests.wifi_status.value = ssid .. " (" .. ip .. ")"
    elseif status == wifi.STATUS_CONNECTING then
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "Connecting..."
    elseif status == wifi.STATUS_FAILED then
        tests.wifi_status.status = "WARN"
        tests.wifi_status.value = "Failed"
    else
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "Disconnected"
    end
end

local function run_all_tests()
    run_battery_test()
    run_usb_test()
    run_time_test()
    run_log_test()
    run_fs_read_test()
    run_fs_mkdir_test()
    run_fs_write_test()
    run_fs_list_test()
    run_wifi_avail_test()
    run_wifi_status_test()
end

-- ── Drawing Functions ─────────────────────────────────────────────────────────

local function draw_status_color(status)
    if status == "PASS" then return GOOD
    elseif status == "FAIL" then return WARN
    elseif status == "WARN" then return WARN
    elseif status == "INFO" then return disp.CYAN
    else return DIM
    end
end

local function draw_tabs_demo()
    disp.clear(BG)
    
    pc.ui.drawHeader("System Test Suite")
    
    -- Draw tabs with customizable navigation keys
    local new_tab, height = pc.ui.drawTabs(29, tab_labels, active_tab, 
                                           nav_keys.prev, nav_keys.next)
    active_tab = new_tab
    
    -- Content area starts below tabs
    local content_y = 29 + height + 10
    
    -- Draw content based on active tab
    if active_tab == 1 then
        -- Tests tab - show test results
        local y = content_y
        local line_height = 11
        
        for _, key in ipairs(test_order) do
            local test = tests[key]
            local color = draw_status_color(test.status)
            
            disp.drawText(4, y, test.name, FG, BG)
            local status_x = 70
            disp.drawText(status_x, y, test.status, color, BG)
            
            if test.value ~= "" then
                local value_x = 110
                local max_len = 28
                local display_val = test.value
                if #display_val > max_len then
                    display_val = display_val:sub(1, max_len - 3) .. "..."
                end
                disp.drawText(value_x, y, display_val, DIM, BG)
            end
            
            y = y + line_height
        end
        
        -- Runtime info
        y = y + 10
        local runtime = sys.getTimeMs() - start_time
        disp.drawText(4, y, "Runtime: " .. runtime .. " ms", DIM, BG)
        
        -- Footer instructions
        pc.ui.drawFooter("←/→:Tab  ENTER/R:Rerun  ESC:Exit", nil)
        
    elseif active_tab == 2 then
        -- Input tab with keytest functionality
        disp.drawText(20, content_y, "Input Tab", TITLE, BG)
        content_y = content_y + 16
        
        -- Last raw keycode
        disp.drawText(20, content_y, "Last raw:", DIM, BG)
        content_y = content_y + 10
        disp.drawText(80, content_y, string.format("0x%02X  (%d)", last_raw, last_raw), FG, BG)
        content_y = content_y + 12
        
        -- Last character
        disp.drawText(20, content_y, "Char:", DIM, BG)
        if last_char then
            local label = last_char
            if string.byte(last_char) == 8 then label = "<Bkspc>" end
            disp.drawText(80, content_y, "'" .. label .. "'", FG, BG)
        else
            disp.drawText(80, content_y, "(none)", DIM, BG)
        end
        content_y = content_y + 12
        
        -- Currently held
        disp.drawText(20, content_y, "Held:", DIM, BG)
        disp.drawText(80, content_y, btn_names(button_state) or "(none)", FG, BG)
        content_y = content_y + 16
        
        -- Event log
        disp.drawText(20, content_y, "Event log:", DIM, BG)
        content_y = content_y + 10
        for i, entry in ipairs(history) do
            local y = content_y + (i - 1) * 10
            if y < 300 then
                disp.drawText(20, y, entry, i == 1 and FG or DIM, BG)
            end
        end
        
    elseif active_tab == 3 then
        disp.drawText(20, content_y, "Display Tab", TITLE, BG)
        content_y = content_y + 20
        disp.drawText(20, content_y, "Resolution: 320x320", FG, BG)
        content_y = content_y + 12
        disp.drawText(20, content_y, "Color depth: RGB565", FG, BG)
        content_y = content_y + 12
        -- Draw a gradient demo
        for i = 0, 50 do
            local c = disp.rgb(i * 5, 100, 255 - i * 5)
            disp.fillRect(20 + i * 2, content_y + 10, 2, 20, c)
        end
        
    elseif active_tab == 4 then
        disp.drawText(20, content_y, "Network Tab", TITLE, BG)
        content_y = content_y + 20
        disp.drawText(20, content_y, "WiFi: " .. tests.wifi_status.value, FG, BG)
        content_y = content_y + 12
        disp.drawText(20, content_y, "Status: " .. tests.wifi_avail.value, FG, BG)
        
    elseif active_tab == 5 then
        disp.drawText(20, content_y, "System Tab", TITLE, BG)
        content_y = content_y + 20
        disp.drawText(20, content_y, "Uptime: " .. (sys.getTimeMs() - start_time) .. " ms", FG, BG)
        content_y = content_y + 12
        disp.drawText(20, content_y, "Battery: " .. tests.battery.value, FG, BG)
        content_y = content_y + 12
        disp.drawText(20, content_y, "USB: " .. tests.usb.value, FG, BG)
    end
    
    -- Footer with instructions
    pc.ui.drawFooter("F1:Exit  T:ToggleKeys  ESC:Back", nil)
end

-- ── Main Loop ─────────────────────────────────────────────────────────────────

start_time = sys.getTimeMs()

-- Run all tests once at startup
run_all_tests()

while true do
    input.update()
    local pressed = input.getButtonsPressed()
    local released = input.getButtonsReleased()
    button_state = input.getButtons()
    
    -- Track raw key and character for Input tab
    -- Get char first so it's available when we check raw key
    last_char = input.getChar()
    local raw = input.getRawKey()
    if raw ~= 0 and raw ~= last_raw then
        last_raw = raw
        local label = ""
        if last_char then
            if string.byte(last_char) == 8 then
                label = "<Bkspc>"
            else
                label = string.format("'%s'", last_char)
            end
        elseif btn_names(pressed) then
            label = btn_names(pressed)
        else
            -- Raw key but no char and no button name - use raw key code
            label = string.format("[0x%02X]", raw)
        end
        push_history(string.format("0x%02X  %s", raw, label))
    end
    
    -- ESC: exit
    if pressed & input.BTN_ESC ~= 0 then
        return
    end
    
    -- ENTER or R: rerun tests
    if pressed & input.BTN_ENTER ~= 0 or (last_char and last_char:upper() == "R") then
        run_all_tests()
        last_raw = 0
        last_char = nil
    end
    
    -- T: Toggle navigation keys between Left/Right and Up/Down
    if last_char and last_char:upper() == "T" then
        if nav_keys.prev == input.BTN_LEFT then
            nav_keys.prev = input.BTN_UP
            nav_keys.next = input.BTN_DOWN
        else
            nav_keys.prev = input.BTN_LEFT
            nav_keys.next = input.BTN_RIGHT
        end
    end
    
    draw_tabs_demo()
    
    disp.flush()
    sys.sleep(16)
end
