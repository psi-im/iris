local st = require "util.stanza"

local target_node = module:get_option_string("delay_pep_snapshot_node", "urn:xmpp:jinglepub:ci")
local delay = module:get_option_number("delay_pep_snapshot_seconds", 1.5)
local suppress_user = module:get_option_string("delay_pep_snapshot_suppress_user", "user02")
local suppress_resource = module:get_option_string("delay_pep_snapshot_suppress_resource", "publisher")

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

-- Prosody normally delivers a self-PEP headline event to the publishing
-- resource. Iris deliberately replays such buffered live events after an
-- in-flight authority snapshot, which heals a stale empty snapshot. Suppress
-- exactly that event for the race account so the test also covers servers
-- which do not send a self notification to the owner resource.
module:hook("pre-message/full", function (event)
    local stanza = event.stanza
    if stanza.attr.type ~= "headline" then
        return
    end
    local to = stanza.attr.to or ""
    local expected = suppress_user .. "@" .. module.host .. "/" .. suppress_resource
    if to ~= expected then
        return
    end
    local pubsub_event = stanza:get_child("event", "http://jabber.org/protocol/pubsub#event")
    local items = pubsub_event and pubsub_event:get_child("items")
    if not items or items.attr.node ~= target_node then
        return
    end
    module:log("debug", "Suppressing self-PEP event for %s on %s", expected, target_node)
    return true
end, 1000)
