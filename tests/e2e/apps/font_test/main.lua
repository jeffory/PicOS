-- Font fixture. One page per font; Right advances, Esc exits.
-- Pages 0-3 are the byte-exact golden set for the renderer rewrite:
-- do not change what they draw without regenerating the goldens on the
-- ORIGINAL renderer.
local pc, disp, input = picocalc, picocalc.display, picocalc.input
-- First thing this app does: report the font it inherited. The launcher resets
-- the selection to 0 around every app, so a second run must also see 0 even
-- though the previous run exited with 8x12 selected (see the Esc branch).
pc.sys.log(("FT:ENTRY %d"):format(disp.getFont()))
local BLACK, WHITE = 0x0000, 0xFFFF
local YELLOW = disp.rgb(255, 255, 0)
local BLUE   = disp.rgb(0, 0, 255)
local SAMPLE = "The quick brown fox jumps over 13 lazy dogs."

local pages = {
  { id = disp.FONT_6X8,              name = "6x8" },
  { id = disp.FONT_8X12,             name = "8x12" },
  { id = disp.FONT_SCIENTIFICA,      name = "scientifica" },
  { id = disp.FONT_SCIENTIFICA_BOLD, name = "scientifica-bold" },
  { name = "extended", draw = function(p)
      disp.clear(BLACK)
      disp.setFont(disp.FONT_SCIENTIFICA)
      local s = {}
      for c = 0x80, 0x9F do s[#s + 1] = string.char(c) end
      disp.drawText(2, 2, table.concat(s), WHITE, BLACK)
      disp.setFont(disp.FONT_SCIENTIFICA_BOLD)
      disp.drawText(2, 16, table.concat(s), WHITE, BLACK)
      -- 0x7F is in scientifica's 0x20..0x9F range (a blank glyph); 0x01 is not,
      -- so it must render as the hollow fallback box, never as '?'.
      disp.drawText(2, 30, "a\127b\1c", WHITE, BLACK)
      disp.flush()
    end },
  { name = "loaded", draw = function(p)
      disp.clear(BLACK)
      local id = disp.loadFont(APP_DIR .. "/fonts/demo_prop.pfn")
      pc.sys.log("FT:LOAD " .. tostring(id))
      if not id then disp.flush(); return end
      disp.setFont(id)
      local fw, fh = disp.getFontWidth(), disp.getFontHeight()
      local y = 2
      disp.drawText(2, y, ("loaded id %d %dx%d"):format(id, fw, fh), WHITE, BLACK)
      y = y + fh + 2
      local wi = disp.drawText(2, y, "iiiiiiiiii", YELLOW, BLACK)
      local ww = disp.drawText(120, y, "WWWWWWWWWW", YELLOW, BLACK)
      pc.sys.log(("FT:PROP %d %d"):format(wi, ww))
      y = y + fh + 2
      -- Unloading the slot the display is using must self-heal back to font 0.
      disp.unloadFont(id)
      pc.sys.log("FT:UNLOAD " .. tostring(disp.getFont()))
      -- graphics.font path, wrap via the shared helper
      local f = pc.graphics.font.new(APP_DIR .. "/fonts/demo_prop.pfn")
      pc.sys.log(("FT:FONTOBJ %s %d %d %d"):format(f:getName(), f:getWidth(), f:getHeight(),
                                                  f:getTextWidth("iiiiiiiiii")))
      f:drawTextInRect(2, y, 150, 4 * fh, "wrap me across several narrow lines please", 0, WHITE, BLACK)
      pc.graphics.drawTextInRect("graphics wrap in the loaded font too", 160, y, 150, 4 * fh, 0, f)
      local tw, th = pc.graphics.getTextSize("iiiiiiiiii", f)
      pc.sys.log(("FT:SIZE %d %d"):format(tw, th))
      y = y + 4 * fh + 2
      f:drawTextAligned(160, y, "centered", 1, WHITE, BLACK)
      disp.flush()
      -- a bad path must return nil, never raise from display.loadFont
      pc.sys.log("FT:BADLOAD " .. tostring(disp.loadFont(APP_DIR .. "/fonts/nope.pfn")))
      local ok, err = pcall(pc.graphics.font.new, APP_DIR .. "/fonts/nope.pfn")
      pc.sys.log("FT:BADNEW " .. tostring(ok))
      disp.setFont(disp.FONT_6X8)
    end },
}

local function drawBuiltinPage(p)
  disp.clear(BLACK)
  disp.setFont(p.id)
  local fw, fh = disp.getFontWidth(), disp.getFontHeight()
  local y = 2
  disp.drawText(2, y, p.name .. " " .. fw .. "x" .. fh, WHITE, BLACK)
  y = y + fh + 2
  -- Printable ASCII, 32 per row
  for row = 0, 2 do
    local s = {}
    for c = 0x20 + row * 32, math.min(0x7E, 0x20 + row * 32 + 31) do
      s[#s + 1] = string.char(c)
    end
    disp.drawText(2, y, table.concat(s), WHITE, BLACK)
    y = y + fh
  end
  y = y + 4
  -- Opaque vs transparent over a blue bar
  disp.fillRect(0, y, 320, fh, BLUE)
  disp.drawText(2, y, "opaque", WHITE, BLACK)
  disp.drawText(120, y, "transparent", WHITE, false)
  y = y + fh + 4
  -- Clipped: only x 40..119 of the sample survives
  disp.setClipRect(40, y, 80, fh)
  disp.drawText(2, y, SAMPLE, YELLOW, BLACK)
  disp.clearClipRect()
  y = y + fh + 4
  -- Partly off-screen on both sides
  disp.drawText(-7, y, SAMPLE, WHITE, BLACK)
  disp.drawText(300, y + fh, SAMPLE, WHITE, BLACK)
  y = y + 2 * fh + 4
  local drawn = disp.drawText(2, y, SAMPLE, YELLOW, BLACK)
  disp.flush()
  pc.sys.log(("FT:WIDTH %s %d %d"):format(p.name, drawn, disp.textWidth(SAMPLE)))
end

local function drawPage(n)
  local p = pages[n + 1]
  if p.draw then p.draw(p) else drawBuiltinPage(p) end
  pc.sys.log(("FT:PAGE %d %s"):format(n, p.name))
end

local page = 0
drawPage(page)
while true do
  input.update()
  local pressed = input.getButtonsPressed()
  if (pressed & input.BTN_ESC) ~= 0 then
    -- Exit dirty on purpose: the state reset test must measure the launcher and
    -- nothing else. collectgarbage() first so page 5's graphics.font object
    -- hands slot 4 back; the raw id taken below is then slot 4 again, and being
    -- a plain integer nothing can free it - not the GC at lua_close, not
    -- unloadFont. Only font_registry_unload_all() in the launcher can.
    collectgarbage()
    local leak = disp.loadFont(APP_DIR .. "/fonts/demo_prop.pfn")
    disp.setFont(disp.FONT_8X12)
    pc.sys.log(("FT:EXIT font=%s leak=%s"):format(tostring(disp.getFont()),
                                                  tostring(leak)))
    break
  end
  if (pressed & input.BTN_RIGHT) ~= 0 then
    page = (page + 1) % #pages
    drawPage(page)
  end
  pc.sys.sleep(10)
end
pc.sys.log("FT:DONE")
