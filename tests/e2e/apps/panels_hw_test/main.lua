-- panels_hw_test — rigid scroll comic driving the panels.lua hardware-scroll
-- fast path.  One vertical scroll sequence of three rigid panels (solid
-- backgrounds + static text = no pct- or time-dependent pixels), looping
-- back to itself at the end so the full clamp -> transition -> re-entry
-- cycle is exercised without needing a second sequence.
--
-- maxScroll = 3*250 + 2*8 - 320 = 446px.

local pc   = picocalc
local sys  = pc.sys
local disp = pc.display

local ok, Panels = pcall(sys.loadlib, "panels")
if not ok or type(Panels) ~= "table" then
    sys.log("PHW NOLIB")
    return
end

local comic = {
    name = "PanelsHW",
    sequences = {
        {
            title = "Rigid",
            scrollType = "scroll",
            nextSequence = 1,
            transition = "cut",
            panels = {
                { frame = { height = 250 },
                  backgroundColor = disp.rgb(200, 40, 40),
                  layers = { { text = "TOP", x = 140, y = 20 } } },
                { frame = { height = 250, marginBefore = 8 },
                  backgroundColor = disp.rgb(40, 180, 60),
                  layers = { { text = "MID", x = 140, y = 120 } } },
                { frame = { height = 250, marginBefore = 8 },
                  backgroundColor = disp.rgb(40, 80, 200),
                  layers = { { text = "BOTTOM", x = 130, y = 200 } } },
            },
        },
    },
}

sys.log("PHW START")
local result = Panels.start(comic, { resume = false })
sys.log("PHW EXIT " .. tostring(result))
