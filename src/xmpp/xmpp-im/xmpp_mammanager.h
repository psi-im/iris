/*
 * xmpp_mammanager.h - XEP-0313 Message Archive Management
 * Copyright (C) 2024 mcneb10
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

#ifndef XMPP_MAM_MANAGER_H
#define XMPP_MAM_MANAGER_H

#include "xmpp_client.h"
#include "xmpp_mamtask.h"

#include <QObject>
#include <QString>

namespace XMPP {
class MAMManager : public QObject {
    Q_OBJECT
public:
    MAMManager(Client *client, int mamPageSize = 10, int mamMaxMessages = 0, bool flipPages = true,
               bool backwards = true);
    ~MAMManager();

    void getFullArchive(void (*archiveHandler)(QList<QDomElement>), const Jid &j, const bool allowMUCArchives = true);
    void getArchiveByIDRange(void (*archiveHandler)(QList<QDomElement>), const Jid &j, const QString &from_id,
                             const QString &to_id, const bool allowMUCArchives = true);
    void getArchiveByTimeRange(void (*archiveHandler)(QList<QDomElement>), const Jid &j, const QDateTime &from,
                               const QDateTime &to, const bool allowMUCArchives = true);
    void getLatestMessagesFromArchive(void (*archiveHandler)(QList<QDomElement>), const Jid &j,
                                      const bool allowMUCArchives = true, const QString &from_id, int amount = 100);

private:
    class Private;
    Private *d;
};
}

#endif
