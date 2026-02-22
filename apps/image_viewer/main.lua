-- Image Viewer for PicoCalc OS
-- Browse directories and view images in /data/image_viewer

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local fs    = pc.fs
local sys   = pc.sys

-- ── Constants ────────────────────────────────────────────────────────────────
local ROOT_PATH    = "/data/image_viewer"
local SCREEN_W     = disp.getWidth()
local SCREEN_H     = disp.getHeight()
local CHAR_W       = 6
local CHAR_H       = 8
local HEADER_H     = 28
local FOOTER_H     = 18
local LIST_Y       = HEADER_H
local LIST_ITEM_H  = 12
local VISIBLE_ROWS = math.floor((SCREEN_H - HEADER_H - FOOTER_H) / LIST_ITEM_H)

-- Colors
local C_BG          = disp.BLACK
local C_TEXT        = disp.WHITE
local C_DIR         = disp.rgb(100, 180, 255)   -- light blue for directories
local C_SEL_BG      = disp.rgb(40, 80, 160)     -- selection highlight
local C_EMPTY       = disp.GRAY

-- ── State ────────────────────────────────────────────────────────────────────
local mode          = "browse"   -- "browse" or "view"
local current_path  = ROOT_PATH
local path_stack    = {}         -- breadcrumb for going back up

-- Browse mode state
local dirs          = {}         -- subdirectory names in current_path
local files         = {}         -- image filenames in current_path
local entries       = {}         -- combined list: {name=, is_dir=}
local browse_sel    = 1          -- selected index in entries
local browse_scroll = 0          -- scroll offset

-- View mode state
local current_image = nil
local current_error = nil
local selected_index = 1         -- index into files[]

-- Key repeat state for browse mode
local repeat_key     = 0         -- which key is being held (BTN_UP or BTN_DOWN)
local repeat_held_ms = 0         -- how long the key has been held (in ms)
local REPEAT_DELAY   = 500       -- ms before repeat starts
local REPEAT_RATE    = 150       -- ms between repeats once active
local repeat_accum   = 0         -- accumulator for repeat rate

-- ── Supported image extensions ───────────────────────────────────────────────
local function is_image(name)
    local lower = string.lower(name)
    return string.sub(lower, -4) == ".jpg"
        or string.sub(lower, -5) == ".jpeg"
        or string.sub(lower, -4) == ".png"
        or string.sub(lower, -4) == ".bmp"
        or string.sub(lower, -4) == ".gif"
end

-- ── Directory scanning ───────────────────────────────────────────────────────
local function scanDirectory(path)
    dirs  = {}
    files = {}
    entries = {}

    local all = fs.listDir(path)
    if not all then return end

    -- Separate dirs and image files
    for _, entry in ipairs(all) do
        if entry.name:sub(1, 1) ~= "." then  -- skip hidden entries
            if entry.is_dir then
                table.insert(dirs, entry.name)
            elseif is_image(entry.name) then
                table.insert(files, entry.name)
            end
        end
    end

    -- Sort each group alphabetically
    table.sort(dirs)
    table.sort(files)

    -- Build combined entry list
    -- Add ".." entry to go up when inside a subdirectory
    if path ~= ROOT_PATH then
        table.insert(entries, {name = "..", is_dir = true})
    end
    -- Directories first, then images
    for _, name in ipairs(dirs) do
        table.insert(entries, {name = name, is_dir = true})
    end
    for _, name in ipairs(files) do
        table.insert(entries, {name = name, is_dir = false})
    end

    browse_sel = 1
    browse_scroll = 0
end

-- Find the browse entry index for a given image filename
local function findEntryIndex(image_name)
    for i, entry in ipairs(entries) do
        if not entry.is_dir and entry.name == image_name then
            return i
        end
    end
    return 1
end

-- Set browse_sel and adjust scroll so it's visible
local function selectBrowseEntry(idx)
    browse_sel = idx
    -- Ensure selection is visible
    if browse_sel <= browse_scroll then
        browse_scroll = browse_sel - 1
    elseif browse_sel > browse_scroll + VISIBLE_ROWS then
        browse_scroll = browse_sel - VISIBLE_ROWS
    end
end

