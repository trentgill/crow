get_offset = 0x80

function need_events(f)
    if f.getters then return true end
    if f.commands then
        for _,v in ipairs( f.commands ) do
            if v.get == true then return true end
        end
    end
    return false
end

function make_ii(files)

    local function make_helpers(f)

        function make_cmd_alias( device, name, args )
            local cmd = 'crow.ii.'..device..'.'..name..' = function('
            local body = ') crow.send("ii.'..device..'.'..name..'('
            local tail = ')") end\n'

            if not args then -- no args
                cmd = cmd .. body .. tail
            elseif type(args[1]) == 'string' then -- one arg
                cmd = cmd .. args[1] .. body .. '"..' .. args[1] .. '.."' .. tail
            else -- table of args
                for k,v in ipairs(args) do
                    cmd = cmd .. v[1] .. ','
                end
                cmd = cmd:sub(1,-2)
                cmd = cmd .. body .. '"'
                for k,v in ipairs(args) do
                    cmd = cmd .. '..' .. v[1] .. '..","'
                end
                cmd = cmd:sub(1,-6)
                cmd = cmd .. '.."' .. tail
            end
            return cmd
        end

        local h = make_cmd_alias( f.lua_name, 'help' )
        if f.commands then
            for _,v in ipairs( f.commands ) do
                h = h .. make_cmd_alias( f.lua_name, v.name, v.args )
            end
        end

        return h
    end

    local c = 'crow.ii.init = function()\n'
    for _,f in ipairs(files) do
        if need_events(f) then
            c = c .. '  crow.ii.' .. f.lua_name .. '.event = function(i,v) print("'
                  .. f.lua_name
                  .. ' ii: "..i.." "..v) end\n'
        end
    end
    c = c .. 'end\n\n'
    for _,f in ipairs(files) do
        c = c .. 'crow.ii.' .. f.lua_name .. ' = {}\n'
              .. make_helpers(f) .. '\n'
    end
    return c
end


local in_file_dir = arg[1]
local out_file = arg[2]

do
    local dir = io.popen('/bin/ls ' .. in_file_dir)
    local files = {}
    for filename in dir:lines() do
        table.insert(files, dofile('lua/ii/' .. filename))
    end

    local c = io.open( out_file, 'w' )
    c:write(make_ii(files))
    c:close()
end

-- example usage:
-- lua util/ii_norns_support.lua lua/ii/ util/norns.lua
