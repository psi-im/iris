/*
 * jingle-active-group-extension_p.h - active Jingle group extension validation
 * Copyright (C) 2026  Psi contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef JINGLE_ACTIVE_GROUP_EXTENSION_P_H
#define JINGLE_ACTIVE_GROUP_EXTENSION_P_H

#include "jingle-session.h"

#include <QSet>

namespace XMPP { namespace Jingle { namespace ActiveGroupExtension {

    enum class Kind { Unchanged, Extension, Invalid };

    struct Result {
        Kind      kind       = Kind::Invalid;
        QString   addedName;
        qsizetype groupIndex = -1;
    };

    inline bool same(const QList<ContentGroup> &left, const QList<ContentGroup> &right)
    {
        if (left.size() != right.size())
            return false;
        for (qsizetype i = 0; i < left.size(); ++i) {
            if (left.at(i).semantics != right.at(i).semantics || left.at(i).contents != right.at(i).contents)
                return false;
        }
        return true;
    }

    // Active content-add grouping is intentionally stricter than initial
    // offer/answer negotiation. The peer may preserve the committed topology or
    // append exactly one content to exactly one existing BUNDLE group. Existing
    // groups and member order are immutable in this transaction; removal,
    // reordering, cross-group moves and simultaneous multi-group edits belong to
    // separate negotiations.
    inline Result classify(const QList<ContentGroup> &committed, const QList<ContentGroup> &proposed,
                           const QSet<QString> &stanzaContents)
    {
        if (same(committed, proposed))
            return { Kind::Unchanged, {}, -1 };
        if (committed.size() != proposed.size())
            return {};

        Result result;
        for (qsizetype i = 0; i < committed.size(); ++i) {
            const auto &before = committed.at(i);
            const auto &after  = proposed.at(i);
            if (before.semantics == after.semantics && before.contents == after.contents)
                continue;
            if (result.kind == Kind::Extension)
                return {}; // more than one group changed
            if (before.semantics != QLatin1String("BUNDLE") || after.semantics != QLatin1String("BUNDLE")
                || before.contents.size() < 2 || after.contents.size() != before.contents.size() + 1)
                return {};
            for (qsizetype member = 0; member < before.contents.size(); ++member) {
                if (before.contents.at(member) != after.contents.at(member))
                    return {}; // no removal/reorder/rebinding of established members
            }
            const auto added = after.contents.last();
            if (added.isEmpty() || !stanzaContents.contains(added) || before.contents.contains(added))
                return {};
            result = { Kind::Extension, added, i };
        }
        return result;
    }

}}} // namespace XMPP::Jingle::ActiveGroupExtension

#endif // JINGLE_ACTIVE_GROUP_EXTENSION_P_H
