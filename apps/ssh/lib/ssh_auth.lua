-- ssh_auth.lua — SSH-2 user authentication (RFC 4252)

local wire = require("lib.ssh_wire")

local auth = {}

local MSG = {
    SERVICE_REQUEST  = 5,
    SERVICE_ACCEPT   = 6,
    USERAUTH_REQUEST = 50,
    USERAUTH_FAILURE = 51,
    USERAUTH_SUCCESS = 52,
    USERAUTH_BANNER  = 53,
}

-- Request the "ssh-userauth" service
function auth.request_service(transport)
    local payload = string.char(MSG.SERVICE_REQUEST)
                 .. wire.string("ssh-userauth")
    transport:send_packet(payload)

    local reply = transport:recv_packet_skip(10000)
    if not reply then return false end

    local msg_type = string.byte(reply, 1)
    if msg_type ~= MSG.SERVICE_ACCEPT then
        transport.error = "service request rejected (type=" .. msg_type .. ")"
        return false
    end

    return true
end

-- Attempt password authentication
-- Returns: true on success, false on failure, "banner" + banner_text if banner received
function auth.password(transport, username, password)
    local payload = string.char(MSG.USERAUTH_REQUEST)
                 .. wire.string(username)
                 .. wire.string("ssh-connection")
                 .. wire.string("password")
                 .. wire.boolean(false)  -- no old password
                 .. wire.string(password)
    transport:send_packet(payload)

    -- Read response, potentially multiple messages (banners first)
    local banner_text = nil
    while true do
        local reply = transport:recv_packet_skip(30000)
        if not reply then return false, banner_text end

        local msg_type = string.byte(reply, 1)

        if msg_type == MSG.USERAUTH_SUCCESS then
            return true, banner_text

        elseif msg_type == MSG.USERAUTH_FAILURE then
            local methods = wire.read_namelist(reply, 2)
            transport.error = "authentication failed"
            if methods then
                transport.error = transport.error .. " (methods: " .. table.concat(methods, ",") .. ")"
            end
            return false, banner_text

        elseif msg_type == MSG.USERAUTH_BANNER then
            local msg = wire.read_string(reply, 2)
            banner_text = msg or ""
            -- Continue reading — banner precedes SUCCESS/FAILURE

        else
            transport.error = "unexpected message during auth: " .. msg_type
            return false, banner_text
        end
    end
end

return auth
