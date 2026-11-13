--- Crow i2c library for lua frontend
--

local ii = {}

----------------------------
-- per-module setup
ii.new_mt = {
    -- note: .event is handled in C unless defined in lua
    -- if __index returns nil, then it calls C handler w self
    -- no __newindex protection as we want it freely writable
    __index = function(self,ix)
        -- handle ii.mod[id] syntax for duplicate devices
        if type(ix)=='number' then
            return c_ii_setaddress(self, ix)
        end
        -- look up all other funcs
        return c_ii_index(self, ix)
    end
}

function ii.newmod(name)
    -- TODO don't save string name, but pointer to C struct
    -- speeds up ii_field_lookup
    return setmetatable({_name = name}, ii.new_mt)
end

-- loads all ii devices (generated from ii descriptors)
-- uses ii.newmod and ii.new_mt
-- eg: ii.jf, ii.ansible
c_ii_load(ii)

----------------------------
-- basic ii functionality

-- implemented in lualink.c
ii.help = ii_list_modules
ii.m_help = ii_list_commands
ii.pullup = ii_pullup
ii.fastmode = i2c_fastmode
ii.set_address = ii_set_add
ii.get_address = ii_get_add
ii.raw = ii_lead_bytes
ii.stats = ii_stats

-- to be deprecated
ii.set = ii_lead

-- to be deprecated
function ii.get( address, cmd, ... )
    if not cmd then print'param not found'
    else ii_lead( address, cmd, ... ) end
end

function ii_LeadRx_handler( addr, cmd, _arg, data )
    if ii.event_raw(addr, cmd, data, _arg) then return end
    local dev, name, addrix = c_ii_cmd(addr,cmd)
    local rx_event = { name   = name
                     , device = addrix
                     , arg    = _arg
                     }
    ii[dev].event(rx_event, data)
end

function ii.e( name, event, ... )
    local e_string = '{name=[['..event.name..']]'
                  .. ',device='..event.device
                  .. ',arg='..event.arg
                  .. '}'
    crow.tell('ii.'..name,e_string,...)
end

ii.self =
    { cmds = { [1]='volts'
             , [2]='slew'
             , [4]='call1'
             , [5]='call2'
             , [6]='call3'
             , [7]='call4'
             , [8]='reset'
             , [9]='pulse'
             , [10]='ar'
             , [11]='lfo'
             -- input & output queries in C only
             , [5+128]='query0'
             , [6+128]='query1'
             , [7+128]='query2'
             , [8+128]='query3'
             }
    }

function ii.reset_events()
    -- the 8 variable-arity fns funnel through these shared fns
    -- double-indirection seems unnecessary. not sure why?
    ii.self._call  = function(...) ii.self.call{...} end
    ii.self._query = function(...) return ii.self.query{...} end

    ii.self.call1  = ii.self._call
    ii.self.call2  = ii.self._call
    ii.self.call3  = ii.self._call
    ii.self.call4  = ii.self._call
    ii.self.query0 = ii.self._query
    ii.self.query1 = ii.self._query
    ii.self.query2 = ii.self._query
    ii.self.query3 = ii.self._query

    -- all the individual call/queries forward to these 2 user fns
    ii.self.call = function(args) print('call'..#args) end
    ii.self.query = function(args) print('query'..#args); return 0 end

    ii.event_raw = function(addr, cmd, data, arg) end
end
ii.reset_events()

function ii_followRx_handler( cmd, ... )
    local name = ii.self.cmds[cmd]
    ii.self[name](...)
end

function ii_followRxTx_handler( cmd, ... )
    local name = ii.self.cmds[cmd]
    return ii.self[name](...)
end

--- METAMETHODS
ii.__index = function( self, ix )
    if ix == 'address' then return ii.get_address() end
    print'not found. try ii.help()'
end
ii.__newindex = function( self, ix, v )
    if ix == 'address' then ii.set_address(v)
    else rawset(self, ix, v) end -- FIXME prob don't want this?
end
setmetatable(ii, ii)

return ii
