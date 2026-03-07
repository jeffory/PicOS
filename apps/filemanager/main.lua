-- File Manager for PicOS
-- Dual-panel Norton Commander-style file manager
-- Requires: root-filesystem

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local fs    = pc.fs
local ui    = pc.ui
local gfx   = pc.graphics
local sys   = pc.sys

-- ── Display constants ─────────────────────────────────────────────────────────
local CHAR_W      = 6
local CHAR_H      = 8
local SW, SH      = 320, 320
local PANEL_CHARS = 26          -- chars per panel (160 / 6 ≈ 26)
local DIVIDER_X   = 159         -- 1-pixel divider column
local HDR_H       = 12          -- path header height (px)
local ENTRY_Y0    = 12          -- first file-row top y
local FOOTER_Y    = 308         -- footer top y
local VISIBLE_ROWS = 37         -- (308-12)/8 = 37 rows

-- ── Colours ──────────────────────────────────────────────────────────────────
local BG        = disp.BLACK
local WHITE     = disp.WHITE
local CYAN      = disp.CYAN
local YELLOW    = disp.YELLOW
local GREEN     = disp.GREEN
local GRAY      = disp.GRAY
local HDR_ACT   = disp.rgb(0, 80, 160)   -- active-panel header
local HDR_INACT = disp.rgb(0, 30,  60)   -- inactive-panel header
local SEL_BG    = disp.rgb(0, 80,   0)   -- cursor highlight
local FOOT_BG   = disp.rgb(30, 30, 30)   -- footer background
local HELP_BG   = disp.rgb(0,  0,  40)   -- help screen background

-- ── State machine ─────────────────────────────────────────────────────────────
local ST = {BROWSE=1, IMG_VIEW=2, TXT_VIEW=3, HELP=4, MP3_VIEW=5}

-- ── File-type extension sets ──────────────────────────────────────────────────
-- Image extensions trigger the image viewer; everything else uses the text viewer
local IMG_EXT = {jpg=true,jpeg=true,png=true,bmp=true,gif=true}
local MP3_EXT = {mp3=true}

-- ── App state ────────────────────────────────────────────────────────────────
local state  = ST.BROWSE
local active = 1  -- 1 = left panel, 2 = right panel

-- Panel: {path, entries, cursor (1-based), scroll (0-based), marks}
local panels = {
  {path="/", entries={}, cursor=1, scroll=0, marks={}},
  {path="/", entries={}, cursor=1, scroll=0, marks={}},
}

-- Image viewer
local img_obj  = nil
local img_name = ""
local img_files = {}       -- list of image files in current directory
local img_index = 1        -- current index in img_files
local img_dir = ""         -- directory being browsed
local img_panel = 1        -- which panel (1 or 2) was active when opening image

-- MP3 player
local mp3_player      = nil
local mp3_name        = ""
local mp3_vol         = 80    -- default 80% (scale 0-100)
local mp3_sample_rate = 44100 -- updated after load; used to convert samples→seconds

-- Text viewer
local TXT_COLS = 53   -- 320 / 6 = 53 chars
local TXT_ROWS = 37   -- (308 - 12) / 8 = 37 visible rows
local txt_lines  = {}
local txt_scroll = 0
local txt_title  = ""

-- Status bar
local status_msg = ""
local status_ttl = 0   -- frames remaining

-- ── Utilities ────────────────────────────────────────────────────────────────
local function path_join(dir, name)
  if dir == "/" then return "/"..name end
  return dir.."/"..name
end

local function path_parent(p)
  if p == "/" then return "/" end
  local s = p:match("^(.*)/[^/]+$")
  return (s == nil or s == "") and "/" or s
end

local function file_ext(name)
  local e = name:match("%.([^%.]+)$")
  return e and e:lower() or ""
end

