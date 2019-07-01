/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#ifndef ADDRESSRESOLVER_H
#define ADDRESSRESOLVER_H

#include <QObject>
#include <QHostAddress>

namespace XMPP {

// resolve both AAAA and A for a hostname
class AddressResolver : public QObject
{
    Q_OBJECT

public:
    enum Error
    {
        ErrorGeneric
    };

    AddressResolver(QObject *parent = nullptr);
    ~AddressResolver();

    void start(const QByteArray &hostName);
    void stop();

signals:
    void resultsReady(const QList<QHostAddress> &results);
    void error(XMPP::AddressResolver::Error e);

private:
    class Private;
    friend class Private;
    Private *d;
};

}

#endif
