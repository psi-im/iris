/*
 * Copyright (C) 2007  Justin Karneges
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "irisnetplugin.h"

#include <QtCore>
#include "netnames.h"
#include "qdnssd.h"

class AppleNameProvider;

static QByteArray nameToDottedString(const QByteArray &in)
{
	QByteArray out;
	int at = 0;
	while(at < in.size())
	{
		int len = in[at++];
		if(len > 0)
			out += in.mid(at, len);
		out += '.';
		at += len;
	}
	return out;
}

static XMPP::NameRecord importQDnsSdRecord(const QDnsSd::Record &in)
{
	XMPP::NameRecord out;
	switch(in.rrtype)
	{
		case 1: // A
		{
			quint32 *p = (quint32 *)in.rdata.data();
			out.setAddress(QHostAddress(ntohl(*p)));
		}
		break;

		case 28: // AAAA
		{
			out.setAddress(QHostAddress((quint8 *)in.rdata.data()));
		}
		break;

		case 12: // PTR
		{
			out.setPtr(nameToDottedString(in.rdata));
		}
		break;

		case 10: // NULL
		{
			out.setNull(in.rdata);
		}
		break;

		case 16: // TXT
		{
			QList<QByteArray> txtEntries = QDnsSd::parseTxtRecord(in.rdata);
			if(txtEntries.isEmpty())
				return out;
			out.setTxt(txtEntries);
		}
		break;

		default: // unsupported
		{
			return out;
		}
	}

	out.setOwner(in.name);
	out.setTtl(in.ttl);
	return out;
}

//----------------------------------------------------------------------------
// AppleProvider
//----------------------------------------------------------------------------
class AppleProvider : public XMPP::IrisNetProvider
{
	Q_OBJECT
	Q_INTERFACES(XMPP::IrisNetProvider);
public:
	QDnsSd dns;
	QHash<int,AppleNameProvider*> nameProviderById;

	AppleProvider() :
		dns(this)
	{
		connect(&dns, SIGNAL(queryResult(int, const QDnsSd::QueryResult &)), SLOT(dns_queryResult(int, const QDnsSd::QueryResult &)));
	}

	virtual XMPP::NameProvider *createNameProviderInternet();
	virtual XMPP::NameProvider *createNameProviderLocal();

	int query(AppleNameProvider *p, const QByteArray &name, int qType);
	void stop(int id);

private slots:
	void dns_queryResult(int id, const QDnsSd::QueryResult &result);
};

//----------------------------------------------------------------------------
// AppleNameProvider
//----------------------------------------------------------------------------
class AppleNameProvider : public XMPP::NameProvider
{
	Q_OBJECT
public:
	AppleProvider *global;

	AppleNameProvider(AppleProvider *parent) :
		NameProvider(parent),
		global(parent)
	{
	}

	bool supportsSingle() const
	{
		return false;
	}

	bool supportsLongLived() const
	{
		return true;
	}

	virtual int resolve_start(const QByteArray &name, int qType, bool longLived)
	{
		Q_UNUSED(longLived); // query is always long lived
		return global->dns.query(name, qType);
	}

	virtual void resolve_stop(int id)
	{
		global->dns.stop(id);
	}

	// called by AppleProvider

	void dns_queryResult(int id, const QDnsSd::QueryResult &result)
	{
		if(!result.success)
		{
			emit resolve_error(id, XMPP::NameResolver::ErrorGeneric);
			return;
		}

		QList<XMPP::NameRecord> results;
		foreach(const QDnsSd::Record &rec, result.records)
		{
			XMPP::NameRecord nr = importQDnsSdRecord(rec);

			// unsupported type
			if(nr.isNull())
				continue;

			// if removed, ensure ttl is 0
			if(!rec.added)
				nr.setTtl(0);

			results += nr;
		}

		emit resolve_resultsReady(id, results);
	}
};

// AppleProvider
XMPP::NameProvider *AppleProvider::createNameProviderInternet()
{
	return new AppleNameProvider(this);
}

XMPP::NameProvider *AppleProvider::createNameProviderLocal()
{
	return new AppleNameProvider(this);
}

int AppleProvider::query(AppleNameProvider *p, const QByteArray &name, int qType)
{
	int id = dns.query(name, qType);
	nameProviderById[id] = p;
	return id;
}

void AppleProvider::stop(int id)
{
	nameProviderById.remove(id);
}

void AppleProvider::dns_queryResult(int id, const QDnsSd::QueryResult &result)
{
	nameProviderById[id]->dns_queryResult(id, result);
}

#include "appledns.moc"

#ifdef APPLEDNS_STATIC
IrisNetProvider *irisnet_createAppleProvider()
{
        return new AppleProvider;
}
#else
Q_EXPORT_PLUGIN2(appledns, AppleProvider)
#endif
