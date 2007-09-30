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

#ifndef QDNSSD_H
#define QDNSSD_H

#include <QObject>
#include <QByteArray>
#include <QList>

// DOR-compliant
class QDnsSd : public QObject
{
	Q_OBJECT
public:
	class Record
	{
	public:
		QByteArray name;
		int rrtype;
		QByteArray rdata;
		quint32 ttl;
	};

	class BrowseEntry
	{
	public:
		QByteArray serviceName;

		// these may be different from request, see dns_sd docs
		QByteArray serviceType;
		QByteArray replyDomain;
	};

	class QueryResult
	{
	public:
		bool success;
		QList<Record> added;
		QList<Record> removed;
	};

	class BrowseResult
	{
	public:
		bool success;
		QList<BrowseEntry> added;
		QList<BrowseEntry> removed;
	};

	class ResolveResult
	{
	public:
		bool success;
		QByteArray fullName;
		QByteArray hostTarget;
		int port; // host byte-order
		QByteArray txtRecord;
	};

	class RegResult
	{
	public:
		enum Error
		{
			ErrorGeneric,
			ErrorConflict
		};

		bool success;
		Error errorCode;

		QByteArray domain;
	};

	QDnsSd(QObject *parent = 0);
	~QDnsSd();

	int query(const QByteArray &name, int qType);

	// domain may be empty
	int browse(const QByteArray &serviceType, const QByteArray &domain);

	int resolve(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain);

	// domain may be empty
	int reg(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain, int port, const QByteArray &txtRecord);

	// return -1 on error, else a record id
	int recordAdd(int reg_id, const Record &rec);

	bool recordUpdate(int rec_id, const Record &rec);
	bool recordUpdateTxt(int reg_id, const QByteArray &txtRecord);
	void recordRemove(int rec_id);

	void stop(int id);

signals:
	void queryResult(int id, const QDnsSd::QueryResult &result);
	void browseResult(int id, const QDnsSd::BrowseResult &result);
	void resolveResult(int id, const QDnsSd::ResolveResult &result);
	void regResult(int id, const QDnsSd::RegResult &result);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
