-- runner.lua — Run the user's project from PicoForge

local Runner = {}

-- Read a file into a string using the PicOS fs API
local function read_file(fs, path)
    local f = fs.open(path, "r")
    if not f then return nil, "cannot open " .. path end
    local chunks = {}
    while true do
        local data = fs.read(f, 4096)
        if not data or #data == 0 then break end
        chunks[#chunks + 1] = data
    end
    fs.close(f)
    return table.concat(chunks)
end

function Runner.run(project, fs, disp)
    -- Save all modified buffers first
    project:save_all(fs)

    -- Read main.lua
    local main_path = project.base_path .. "/main.lua"
    local source, err = read_file(fs, main_path)
    if not source then
        return false, err
    end

    -- Create sandboxed environment with access to picocalc globals
    local env = setmetatable({
        APP_DIR = project.base_path,
        APP_NAME = project.name,
        APP_ID = "picoforge.user." .. project.name,
    }, {__index = _G})

    -- Compile
    local fn, compile_err = load(source, "@main.lua", "t", env)
    if not fn then
        return false, "Compile: " .. compile_err
    end

    -- Clear screen for the running app
    disp.clear(0x0000)
    disp.flush()

    -- Execute
    local ok, run_err = pcall(fn)

    -- App returned — no need to restore display here,
    -- the main loop will re-render PicoForge
    if not ok then
        -- Check for exit sentinel
        local msg = tostring(run_err)
        if msg:find("__picocalc_exit__") then
            return true  -- clean exit
        end
        return false, "Runtime: " .. msg
    end

    return true
end

return Runner
