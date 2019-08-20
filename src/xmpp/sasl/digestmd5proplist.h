/*
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef DIGESTMD5PROPLIST_H
#define DIGESTMD5PROPLIST_H

#include <QByteArray>
#include <QList>

namespace XMPP {
    struct DIGESTMD5Prop
    {
        QByteArray var, val;
    };

    class DIGESTMD5PropList : public QList<DIGESTMD5Prop>
    {
        public:
            DIGESTMD5PropList();

            void set(const QByteArray &var, const QByteArray &val);
            QByteArray get(const QByteArray &var) const;
            QByteArray toString() const;
            bool fromString(const QByteArray &str);

        private:
            int varCount(const QByteArray &var) const;
    };
} // namespace XMPP

#endif // DIGESTMD5PROPLIST_H