local function fmt_size(sz)
  if sz < 1024       then return string.format("%5d",  sz) end
  if sz < 1048576    then return string.format("%4dK",  sz // 1024) end
  if sz < 1073741824 then return string.format("%4dM",  sz // 1048576) end
  return                      string.format("%4dG",  sz // 1073741824)
end

-- Return s padded with spaces or truncated to exactly n chars.
local function pad(s, n)
  if #s == n then return s end
  if #s >  n then return s:sub(1, n-1).."~" end
  return s..string.rep(" ", n - #s)
end

local function set_status(msg)
  status_msg = msg
  status_ttl = 150   -- ~2.5 s at 60 fps
end

local function fmt_diskspace(kb)
  if kb < 0 then return "?" end
  if kb >= 1024*1024 then return string.format("%dGB", kb // (1024*1024)) end
  if kb >= 1024       then return string.format("%dMB", kb // 1024) end
  return string.format("%dKB", kb)
end

-- ── Directory loading ─────────────────────────────────────────────────────────
local function load_dir(p)
  local raw  = fs.listDir(p.path)
  local dirs, files = {}, {}
  for _, e in ipairs(raw) do
    if e.name ~= "." then
      if e.is_dir then dirs[#dirs+1] = e else files[#files+1] = e end
    end
  end
  local ci = function(a, b) return a.name:lower() < b.name:lower() end
  table.sort(dirs,  ci)
  table.sort(files, ci)

  local entries = {}
  if p.path ~= "/" then entries[1] = {name="..", is_dir=true, size=0} end
  for _, e in ipairs(dirs)  do entries[#entries+1] = e end
  for _, e in ipairs(files) do entries[#entries+1] = e end

  p.entries = entries
  p.marks   = {}
  if p.cursor < 1            then p.cursor = 1 end
  if p.cursor > #entries     then p.cursor = math.max(1, #entries) end
end

local function clamp_scroll(p)
  local n = #p.entries
  local max_scroll = math.max(0, n - VISIBLE_ROWS)
  if p.cursor < p.scroll + 1           then p.scroll = p.cursor - 1 end
  if p.cursor > p.scroll + VISIBLE_ROWS then p.scroll = p.cursor - VISIBLE_ROWS end
  if p.scroll < 0          then p.scroll = 0 end
  if p.scroll > max_scroll then p.scroll = max_scroll end
end

-- ── Drawing: panels ───────────────────────────────────────────────────────────
local function draw_panel(pi)
  local p   = panels[pi]
  local ox  = (pi == 1) and 0 or 160
  local pw  = (pi == 1) and 159 or 160

  -- Header
  local hbg = (pi == active) and HDR_ACT or HDR_INACT
  disp.fillRect(ox, 0, pw, HDR_H, hbg)
  disp.drawText(ox + 1, 2, pad(p.path, PANEL_CHARS), WHITE, hbg)

  -- File rows
  local n = #p.entries
  for row = 0, VISIBLE_ROWS - 1 do
    local idx = p.scroll + row + 1
    local y   = ENTRY_Y0 + row * CHAR_H
    if idx > n then
      disp.fillRect(ox, y, pw, CHAR_H, BG)
    else
      local e         = p.entries[idx]
      local is_cursor = (idx == p.cursor) and (pi == active)
      local is_marked = p.marks[idx] == true
      local bg = is_cursor and SEL_BG or BG
      local fg
      if is_cursor then
        fg = WHITE
      elseif e.name == ".." then
        fg = GRAY
      elseif e.is_dir then
        fg = CYAN
      elseif is_marked then
        fg = YELLOW
      else
        fg = WHITE
      end

      disp.fillRect(ox, y, pw, CHAR_H, bg)

      if e.name == ".." then
        disp.drawText(ox + 1, y, pad("[..]", PANEL_CHARS), fg, bg)
      elseif e.is_dir then
        local label = "[" .. pad(e.name, PANEL_CHARS - 2) .. "]"
        disp.drawText(ox + 1, y, label, fg, bg)
      else
        -- Name (20 chars) then size (6 chars = space + 5-char fmt)
        local name_str = pad(e.name, 20)
        local sz_str   = " " .. fmt_size(e.size)
        local sz_fg    = is_cursor and WHITE or GREEN
        disp.drawText(ox + 1,               y, name_str, fg,    bg)
        disp.drawText(ox + 1 + 20 * CHAR_W, y, sz_str,   sz_fg, bg)
      end
    end
  end
end

local function draw_divider()
  disp.drawLine(DIVIDER_X, 0, DIVIDER_X, FOOTER_Y - 1, GRAY)
end

local function draw_footer()
  disp.fillRect(0, FOOTER_Y, SW, SH - FOOTER_Y, FOOT_BG)
  -- Item count (right-aligned)
  local count_str = string.format("%d items", #panels[active].entries)
  local cx = SW - #count_str * CHAR_W - 2
  disp.drawText(cx, FOOTER_Y + 2, count_str, GRAY, FOOT_BG)
  -- Hints or status message (left)
  if status_ttl > 0 then
    disp.drawText(1, FOOTER_Y + 2, pad(status_msg, 38), YELLOW, FOOT_BG)
  else
    disp.drawText(1, FOOTER_Y + 2,
      pad("F1:Help F5:Cp F6:Ren F7:Mk F8:Del Esc", 38), GRAY, FOOT_BG)
  end
end

local function draw_browse()
  draw_panel(1)
  draw_panel(2)
  draw_divider()
  draw_footer()
end

-- ── Drawing: image viewer ─────────────────────────────────────────────────────
local function draw_img_view()
  disp.fillRect(0, 0, SW, SH, BG)

  -- Header
  disp.fillRect(0, 0, SW, HDR_H, HDR_ACT)
  local nav_info = (#img_files > 1) and string.format(" [%d/%d]", img_index, #img_files) or ""
  local iw, ih = 0, 0
  if img_obj then
    iw, ih = img_obj:getSize()
  end
  local title = string.format("%s  %dx%d%s", img_name, iw, ih, nav_info)
  disp.drawText(1, 2, pad(title, 53), WHITE, HDR_ACT)

  if not img_obj then
    disp.drawText(80, 150, "Failed to load image", disp.RED, BG)
    disp.drawText(80, 165, img_name, GRAY, BG)
  else
    -- Image area: y=12..309 (298 px high), footer hint at y=310
    local avail_w = SW
    local avail_h = SH - HDR_H - 10
    local scale = math.min(avail_w / iw, avail_h / ih)

    if scale < 1.0 then
      local dw = math.floor(iw * scale)
      local dh = math.floor(ih * scale)
      local ox = (SW - dw) // 2
      local oy = HDR_H + (avail_h - dh) // 2
      img_obj:drawScaled(ox, oy, scale)
    else
      local ox = (SW - iw) // 2
      local oy = HDR_H + (avail_h - ih) // 2
      img_obj:draw(ox, oy)
    end
  end

  disp.fillRect(0, SH - 10, SW, 10, FOOT_BG)
  if #img_files > 1 then
    disp.drawText(1, SH - 9, "< >: Prev/Next  Esc: close", GRAY, FOOT_BG)
  else
    disp.drawText(1, SH - 9, "Esc or Enter: close", GRAY, FOOT_BG)
  end
end

-- ── Drawing: text viewer ──────────────────────────────────────────────────────
local function draw_txt_view()
  disp.fillRect(0, 0, SW, SH, BG)

  -- Header
  disp.fillRect(0, 0, SW, HDR_H, HDR_ACT)
  local hdr = string.format("%s  (%d/%d lines)", txt_title, txt_scroll + 1, #txt_lines)
  disp.drawText(1, 2, pad(hdr, 53), WHITE, HDR_ACT)

  -- Text rows
  for r = 0, TXT_ROWS - 1 do
    local li = txt_scroll + r + 1
    local y  = ENTRY_Y0 + r * CHAR_H
    if li <= #txt_lines then
      disp.drawText(0, y, pad(txt_lines[li], TXT_COLS), WHITE, BG)
    else
      disp.fillRect(0, y, SW, CHAR_H, BG)
    end
  end

  -- Footer
  disp.fillRect(0, SH - 10, SW, 10, FOOT_BG)
  disp.drawText(1, SH - 9, "Up/Dn: line   L/R: page   Esc: close", GRAY, FOOT_BG)
end

-- ── Drawing: help screen ──────────────────────────────────────────────────────
local HELP_LINES = {
  "  File Manager - Keyboard Reference",
  "",
  "  Up / Down     Move cursor one step",
  "  Left / Right  Move cursor 10 steps",
  "  Enter         Enter dir / view file",
  "  Backspace     Go up one directory",
  "  Tab           Switch active panel",
  "  Space         Mark / unmark file",
  "",
  "  F1            This help screen",
  "  F3            View (image, MP3, or text)",
  "  F5            Copy to other panel",
  "  F6            Move/Rename",
  "  F7            Create directory",
  "  F8 / Del      Delete file",
  "  Esc           Exit to launcher",
  "",
  "  Enter/F3       Play/pause MP3 audio",
  "",
  "  Directories shown in cyan.",
  "  Marked files shown in yellow.",
  "",
  "  Press any key to close.",
}

local function draw_help()
  disp.fillRect(0, 0, SW, SH, HELP_BG)
  disp.fillRect(0, 0, SW, HDR_H, HDR_ACT)
  disp.drawText(1, 2, "Help", WHITE, HDR_ACT)
  for i, line in ipairs(HELP_LINES) do
    if i > 38 then break end
    disp.drawText(0, HDR_H + (i - 1) * CHAR_H, line, WHITE, HELP_BG)
  end
end

-- ── Drawing: MP3 player ───────────────────────────────────────────────────────
local function fmt_time(s)
  return string.format("%d:%02d", s // 60, s % 60)
end

local function draw_mp3_view()
  disp.fillRect(0, 0, SW, SH, BG)

  -- Header
  disp.fillRect(0, 0, SW, HDR_H, HDR_ACT)
  disp.drawText(1, 2, pad("Now Playing", 53), WHITE, HDR_ACT)

  -- Filename
  disp.drawText(0, 24, pad(mp3_name, TXT_COLS), CYAN, BG)

  -- Status
  local playing  = mp3_player and mp3_player:isPlaying()
  local pos_samp = mp3_player and mp3_player:getPosition() or 0
  local pos_sec  = pos_samp // mp3_sample_rate
  local st_str, st_col
  if playing then
    st_str, st_col = "PLAYING",  GREEN
  elseif pos_samp > 0 and not playing then
    st_str, st_col = "PAUSED",   YELLOW
  else
    st_str, st_col = "STOPPED",  GRAY
  end
  local sx = math.max(0, (SW - #st_str * CHAR_W) // 2)
  disp.drawText(sx, 48, st_str, st_col, BG)

  -- Time (elapsed only; getLength() always returns 0 so no end time)
  local t_str = fmt_time(pos_sec)
  local tx = math.max(0, (SW - #t_str * CHAR_W) // 2)
  disp.drawText(tx, 72, t_str, WHITE, BG)

  -- Volume label (0-100 scale)
  local v_str = string.format("Volume: %d%%", mp3_vol)
  local vx = math.max(0, (SW - #v_str * CHAR_W) // 2)
  disp.drawText(vx, 88, v_str, WHITE, BG)

  -- Volume bar
  local vbx, vby, vbw, vbh = 20, 100, SW - 40, 8
  disp.drawRect(vbx, vby, vbw, vbh, GRAY)
  local vfill = math.floor(vbw * mp3_vol / 100)
  if vfill > 0 then disp.fillRect(vbx, vby, vfill, vbh, CYAN) end

  -- Footer
  disp.fillRect(0, FOOTER_Y, SW, SH - FOOTER_Y, FOOT_BG)
  disp.drawText(1, FOOTER_Y + 2,
    pad("Space/Enter:Play/Pause  Up/Dn:Vol  Esc:Stop", 53), GRAY, FOOT_BG)
end

-- ── Commands ─────────────────────────────────────────────────────────────────
local function cmd_copy()
  local p = panels[active]
  if #p.entries == 0 then return end
  local e = p.entries[p.cursor]
  if e.name == ".." or e.is_dir then set_status("Cannot copy directories"); return end

  local src     = path_join(p.path, e.name)
  local other   = (active == 1) and 2 or 1
  local dst_dir = panels[other].path
  local dst     = path_join(dst_dir, e.name)
  if src == dst then set_status("Source = destination; skipped"); return end

  if not ui.confirm("Copy " .. e.name .. "\nto: " .. dst_dir .. "?") then return end

  -- Restore browse view before blocking copy
  set_status("Copying: " .. e.name)
  draw_browse(); disp.flush()

  local last_pct = -1
  local function on_progress(done, total)
    if total == 0 then return end
    local pct = done * 100 // total
    if pct == last_pct then return end
    last_pct = pct
    status_msg = string.format("Copying %d%%: %s", pct, e.name)
    status_ttl = 5
    draw_footer(); disp.flush()
  end

  local ok, err = fs.copy(src, dst, on_progress)
  if ok then
    set_status("Copied: " .. e.name)
    load_dir(panels[other])
  else
    set_status("Copy failed: " .. (err or "?"))
  end
end

local function cmd_mkdir()
  local p = panels[active]
  local name = ui.textInput("New directory name:", "")
  if not name or name == "" then return end
  if fs.mkdir(path_join(p.path, name)) then
    set_status("Created: " .. name); load_dir(p)
  else
    set_status("mkdir failed")
  end
end

local function cmd_rename()
  local p = panels[active]
  if #p.entries == 0 then return end
  local e = p.entries[p.cursor]
  if e.name == ".." then return end
  local newname = ui.textInput("Rename to:", e.name)
  if not newname or newname == "" or newname == e.name then return end
  local ok, err = fs.rename(path_join(p.path, e.name), path_join(p.path, newname))
  if ok then
    set_status("Renamed: " .. e.name .. " -> " .. newname)
    load_dir(p)
    for i, x in ipairs(p.entries) do
      if x.name == newname then p.cursor = i; clamp_scroll(p); break end
    end
  else
    set_status("Rename failed: " .. (err or "?"))
  end
end

local function cmd_delete()
  local p = panels[active]
  if #p.entries == 0 then return end
  local e = p.entries[p.cursor]
  if e.name == ".." then return end
  local kind = e.is_dir and "directory" or "file"
  if not ui.confirm("Delete " .. kind .. ":\n" .. e.name) then return end
  local ok, err = fs.delete(path_join(p.path, e.name))
  if ok then
    if p.cursor > 1 then p.cursor = p.cursor - 1 end
    set_status("Deleted: " .. e.name); load_dir(p)
  else
    set_status("Delete failed: " .. (err or "?"))
  end
end

local function open_img_view(path, fname)
  -- Free previous image if exists
  if img_obj then
    img_obj = nil
    collectgarbage()
  end

  -- Scan current directory for images to allow navigation
  local dir = path:match("^(.*)/")
  if not dir then dir = "/" end
  img_dir = dir
  img_panel = active  -- remember which panel was active
  img_files = {}
  local raw = fs.listDir(dir) or {}
  for _, e in ipairs(raw) do
    if not e.is_dir and IMG_EXT[file_ext(e.name)] then
      img_files[#img_files + 1] = e.name
    end
  end
  table.sort(img_files, function(a, b) return a:lower() < b:lower() end)

  -- Find current index
  img_index = 1
  for i, name in ipairs(img_files) do
    if name == fname then
      img_index = i
      break
    end
  end

  local ok, img = pcall(function() return gfx.image.load(path) end)
  if not ok or not img then
    collectgarbage()
    set_status("Cannot load: " .. fname)
    return
  end
  img_obj  = img
  img_name = fname
  state = ST.IMG_VIEW
end

local function open_txt_view(path, fname)
  local sz = fs.size(path)
  if sz < 0 then
    set_status("Cannot access: " .. fname)
    return
  end

  local content
  local CAP = 131072  -- 128 KB
  if sz <= CAP then
    content = fs.readFile(path) or ""
  else
    local f = fs.open(path, "r")
    if not f then
      set_status("Cannot open: " .. fname)
      return
    end
    content = (fs.read(f, CAP) or "") .. "\n[file truncated at 128 KB]"
    fs.close(f)
  end

  -- Split into lines; wrap at TXT_COLS
  txt_lines = {}
  for line in (content .. "\n"):gmatch("([^\n]*)\n") do
    line = line:gsub("\t", "    ")
    if #line == 0 then
      txt_lines[#txt_lines + 1] = ""
    else
      repeat
        txt_lines[#txt_lines + 1] = line:sub(1, TXT_COLS)
        line = line:sub(TXT_COLS + 1)
      until line == ""
    end
  end

  txt_scroll = 0
  txt_title  = fname
  state = ST.TXT_VIEW
end

local function open_mp3_view(path, fname)
  if mp3_player then mp3_player:stop(); mp3_player = nil end
  local p = picocalc.sound.mp3player()
  local loaded, err = p:load(path)
  if not loaded then
    set_status("Cannot play: " .. (err or fname))
    return
  end
  local sr = p:getSampleRate()
  mp3_sample_rate = (sr and sr > 0) and sr or 44100
  p:setVolume(mp3_vol)
  p:play(1)           -- play once (no loop)
  mp3_player = p
  mp3_name   = fname
  state = ST.MP3_VIEW
end

local function cmd_view()
  local p = panels[active]
  if #p.entries == 0 then return end
  local e = p.entries[p.cursor]
  if e.is_dir then return end
  local path = path_join(p.path, e.name)
  if IMG_EXT[file_ext(e.name)] then
    open_img_view(path, e.name)
  elseif MP3_EXT[file_ext(e.name)] then
    open_mp3_view(path, e.name)
  else
    open_txt_view(path, e.name)
  end
end

local function cmd_open()
  local p = panels[active]
  if #p.entries == 0 then return end
  local e = p.entries[p.cursor]
  if e.is_dir then
    local old_name
    if e.name == ".." then
      old_name = p.path:match("[^/]+$") or ""
      p.path   = path_parent(p.path)
    else
      p.path = path_join(p.path, e.name)
    end
    p.cursor = 1
    p.scroll = 0
    load_dir(p)
    -- Try to restore cursor to the directory we came from
    if old_name then
      for i, x in ipairs(p.entries) do
        if x.name == old_name then
          p.cursor = i
          clamp_scroll(p)
          break
        end
      end
    end
  else
    local path = path_join(p.path, e.name)
    if IMG_EXT[file_ext(e.name)] then
      open_img_view(path, e.name)
    elseif MP3_EXT[file_ext(e.name)] then
      open_mp3_view(path, e.name)
    else
      open_txt_view(path, e.name)
    end
  end
end

local function cmd_go_up()
  local p = panels[active]
  local par = path_parent(p.path)
  if par == p.path then return end  -- already at root
  local old_name = p.path:match("[^/]+$") or ""
  p.path = par
  load_dir(p)
  for i, e in ipairs(p.entries) do
    if e.name == old_name then
      p.cursor = i
      clamp_scroll(p)
      break
    end
  end
end

-- ── Input handlers ────────────────────────────────────────────────────────────
local function handle_browse(pressed)
  local p = panels[active]
  local n = #p.entries

  if     pressed & input.BTN_UP        ~= 0 then
    p.cursor = math.max(1, p.cursor - 1);  clamp_scroll(p)
  elseif pressed & input.BTN_DOWN      ~= 0 then
    p.cursor = math.min(n, p.cursor + 1);  clamp_scroll(p)
  elseif pressed & input.BTN_LEFT      ~= 0 then
    p.cursor = math.max(1, p.cursor - 10); clamp_scroll(p)
  elseif pressed & input.BTN_RIGHT     ~= 0 then
    p.cursor = math.min(n, p.cursor + 10); clamp_scroll(p)
  elseif pressed & input.BTN_ENTER     ~= 0 then cmd_open()
  elseif pressed & input.BTN_BACKSPACE ~= 0 then cmd_go_up()
  elseif pressed & input.BTN_TAB       ~= 0 then
    active = (active == 1) and 2 or 1
  elseif pressed & input.BTN_F1 ~= 0 then state = ST.HELP
  elseif pressed & input.BTN_F3 ~= 0 then cmd_view()
  elseif pressed & input.BTN_F5 ~= 0 then cmd_copy()
  elseif pressed & input.BTN_F6 ~= 0 then cmd_rename()
  elseif pressed & input.BTN_F7 ~= 0 then cmd_mkdir()
  elseif (pressed & input.BTN_F8  ~= 0) or
         (pressed & input.BTN_DEL ~= 0) then cmd_delete()
  elseif pressed & input.BTN_ESC ~= 0 then
    return false  -- exit to launcher
  end

  -- Space toggles mark (printable key, comes via getChar)
  local ch = input.getChar()
  if ch == " " then
    local e = p.entries[p.cursor]
    if e and e.name ~= ".." then
      p.marks[p.cursor] = (not p.marks[p.cursor]) or nil
    end
  end

  return true
end

local function handle_img_view(pressed)
  input.getChar()   -- drain char buffer

  -- Navigate between images in directory
  if #img_files > 1 then
    if pressed & input.BTN_LEFT ~= 0 then
      img_index = img_index - 1
      if img_index < 1 then img_index = #img_files end
      local new_path = path_join(img_dir, img_files[img_index])
      local new_name = img_files[img_index]
      -- Free current and load next
      img_obj = nil
      collectgarbage()
      local ok, img = pcall(function() return gfx.image.load(new_path) end)
      if ok and img then
        img_obj = img
        img_name = new_name
      else
        collectgarbage()
        img_obj = nil
        img_name = "Error: " .. new_name
      end
      -- Sync panel cursor
      local p = panels[img_panel]
      for i, e in ipairs(p.entries) do
        if e.name == new_name then
          p.cursor = i
          clamp_scroll(p)
          break
        end
      end
      return
    elseif pressed & input.BTN_RIGHT ~= 0 then
      img_index = img_index + 1
      if img_index > #img_files then img_index = 1 end
      local new_path = path_join(img_dir, img_files[img_index])
      local new_name = img_files[img_index]
      -- Free current and load next
      img_obj = nil
      collectgarbage()
      local ok, img = pcall(function() return gfx.image.load(new_path) end)
      if ok and img then
        img_obj = img
        img_name = new_name
      else
        collectgarbage()
        img_obj = nil
        img_name = "Error: " .. new_name
      end
      -- Sync panel cursor
      local p = panels[img_panel]
      for i, e in ipairs(p.entries) do
        if e.name == new_name then
          p.cursor = i
          clamp_scroll(p)
          break
        end
      end
      return
    end
  end

  if pressed & input.BTN_ESC   ~= 0 or
     pressed & input.BTN_ENTER ~= 0 or
     pressed & input.BTN_F3    ~= 0 then
    img_obj = nil
    img_files = {}
    img_index = 1
    img_dir = ""
    collectgarbage()
    state = ST.BROWSE
  end
end

local function handle_txt_view(pressed)
  local max_s = math.max(0, #txt_lines - TXT_ROWS)
  if     pressed & input.BTN_UP    ~= 0 then txt_scroll = math.max(0,     txt_scroll - 1)
  elseif pressed & input.BTN_DOWN  ~= 0 then txt_scroll = math.min(max_s, txt_scroll + 1)
  elseif pressed & input.BTN_LEFT  ~= 0 then txt_scroll = math.max(0,     txt_scroll - TXT_ROWS)
  elseif pressed & input.BTN_RIGHT ~= 0 then txt_scroll = math.min(max_s, txt_scroll + TXT_ROWS)
  elseif pressed & input.BTN_ESC   ~= 0 or
         pressed & input.BTN_F3    ~= 0 then
    txt_lines = {}
    state = ST.BROWSE
  end
end

local function handle_help(pressed)
  if pressed ~= 0 then
    input.getChar()  -- drain
    state = ST.BROWSE
  end
end

local function handle_mp3_view(pressed)
  local ch = input.getChar()
  if pressed & input.BTN_ESC ~= 0 then
    mp3_player:stop(); mp3_player = nil; state = ST.BROWSE; return
  end
  if pressed & input.BTN_ENTER ~= 0 or
     pressed & input.BTN_F3    ~= 0 or ch == " " then
    if mp3_player:isPlaying() then mp3_player:pause()
    else                           mp3_player:resume() end
  end
  if pressed & input.BTN_UP ~= 0 then
    mp3_vol = math.min(100, mp3_vol + 10); mp3_player:setVolume(mp3_vol)
  elseif pressed & input.BTN_DOWN ~= 0 then
    mp3_vol = math.max(0,   mp3_vol - 10); mp3_player:setVolume(mp3_vol)
  end
end

-- ── Main loop ─────────────────────────────────────────────────────────────────
load_dir(panels[1])
load_dir(panels[2])

local di = fs.diskInfo()
if di then
  set_status(fmt_diskspace(di.free) .. " free / " .. fmt_diskspace(di.total) .. " total")
end

local running = true
while running do
  input.update()
  local pressed = input.getButtonsPressed()

  -- Input dispatch
  if     state == ST.BROWSE   then if not handle_browse(pressed) then running = false end
  elseif state == ST.IMG_VIEW then handle_img_view(pressed)
  elseif state == ST.TXT_VIEW then handle_txt_view(pressed)
  elseif state == ST.HELP     then handle_help(pressed)
  elseif state == ST.MP3_VIEW then handle_mp3_view(pressed)
  end

  -- Status timer
  if status_ttl > 0 then
    status_ttl = status_ttl - 1
    if status_ttl == 0 then status_msg = "" end
  end

  -- Draw
  if state == ST.IMG_VIEW then
    draw_img_view()
  elseif state == ST.TXT_VIEW then
    draw_txt_view()
  elseif state == ST.HELP then
    draw_help()
  elseif state == ST.MP3_VIEW then
    draw_mp3_view()
  else
    draw_browse()
  end

  disp.flush()
  sys.sleep(16)
end
