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

local mode = "auto"  -- "auto" or "reboot_confirm"
local start_time = 0
local data_dir = "/data/" .. APP_ID
local test_file = data_dir .. "/systest.txt"
local last_char = ""
local button_state = 0

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
    local entries = fs.listDir("/apps")
    if entries and #entries > 0 then
        tests.fs_list.status = "PASS"
        tests.fs_list.value = #entries .. " apps"
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

local function draw_test_results()
    local y = 30
    local line_height = 11
    
    for _, key in ipairs(test_order) do
        local test = tests[key]
        local color = draw_status_color(test.status)
        
        -- Test name (shorter width)
        disp.drawText(4, y, test.name, FG, BG)
        
        -- Status
        local status_x = 90
        disp.drawText(status_x, y, test.status, color, BG)
        
        -- Value (truncate if too long)
        if test.value ~= "" then
            local value_x = 150
            local max_len = 28  -- Fit within screen
            local display_val = test.value
            if #display_val > max_len then
                display_val = display_val:sub(1, max_len - 3) .. "..."
            end
            disp.drawText(value_x, y, display_val, DIM, BG)
        end
        
        y = y + line_height
    end
end

local function draw_auto_mode()
    disp.clear(BG)
    
    -- Header
    pc.ui.drawHeader("System Test Suite")
    
    -- Test results
    draw_test_results()
    
    -- Runtime info (more compact)
    local y = 150
    local runtime = sys.getTimeMs() - start_time
    disp.drawText(4, y, "Runtime: " .. runtime .. " ms", DIM, BG)
    y = y + 10
    
    -- Current time
    disp.drawText(4, y, "Time: " .. sys.getTimeMs() .. " ms", DIM, BG)
    y = y + 10
    
    -- Button state display
    disp.drawText(4, y, "Btns: 0x" .. string.format("%X", button_state), DIM, BG)
    
    -- Last character typed (on right side)
    if last_char ~= "" then
        disp.drawText(160, y, "Key: '" .. last_char .. "'", DIM, BG)
    end
    y = y + 10
    
    -- Instructions (compact)
    pc.ui.drawFooter("ENTER:Rerun ESC:Exit", nil)
end

local function draw_reboot_mode()
    disp.clear(BG)
    
    -- Warning
    local y = 100
    disp.drawText(80, y, "WARNING", WARN, BG)
    y = y + 30
    
    disp.drawText(40, y, "Reboot the system?", FG, BG)
    y = y + 30
    
    disp.drawText(20, y, "This will restart the Pico", DIM, BG)
    y = y + 20
    disp.drawText(20, y, "and return to the launcher.", DIM, BG)
    
    y = y + 40
    disp.drawText(40, y, "ENTER: Reboot now", WARN, BG)
    y = y + 20
    disp.drawText(40, y, "ESC: Cancel", GOOD, BG)
end

-- ── Main Loop ─────────────────────────────────────────────────────────────────

start_time = sys.getTimeMs()

-- Run all tests once at startup
run_all_tests()

while true do
    input.update()
    local pressed = input.getButtonsPressed()
    button_state = input.getButtons()
    
    -- Check for character input
    local ch = input.getChar()
    if ch then
        last_char = ch
    end
    
    -- Mode handling
    if mode == "auto" then
        -- ESC: exit
        if pressed & input.BTN_ESC ~= 0 then
            return
        end
        
        -- ENTER: rerun tests
        if pressed & input.BTN_ENTER ~= 0 then
            run_all_tests()
            last_char = ""  -- Clear on test rerun
        end
        
        -- MENU: go to reboot confirmation
        if pressed & input.BTN_MENU ~= 0 then
            mode = "reboot_confirm"
        end
        
        draw_auto_mode()
        
    elseif mode == "reboot_confirm" then
        -- ESC: cancel
        if pressed & input.BTN_ESC ~= 0 then
            mode = "auto"
        end
        
        -- ENTER: actually reboot
        if pressed & input.BTN_ENTER ~= 0 then
            disp.clear(BG)
            disp.drawText(80, 150, "Rebooting...", WARN, BG)
            disp.flush()
            sys.sleep(500)
            sys.reboot()  -- This will never return
        end
        
        draw_reboot_mode()
    end
    
    disp.flush()
    sys.sleep(16)  -- ~60fps
end
