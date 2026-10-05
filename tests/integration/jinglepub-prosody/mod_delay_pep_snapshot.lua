local st = require "util.stanza"

local target_node = module:get_option_string("delay_pep_snapshot_node", "urn:xmpp:jinglepub:ci")
local delay = module:get_option_number("delay_pep_snapshot_seconds", 1.5)

-- Return an authoritative empty snapshot, but only after a delay.  This models
-- a PubSub items query which raced with a subsequent publish: the snapshot was
-- taken before the item existed, while its IQ result arrives after publish ACK.
module:hook("pre-iq/bare", function (event)
    local stanza = event.stanza
    if stanza.attr.type ~= "get" then
        return
    end

    local pubsub = stanza:get_child("pubsub", "http://jabber.org/protocol/pubsub")
    local items = pubsub and pubsub:get_child("items")
    if not items or items.attr.node ~= target_node then
        return
    end

    local reply = st.reply(stanza)
        :tag("pubsub", { xmlns = "http://jabber.org/protocol/pubsub" })
            :tag("items", { node = target_node }):up()
        :up()
    local origin = event.origin
    module:log("debug", "Delaying empty PEP snapshot for %s by %.2fs", target_node, delay)
    module:add_timer(delay, function ()
        if origin and origin.send then
            origin.send(reply)
        end
    end)
    return true
end, 1000)
