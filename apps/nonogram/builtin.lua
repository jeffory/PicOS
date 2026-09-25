-- builtin.lua — puzzles that ship with the app, so it is playable before any
-- puzzle has been created, imported or downloaded.
--
-- Two sources, both verified unique by tests/run_host.lua:
--
--   `raw` below   hand-authored recognisable art (an arrow, a key, a skull...)
--   generated.lua 120 procedurally generated abstract grids
--
-- Hand-authored puzzles sort first within each size, because recognisable art is
-- more satisfying to solve and makes a better first impression than abstract
-- blobs.
--
-- Grids are written as strings for legibility: '#' is filled, anything else is
-- empty. Row 1 is the top.

local Clues = require("clues")

local BI = {}

-- Every entry here is verified by tests/run_host.lua to have a UNIQUE solution.
-- That check exists because it caught three of the first six puzzles written for
-- this file: symmetric line-art (an X, a diamond, a spider) very often admits a
-- second grid with identical clues, and the ambiguity is invisible by eye.
local raw = {
    {
        name = "UPLINK", size = 5,
        rows = {
            "..#..",
            "..#..",
            "#####",
            "..#..",
            "..#..",
        },
    },
    {
        name = "ARROW", size = 5,
        rows = {
            "..#..",
            ".###.",
            "#.#.#",
            "..#..",
            "..#..",
        },
    },
    {
        name = "KEY", size = 5,
        rows = {
            "###..",
            "#.#..",
            "###..",
            "..#..",
            "..##.",
        },
    },
    {
        name = "WAVE", size = 5,
        rows = {
            "#..##",
            ".##.#",
            "##..#",
            "#..##",
            ".####",
        },
    },
    {
        name = "MASK", size = 10,
        rows = {
            "..######..",
            ".########.",
            "##.####.##",
            "##.####.##",
            "##########",
            "##########",
            ".##....##.",
            "..######..",
            "...####...",
            "....##....",
        },
    },
    {
        name = "CIRCUIT", size = 10,
        rows = {
            "#...#...#.",
            "#####.###.",
            "#...#...#.",
            "#.#####.##",
            "#.#...#...",
            "###.#####.",
            "..#.#...#.",
            "####.####.",
            "...#.#...#",
            "#####.####",
        },
    },
    {
        name = "DRONE", size = 10,
        rows = {
            "##......##",
            "###....###",
            ".########.",
            "..######..",
            "..#.##.#..",
            ".##.##.##.",
            "####..####",
            "..#....#..",
            ".##....##.",
            "##......##",
        },
    },
    {
        name = "ICE WALL", size = 15,
        rows = {
            "###############",
            "#.............#",
            "#.###########.#",
            "#.#.........#.#",
            "#.#.#######.#.#",
            "#.#.#.....#.#.#",
            "#.#.#.###.#.#.#",
            "#.#.#.#.#.#.#.#",
            "#.#.#.###.#.#.#",
            "#.#.#.....#.#.#",
            "#.#.#######.#.#",
            "#.#.........#.#",
            "#.###########.#",
            "#.............#",
            "###############",
        },
    },
    {
        name = "SKULL", size = 15,
        rows = {
            "...#########...",
            "..###########..",
            ".#############.",
            "###############",
            "##.##.....##.##",
            "#...#.....#...#",
            "#...#.....#...#",
            "##.##.....##.##",
            "###############",
            "###############",
            ".##.#.#.#.#.##.",
            ".##.#.#.#.#.##.",
            "..###########..",
            "...#.#.#.#.#...",
            "....#.#.#.#....",
        },
    },
    -- 20x20 exists to exercise the scrolling viewport: 20 rows of column clues
    -- leave under the 12px-per-row floor, so this is the one size that cannot
    -- fit the screen whole. Verifies as "guess" rather than "unique-line" —
    -- still exactly one solution, but it needs one guess to reach.
    {
        name = "MAINFRAME", size = 20,
        rows = {
            "####....####....####",
            "#..#....#..#....#..#",
            "#..#....#..#....#..#",
            "####....####....####",
            "....#..#....#..#....",
            "....#..#....#..#....",
            "####....####....####",
            "#..#....#..#....#..#",
            "#..#....#..#....#..#",
            "####....####....####",
            "....#..#....#..#....",
            "....#..#....#..#....",
            "####....####....####",
            "#..#....#..#....#..#",
            "#..#....#..#....#..#",
            "####....####....####",
            "....#..#....#..#....",
            "....#..#....#..#....",
            "####....####....####",
            "#..#....#..#....#..#",
        },
    },
}

-- Parses a row-string grid into a solution table plus derived clues.
local function build(entry, curated)
    local h = #entry.rows
    local w = #entry.rows[1]

    local solution = {}
    for r = 1, h do
        local line = entry.rows[r]
        if #line ~= w then
            error(("builtin %s: row %d is %d wide, expected %d")
                :format(entry.name, r, #line, w))
        end
        solution[r] = {}
        for c = 1, w do
            solution[r][c] = (line:sub(c, c) == "#") and 1 or 0
        end
    end

    local rowClues, colClues = Clues.derive(solution, w, h)
    return {
        id       = "builtin_" .. entry.name:lower():gsub("%W", "_"),
        name     = entry.name,
        author   = "PicOS",
        w        = w,
        h        = h,
        solution = solution,
        rowClues = rowClues,
        colClues = colClues,
        source   = "builtin",
        curated  = curated or false,
        readonly = true,
    }
end

-- Built once and reused: parsing 130 grids and deriving 130 pairs of clue lists
-- on every menu repaint would be wasteful, and BI.list() is called on each
-- scene enter.
local cache = nil

-- list() -> array of puzzles, smallest first so the menu reads as a difficulty
-- ramp, hand-authored art before generated grids within each size.
function BI.list()
    if cache then return cache end

    local out = {}
    for i = 1, #raw do out[#out + 1] = build(raw[i], true) end

    -- generated.lua is optional: a checkout without it still ships the curated
    -- set rather than failing to launch.
    local ok, gen = pcall(require, "generated")
    if ok and type(gen) == "table" then
        for i = 1, #gen do out[#out + 1] = build(gen[i], false) end
    end

    table.sort(out, function(a, b)
        if a.w ~= b.w then return a.w < b.w end
        if a.curated ~= b.curated then return a.curated end
        return a.name < b.name
    end)

    cache = out
    return out
end

-- Distinct grid sizes present, ascending — drives the menu's size filter.
function BI.sizes()
    local seen, out = {}, {}
    for _, p in ipairs(BI.list()) do
        if not seen[p.w] then seen[p.w] = true; out[#out + 1] = p.w end
    end
    table.sort(out)
    return out
end

function BI.byId(id)
    for _, p in ipairs(BI.list()) do
        if p.id == id then return p end
    end
    return nil
end

return BI
