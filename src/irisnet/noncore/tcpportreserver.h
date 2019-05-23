/*
 * tcpportreserver.cpp - dialog for handling tabbed chats
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef TCPPORTRESERVER_H
#define TCPPORTRESERVER_H

#include <QObject>

class QTcpServer;
class QHostAddress;

namespace XMPP {

class TcpPortReserver : public QObject
{
    Q_OBJECT
public:
    explicit TcpPortReserver(QObject *parent = nullptr);
    ~TcpPortReserver();

    // port withing instanceId are uique but can be the same withing the same scopeId
    // scopes never intersect
    QList<QTcpServer*> borrow(const QString &scopeId, const QString &intanceId);
    void setExternalHost(const QString &scopeId, const QString &extHost, quint16 extPort, const QHostAddress &localIp, quint16 localPort);

signals:

public slots:
private:
    class Private;
    QScopedPointer<Private> d;
};

} // namespace XMPP

#endif // TCPPORTRESERVER_H