-- ── Image loading ────────────────────────────────────────────────────────────
local function loadCurrentImage()
    if #files == 0 then return end
    current_image = nil
    collectgarbage()

    local filepath = current_path .. "/" .. files[selected_index]
    local status, img_or_err = pcall(pc.graphics.image.load, filepath)
    if status and img_or_err then
        current_image = img_or_err
        current_error = nil
    else
        current_image = "error"
        current_error = tostring(img_or_err)
    end
end

-- ── Get display name for current directory ───────────────────────────────────
local function getDisplayPath()
    if current_path == ROOT_PATH then
        return "Image Viewer"
    end
    -- Show just the last component
    local last = current_path:match("([^/]+)$")
    return last or "Image Viewer"
end

-- ── Draw: Browse mode ────────────────────────────────────────────────────────
local function drawBrowse()
    disp.clear(C_BG)
    pc.ui.drawHeader(getDisplayPath())

    if #entries == 0 then
        disp.drawText(20, 100, "No images or folders", C_EMPTY, C_BG)
        disp.drawText(20, 115, "in " .. current_path, C_EMPTY, C_BG)
    else
        for i = 1, VISIBLE_ROWS do
            local idx = i + browse_scroll
            if idx > #entries then break end

            local entry = entries[idx]
            local y = LIST_Y + (i - 1) * LIST_ITEM_H
            local selected = (idx == browse_sel)

            -- Background highlight
            if selected then
                disp.fillRect(0, y, SCREEN_W, LIST_ITEM_H, C_SEL_BG)
            end

            local bg = selected and C_SEL_BG or C_BG

            -- Prefix and colour
            local prefix = selected and "> " or "  "
            local label, fg
            if entry.is_dir then
                label = entry.name .. "/"
                fg = C_DIR
            else
                label = entry.name
                fg = C_TEXT
            end

            disp.drawText(4, y + 2, prefix .. label, fg, bg)
        end
    end

    -- Footer
    pc.ui.drawFooter("Enter:Open | Esc:Exit")

    disp.flush()
end

-- ── Draw: View mode ──────────────────────────────────────────────────────────
local CONTENT_Y = HEADER_H
local CONTENT_H = SCREEN_H - HEADER_H - FOOTER_H

