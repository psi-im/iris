/*
 * xmpp_features.cpp - XMPP entity features
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

#include <QCoreApplication>
#include <QString>
#include <QMap>
#include <QStringList>

#include "xmpp_features.h"

#include "jingle.h"
#include "jingle-ft.h"

using namespace XMPP;

Features::Features()
{
}

Features::Features(const QStringList &l)
{
    setList(l);
}

Features::Features(const QSet<QString> &s)
{
    setList(s);
}

Features::Features(const QString &str)
{
    QSet<QString> l;
    l << str;

    setList(l);
}

Features::~Features()
{
}

QStringList Features::list() const
{
    return _list.toList();
}

void Features::setList(const QStringList &l)
{
    _list = QSet<QString>::fromList(l);
}

void Features::setList(const QSet<QString> &l)
{
    _list = l;
}

void Features::addFeature(const QString& s)
{
    _list += s;
}

bool Features::test(const QStringList &ns) const
{
    return _list.contains(QSet<QString>::fromList(ns));
}

bool Features::test(const QSet<QString> &ns) const
{
    return _list.contains(ns);
}

#define FID_MULTICAST "http://jabber.org/protocol/address"
bool Features::hasMulticast() const
{
    QSet<QString> ns;
    ns << FID_MULTICAST;

    return test(ns);
}

#define FID_AHCOMMAND "http://jabber.org/protocol/commands"
bool Features::hasCommand() const
{
    QSet<QString> ns;
    ns << FID_AHCOMMAND;

    return test(ns);
}

#define FID_REGISTER "jabber:iq:register"
bool Features::hasRegister() const
{
    QSet<QString> ns;
    ns << FID_REGISTER;

    return test(ns);
}

#define FID_SEARCH "jabber:iq:search"
bool Features::hasSearch() const
{
    QSet<QString> ns;
    ns << FID_SEARCH;

    return test(ns);
}

#define FID_GROUPCHAT "http://jabber.org/protocol/muc"
bool Features::hasGroupchat() const
{
    QSet<QString> ns;
    ns << FID_GROUPCHAT;

    return test(ns);
}

#define FID_VOICE "http://www.google.com/xmpp/protocol/voice/v1"
bool Features::hasVoice() const
{
    QSet<QString> ns;
    ns << FID_VOICE;

    return test(ns);
}

#define FID_GATEWAY "jabber:iq:gateway"
bool Features::hasGateway() const
{
    QSet<QString> ns;
    ns << FID_GATEWAY;

    return test(ns);
}

#define FID_QUERYVERSION "jabber:iq:version"
bool Features::hasVersion() const
{
    QSet<QString> ns;
    ns << FID_QUERYVERSION;

    return test(ns);
}

#define FID_DISCO "http://jabber.org/protocol/disco"
bool Features::hasDisco() const
{
    QSet<QString> ns;
    ns << FID_DISCO;
    ns << "http://jabber.org/protocol/disco#info";
    ns << "http://jabber.org/protocol/disco#items";

    return test(ns);
}

#define FID_CHATSTATE "http://jabber.org/protocol/chatstates"
bool Features::hasChatState() const
{
    QSet<QString> ns;
    ns << FID_CHATSTATE;

    return test(ns);
}

#define FID_VCARD "vcard-temp"
bool Features::hasVCard() const
{
    QSet<QString> ns;
    ns << FID_VCARD;

    return test(ns);
}

#define FID_MESSAGECARBONS "urn:xmpp:carbons:2"
bool Features::hasMessageCarbons() const
{
    QStringList ns;
    ns << FID_MESSAGECARBONS;

    return test(ns);
}

bool Features::hasJingleFT() const
{
    QStringList ns;
    ns << Jingle::FileTransfer::NS;

    return test(ns);
}

// custom Psi actions
#define FID_ADD "psi:add"

class Features::FeatureName : public QObject
{
    Q_OBJECT

public:
    FeatureName()
    : QObject(QCoreApplication::instance())
    {
        id2s[FID_Invalid]        = tr("ERROR: Incorrect usage of Features class");
        id2s[FID_None]            = tr("None");
        id2s[FID_Register]        = tr("Register");
        id2s[FID_Search]        = tr("Search");
        id2s[FID_Groupchat]        = tr("Groupchat");
        id2s[FID_Gateway]        = tr("Gateway");
        id2s[FID_Disco]            = tr("Service Discovery");
        id2s[FID_VCard]            = tr("VCard");
        id2s[FID_AHCommand]        = tr("Execute command");
        id2s[FID_QueryVersion]    = tr("Query version");
        id2s[FID_MessageCarbons]= tr("Message Carbons");

        // custom Psi actions
        id2s[FID_Add]            = tr("Add to roster");

        // compute reverse map
        //QMap<QString, long>::Iterator it = id2s.begin();
        //for ( ; it != id2s.end(); ++it)
        //    s2id[it.data()] = it.key();

        id2f[FID_Register]        = FID_REGISTER;
        id2f[FID_Search]        = FID_SEARCH;
        id2f[FID_Groupchat]        = FID_GROUPCHAT;
        id2f[FID_Gateway]        = FID_GATEWAY;
        id2f[FID_Disco]            = FID_DISCO;
        id2f[FID_VCard]            = FID_VCARD;
        id2f[FID_AHCommand]        = FID_AHCOMMAND;
        id2f[FID_QueryVersion]    = FID_QUERYVERSION;
        id2f[FID_MessageCarbons]= FID_MESSAGECARBONS;

        // custom Psi actions
        id2f[FID_Add]            = FID_ADD;
    }

    //QMap<QString, long> s2id;
    QMap<long, QString> id2s;
    QMap<long, QString> id2f;
};

static Features::FeatureName *featureName = nullptr;

long Features::id() const
{
    if ( _list.count() > 1 )
        return FID_Invalid;
    else if ( hasRegister() )
        return FID_Register;
    else if ( hasSearch() )
        return FID_Search;
    else if ( hasGroupchat() )
        return FID_Groupchat;
    else if ( hasGateway() )
        return FID_Gateway;
    else if ( hasDisco() )
        return FID_Disco;
    else if ( hasVCard() )
        return FID_VCard;
    else if ( hasCommand() )
        return FID_AHCommand;
    else if ( test(QStringList(FID_ADD)) )
        return FID_Add;
    else if ( hasVersion() )
        return FID_QueryVersion;

    return FID_None;
}

long Features::id(const QString &feature)
{
    Features f(feature);
    return f.id();
}

QString Features::feature(long id)
{
    if ( !featureName )
        featureName = new FeatureName();

    return featureName->id2f[id];
}

Features &Features::operator<<(const QString &feature)
{
    _list << feature;
    return *this;
}

QString Features::name(long id)
{
    if ( !featureName )
        featureName = new FeatureName();

    return featureName->id2s[id];
}

QString Features::name() const
{
    return name(id());
}

QString Features::name(const QString &feature)
{
    Features f(feature);
    return f.name(f.id());
}

#include "xmpp_features.moc"
