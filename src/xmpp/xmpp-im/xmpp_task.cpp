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

#include "xmpp_task.h"

#include "xmpp_client.h"
#include "xmpp_stanza.h"
#include "xmpp_xmlcommon.h"

#include <QTimer>

#define DEFAULT_TIMEOUT 120

using namespace XMPP;

class Task::TaskPrivate
{
public:
    TaskPrivate() = default;

    QString id;
    bool success = false;
    int statusCode = 0;
    QString statusString;
    XMPP::Stanza::Error error;
    Client *client = nullptr;
    bool insig = false;
    bool deleteme = false;
    bool autoDelete = false;
    bool done = false;
    int timeout = 0;
};

Task::Task(Task *parent)
:QObject(parent)
{
    init();

    d->client = parent->client();
    d->id = client()->genUniqueId();
    connect(d->client, SIGNAL(disconnected()), SLOT(clientDisconnected()));
}

Task::Task(Client *parent, bool)
:QObject(nullptr)
{
    init();

    d->client = parent;
    connect(d->client, SIGNAL(disconnected()), SLOT(clientDisconnected()));
}

Task::~Task()
{
    delete d;
}

void Task::init()
{
    d = new TaskPrivate;
    d->success = false;
    d->insig = false;
    d->deleteme = false;
    d->autoDelete = false;
    d->done = false;
    d->timeout = DEFAULT_TIMEOUT;
}

Task *Task::parent() const
{
    return (Task *)QObject::parent();
}

Client *Task::client() const
{
    return d->client;
}

QDomDocument *Task::doc() const
{
    return client()->doc();
}

QString Task::id() const
{
    return d->id;
}

bool Task::success() const
{
    return d->success;
}

int Task::statusCode() const
{
    return d->statusCode;
}

const QString & Task::statusString() const
{
    return d->statusString;
}

const Stanza::Error &Task::error() const
{
    return d->error;
}

void Task::setTimeout(int seconds) const
{
    d->timeout = seconds;
}

int Task::timeout()
{
    return d->timeout;
}

void Task::go(bool autoDelete)
{
    d->autoDelete = autoDelete;

    if (!client() || !client()->hasStream()) {
        qWarning("Task::go(): attempted to send a task over the broken connection.");
        if (autoDelete) {
            deleteLater();
        }
    }
    else {
        onGo();
        if (d->timeout) {
            QTimer::singleShot(d->timeout * 1000, this, SLOT(timeoutFinished()));
        }
    }
}

bool Task::take(const QDomElement &x)
{
    const QObjectList p = children();

    // pass along the xml
    Task *t;
    for(QObjectList::ConstIterator it = p.begin(); it != p.end(); ++it) {
        QObject *obj = *it;
        if(!obj->inherits("XMPP::Task"))
            continue;

        t = static_cast<Task*>(obj);
        if(t->take(x)) // don't check for done here. it will hurt server tasks
            return true;
    }

    return false;
}

void Task::safeDelete()
{
    if(d->deleteme)
        return;

    d->deleteme = true;
    if(!d->insig)
        deleteLater();
}

void Task::onGo()
{
}

void Task::onDisconnect()
{
    if(!d->done) {
        d->success = false;
        d->statusCode = ErrDisc;
        d->statusString = tr("Disconnected");

        // delay this so that tasks that react don't block the shutdown
        QTimer::singleShot(0, this, SLOT(done()));
        // Even server tasks will be marked as done,
        // but we don't check the attribute for them.
    }
}

void Task::onTimeout()
{
    if(!d->done) {
        d->success = false;
        d->statusCode = ErrTimeout;
        d->statusString = tr("Request timed out");
        done();
    }
}

void Task::send(const QDomElement &x)
{
    client()->send(x);
}

void Task::setSuccess(int code, const QString &str)
{
    if(!d->done) {
        d->success = true;
        d->statusCode = code;
        d->statusString = str;
        done();
    }
}

void Task::setError(const QDomElement &e)
{
    if(!d->done) {
        d->success = false;

        QDomElement tag = e.firstChildElement("error");
        if(tag.isNull())
            return;

        XMPP::Stanza::Error err;
        err.fromXml(tag, d->client->streamBaseNS());
        d->error = err;
        d->statusCode = err.code();
        d->statusString = err.toString();
        done();
    }
}

void Task::setError(int code, const QString &str)
{
    if(!d->done) {
        d->success = false;
        d->statusCode = code;
        d->statusString = str;
        done();
    }
}

void Task::done()
{
    if(d->done || d->insig)
        return;
    d->done = true;

    if(d->autoDelete)
        d->deleteme = true;

    d->insig = true;
    emit finished();
    d->insig = false;

    if(d->deleteme)
        deleteLater();
}

void Task::clientDisconnected()
{
    onDisconnect();
}

void Task::timeoutFinished()
{
    if (!d->done)
        onTimeout();
}

void Task::debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    QString str;
    str.vsprintf(fmt, ap);
    va_end(ap);
    debug(str);
}

void Task::debug(const QString &str)
{
    client()->debug(QString("%1: ").arg(metaObject()->className()) + str);
}

/**
 * \brief verifiys a stanza is a IQ reply for this task
 *
 * it checks that the stanze is form the jid the request was send to and the id and the namespace (if given) match.
 *
 * it further checks that the sender jid is not empty (except if \a to is our server), it's not from
 * our bare jid (except if send to one of our resources or our server)
 * \param x the stanza to test
 * \param to the Jid this task send a IQ to
 * \param id the id of the send IQ
 * \param xmlns the expected namespace if the reply (if non empty)
 * \return true if it's a valid reply
*/

bool Task::iqVerify(const QDomElement &x, const Jid &to, const QString &id, const QString &xmlns)
{
    if(x.tagName() != QStringLiteral("iq"))
        return false;

    Jid from(x.attribute(QStringLiteral("from")));
    Jid local = client()->jid();
    Jid server = client()->host();

    // empty 'from' ?
    if(from.isEmpty()) {
        // allowed if we are querying the server
        if(!to.isEmpty() && !to.compare(server))
            return false;
    }
    // from ourself?
    else if(from.compare(local, false) || from.compare(local.domain(),false)) {
        // allowed if we are querying ourself or the server
        if(!to.isEmpty() && !to.compare(local, false) && !to.compare(server))
            return false;
    }
    // from anywhere else?
    else {
        if(!from.compare(to))
            return false;
    }

    if(!id.isEmpty()) {
        if(x.attribute(QStringLiteral("id")) != id)
            return false;
    }

    if(!xmlns.isEmpty()) {
        if(queryNS(x) != xmlns)
            return false;
    }

    return true;
}