local function drawView()
    disp.clear(C_BG)

    local file_text = ""
    if #files > 0 then
        file_text = string.format("%d/%d", selected_index, #files)
    end

    if #files == 0 then
        pc.ui.drawHeader("Image Viewer")
        disp.drawText(20, 100, "No images found in", C_TEXT, C_BG)
        disp.drawText(20, 115, current_path, C_TEXT, C_BG)
        pc.ui.drawFooter("Esc:Browse")
    elseif current_image == "error" then
        pc.ui.drawHeader("Image Viewer")
        disp.drawText(20, 80, "Error loading image:", disp.RED, C_BG)
        disp.drawText(20, 95, files[selected_index], C_TEXT, C_BG)
        if current_error then
            disp.drawText(20, 115, current_error, disp.rgb(248, 0, 248), C_BG)
        end
        pc.ui.drawFooter("< > Navigate | Esc:Browse", file_text)
    elseif current_image then
        pc.ui.drawHeader(getDisplayPath())

        -- Scale image to fit within content area (between header and footer)
        local w, h = current_image:getSize()
        local scale_x = SCREEN_W / w
        local scale_y = CONTENT_H / h
        local scale = math.min(scale_x, scale_y)
        if scale > 1 then scale = 1 end

        -- Center within the content area
        local cx = SCREEN_W / 2
        local cy = CONTENT_Y + CONTENT_H / 2
        current_image:drawScaled(cx, cy, scale, 0)

        pc.ui.drawFooter("< > Navigate | Esc:Browse", file_text)
    else
        pc.ui.drawHeader("Image Viewer")
        disp.drawText(100, 100, "Loading...", C_TEXT, C_BG)
        pc.ui.drawFooter("< > Navigate | Esc:Browse", file_text)
    end

    disp.flush()
end

-- ── Enter a subdirectory ─────────────────────────────────────────────────────
local function enterDirectory(dirname)
    table.insert(path_stack, current_path)
    current_path = current_path .. "/" .. dirname
    scanDirectory(current_path)
end

-- ── Go up one directory ──────────────────────────────────────────────────────
local function goUp()
    if #path_stack > 0 then
        current_path = table.remove(path_stack)
        scanDirectory(current_path)
    end
end

-- ── Enter view mode from browse ──────────────────────────────────────────────
local function enterViewMode(image_name)
    -- Find the index of image_name in files[]
    for i, name in ipairs(files) do
        if name == image_name then
            selected_index = i
            break
        end
    end
    mode = "view"
    current_image = nil
    drawView()       -- show loading state
    loadCurrentImage()
    drawView()       -- show loaded image
end

-- ── Initialise ───────────────────────────────────────────────────────────────
scanDirectory(current_path)

-- Always start in view mode showing images from root directory
mode = "view"
selected_index = 1
if #files > 0 then
    drawView()           -- show loading state
    loadCurrentImage()
end
drawView()

-- ── Main loop ────────────────────────────────────────────────────────────────
while true do
    input.update()
    local pressed = input.getButtonsPressed()

    if mode == "browse" then
        -- ── Browse mode controls ─────────────────────────────────────────
        if (pressed & input.BTN_ESC) ~= 0 then
            break   -- exit app from browse mode
        end

        -- Key repeat logic for Up/Down
        local held = input.getButtons()
        local move_up   = false
        local move_down = false

        -- Initial press (edge-triggered)
        if (pressed & input.BTN_UP) ~= 0 then
            move_up = true
            repeat_key = input.BTN_UP
            repeat_held_ms = 0
            repeat_accum = 0
        elseif (pressed & input.BTN_DOWN) ~= 0 then
            move_down = true
            repeat_key = input.BTN_DOWN
            repeat_held_ms = 0
            repeat_accum = 0
        end

        -- Held repeat (after delay)
        if repeat_key ~= 0 and (held & repeat_key) ~= 0 and not move_up and not move_down then
            repeat_held_ms = repeat_held_ms + 50  -- ~50ms per loop iteration
            if repeat_held_ms >= REPEAT_DELAY then
                repeat_accum = repeat_accum + 50
                if repeat_accum >= REPEAT_RATE then
                    repeat_accum = repeat_accum - REPEAT_RATE
                    if repeat_key == input.BTN_UP then
                        move_up = true
                    else
                        move_down = true
                    end
                end
            end
        elseif repeat_key ~= 0 and (held & repeat_key) == 0 then
            -- Key released, reset
            repeat_key = 0
            repeat_held_ms = 0
            repeat_accum = 0
        end

        if move_up and browse_sel > 1 then
            browse_sel = browse_sel - 1
            if browse_sel <= browse_scroll then
                browse_scroll = browse_sel - 1
            end
            drawBrowse()
        end

        if move_down and browse_sel < #entries then
            browse_sel = browse_sel + 1
            if browse_sel > browse_scroll + VISIBLE_ROWS then
                browse_scroll = browse_sel - VISIBLE_ROWS
            end
            drawBrowse()
        end

        if (pressed & input.BTN_ENTER) ~= 0 then
            if #entries > 0 and browse_sel <= #entries then
                local entry = entries[browse_sel]
                if entry.is_dir then
                    if entry.name == ".." then
                        goUp()
                    else
                        enterDirectory(entry.name)
                    end
                    -- After entering dir, go to view mode if there are images
                    if #files > 0 then
                        selected_index = 1
                        mode = "view"
                        current_image = nil
                        drawView()
                        loadCurrentImage()
                        drawView()
                    else
                        drawBrowse()
                    end
                else
                    enterViewMode(entry.name)
                end
            end
        end

    else
        -- ── View mode controls ───────────────────────────────────────────
        if (pressed & input.BTN_ESC) ~= 0 then
            -- Go to browse mode, selecting the current image
            local viewed_name = (#files > 0) and files[selected_index] or nil
            current_image = nil
            collectgarbage()
            mode = "browse"
            scanDirectory(current_path)
            if viewed_name then
                selectBrowseEntry(findEntryIndex(viewed_name))
            end
            drawBrowse()
        end

        if #files > 0 then
            local changed = false
            if (pressed & input.BTN_LEFT) ~= 0 then
                selected_index = selected_index - 1
                if selected_index < 1 then selected_index = #files end
                changed = true
            elseif (pressed & input.BTN_RIGHT) ~= 0 then
                selected_index = selected_index + 1
                if selected_index > #files then selected_index = 1 end
                changed = true
            end

            if changed then
                current_image = nil
                drawView()          -- show loading state
                loadCurrentImage()
                drawView()          -- show loaded image
            end
        end
    end

    sys.sleep(50)
end
