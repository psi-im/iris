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

#include "qdnssd.h"

#include <QtCore>
#include <stdio.h>

// for ntohs
#ifdef Q_OS_WIN
# include <windows.h>
#else
# include <netinet/in.h>
#endif

#include "dns_sd.h"

namespace {

// DNSServiceRef must be allocated by the user and initialized by the
//   API.  Additionally, it is unclear from the API whether or not
//   DNSServiceRef can be copied (it is an opaque data structure).
//   What we'll do is allocate DNSServiceRef on the heap, allowing us
//   to maintain a pointer which /can/ be copied.  Also, we'll keep
//   a flag to indicate whether the allocated DNSServiceRef has been
//   initialized yet.
class DSReference
{
private:
	DNSServiceRef *_p;
	bool _initialized;

public:
	DSReference() :
		_p(0),
		_initialized(false)
	{
		_p = (DNSServiceRef *)malloc(sizeof(DNSServiceRef));
	}

	~DSReference()
	{
		if(_initialized)
			DNSServiceRefDeallocate(*_p);
		free(_p);
	}

	DNSServiceRef *data()
	{
		return _p;
	}

	void setInitialized()
	{
		_initialized = true;
	}
};

class RecReference
{
private:
	DNSRecordRef *_p;
	bool _initialized;

public:
	RecReference() :
		_p(0),
		_initialized(false)
	{
		_p = (DNSRecordRef *)malloc(sizeof(DNSRecordRef));
	}

	~RecReference()
	{
		//if(_initialized)
		//	DNSServiceRefDeallocate(*_p);
		free(_p);
	}

	DNSRecordRef *data()
	{
		return _p;
	}

	/*void setInitialized()
	{
		_initialized = true;
	}*/
};

}

//----------------------------------------------------------------------------
// QDnsSd
//----------------------------------------------------------------------------
class QDnsSd::Private : public QObject
{
	Q_OBJECT
public:
	QDnsSd *q;

	class SubRecord
	{
	public:
		int _id;
		RecReference *_sdref;

		SubRecord() :
			_id(-1),
			_sdref(0)
		{
		}

		~SubRecord()
		{
			delete _sdref;
		}
	};

	class Request
	{
	public:
		enum Type
		{
			Query,
			Browse,
			Resolve,
			Reg
		};

		Private *_self;
		int _type;
		int _id;
		DSReference *_sdref;
		int _sockfd;
		QSocketNotifier *_sn_read;
		QTimer *_errorTrigger;

		bool _doSignal;
		bool _callbackError;
		QList<Record> _recordsAdd;
		QList<Record> _recordsRemove;
		QList<BrowseEntry> _browseAdd;
		QList<BrowseEntry> _browseRemove;
		QByteArray _resolveFullName;
		QByteArray _resolveHost;
		int _resolvePort;
		QByteArray _resolveTxtRecord;
		QByteArray _regDomain;
		bool _regConflict;

		QList<SubRecord*> _subRecords;

		Request(Private *self) :
			_self(self),
			_id(-1),
			_sdref(0),
			_sockfd(-1),
			_sn_read(0),
			_errorTrigger(0),
			_doSignal(false),
			_callbackError(false)
		{
		}

		~Request()
		{
			qDeleteAll(_subRecords);

			delete _errorTrigger;
			delete _sn_read;
			delete _sdref;
		}
	};

	QHash<int,Request*> _requestsById;
	QHash<QSocketNotifier*,Request*> _requestsBySocket;
	QHash<QTimer*,Request*> _requestsByTimer;
	int _next_id;

	Private(QDnsSd *_q) :
		QObject(_q),
		q(_q)
	{
		_next_id = 0;
	}

	~Private()
	{
		qDeleteAll(_requestsById);
	}

	void setDelayedError(Request *req)
	{
		req->_errorTrigger = new QTimer(this);
		connect(req->_errorTrigger, SIGNAL(timeout()), SLOT(doError()));
		req->_errorTrigger->setSingleShot(true);
		_requestsByTimer.insert(req->_errorTrigger, req);
		req->_errorTrigger->start();
	}

	void removeRequest(Request *req)
	{
		if(req->_errorTrigger)
			_requestsByTimer.remove(req->_errorTrigger);
		if(req->_sn_read)
			_requestsBySocket.remove(req->_sn_read);
		_requestsById.remove(req->_id);
		delete req;
	}

	int nextId()
	{
		return _next_id++;
	}

	int regIdForRecId(int rec_id) const
	{
		QHashIterator<int,Request*> it(_requestsById);
		while(it.hasNext())
		{
			it.next();
			Request *req = it.value();
			foreach(const SubRecord *srec, req->_subRecords)
			{
				if(srec->_id == rec_id)
					return it.key();
			}
		}
		return -1;
	}

	int query(const QByteArray &name, int qType)
	{
		int id = nextId();

		Request *req = new Request(this);
		req->_type = Request::Query;
		req->_id = id;
		req->_sdref = new DSReference;

		DNSServiceErrorType err = DNSServiceQueryRecord(
			req->_sdref->data(), kDNSServiceFlagsLongLivedQuery,
			0, name.constData(), qType, kDNSServiceClass_IN,
			cb_queryRecordReply, req);
		if(err != kDNSServiceErr_NoError)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sdref->setInitialized();

		int sockfd = DNSServiceRefSockFD(*(req->_sdref->data()));
		if(sockfd == -1)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sockfd = sockfd;
		req->_sn_read = new QSocketNotifier(sockfd, QSocketNotifier::Read, this);
		connect(req->_sn_read, SIGNAL(activated(int)), SLOT(sn_activated()));
		_requestsById.insert(id, req);
		_requestsBySocket.insert(req->_sn_read, req);

		return id;
	}

	int browse(const QByteArray &serviceType, const QByteArray &domain)
	{
		int id = nextId();

		Request *req = new Request(this);
		req->_type = Request::Browse;
		req->_id = id;
		req->_sdref = new DSReference;

		DNSServiceErrorType err = DNSServiceBrowse(
			req->_sdref->data(), 0, 0, serviceType.constData(),
			!domain.isEmpty() ? domain.constData() : NULL,
			cb_browseReply, req);
		if(err != kDNSServiceErr_NoError)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sdref->setInitialized();

		int sockfd = DNSServiceRefSockFD(*(req->_sdref->data()));
		if(sockfd == -1)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sockfd = sockfd;
		req->_sn_read = new QSocketNotifier(sockfd, QSocketNotifier::Read, this);
		connect(req->_sn_read, SIGNAL(activated(int)), SLOT(sn_activated()));
		_requestsById.insert(id, req);
		_requestsBySocket.insert(req->_sn_read, req);

		return id;
	}

	int resolve(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain)
	{
		int id = nextId();

		Request *req = new Request(this);
		req->_type = Request::Resolve;
		req->_id = id;
		req->_sdref = new DSReference;

		DNSServiceErrorType err = DNSServiceResolve(
			req->_sdref->data(), 0, 0, serviceName.constData(),
			serviceType.constData(), domain.constData(),
			cb_resolveReply, req);
		if(err != kDNSServiceErr_NoError)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sdref->setInitialized();

		int sockfd = DNSServiceRefSockFD(*(req->_sdref->data()));
		if(sockfd == -1)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sockfd = sockfd;
		req->_sn_read = new QSocketNotifier(sockfd, QSocketNotifier::Read, this);
		connect(req->_sn_read, SIGNAL(activated(int)), SLOT(sn_activated()));
		_requestsById.insert(id, req);
		_requestsBySocket.insert(req->_sn_read, req);

		return id;
	}

	int reg(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain, int port, const QByteArray &txtRecord)
	{
		int id = nextId();

		Request *req = new Request(this);
		req->_type = Request::Reg;
		req->_id = id;

		if(port < 1 || port > 0xffff)
		{
			setDelayedError(req);
			return id;
		}

		uint16_t sport = port;
		sport = htons(sport);

		req->_sdref = new DSReference;

		DNSServiceErrorType err = DNSServiceRegister(
			req->_sdref->data(), kDNSServiceFlagsNoAutoRename, 0,
			serviceName.constData(), serviceType.constData(),
			domain.constData(), NULL, sport, txtRecord.size(),
			txtRecord.data(), cb_regReply, req);
		if(err != kDNSServiceErr_NoError)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sdref->setInitialized();

		int sockfd = DNSServiceRefSockFD(*(req->_sdref->data()));
		if(sockfd == -1)
		{
			delete req->_sdref;
			req->_sdref = 0;

			setDelayedError(req);
			return id;
		}

		req->_sockfd = sockfd;
		req->_sn_read = new QSocketNotifier(sockfd, QSocketNotifier::Read, this);
		connect(req->_sn_read, SIGNAL(activated(int)), SLOT(sn_activated()));
		_requestsById.insert(id, req);
		_requestsBySocket.insert(req->_sn_read, req);

		return id;
	}

	int recordAdd(int reg_id, const Record &rec)
	{
		Request *req = _requestsById.value(reg_id);
		if(!req)
			return -1;

		RecReference *recordRef = new RecReference;

		DNSServiceErrorType err = DNSServiceAddRecord(
			*(req->_sdref->data()), recordRef->data(), 0,
			rec.rrtype, rec.rdata.size(), rec.rdata.data(),
			rec.ttl);
		if(err != kDNSServiceErr_NoError)
		{
			delete recordRef;
			return -1;
		}

		int id = nextId();
		SubRecord *srec = new SubRecord;
		srec->_id = id;
		srec->_sdref = recordRef;
		req->_subRecords += srec;

		return id;
	}

	bool recordUpdate(int reg_id, int rec_id, const Record &rec)
	{
		// FIXME: optimize...

		Request *req = _requestsById.value(reg_id);
		if(!req)
			return false;

		int at = -1;
		for(int n = 0; n < req->_subRecords.count(); ++n)
		{
			if(req->_subRecords[n]->_id == rec_id)
			{
				at = n;
				break;
			}
		}

		if(at == -1)
			return false;

		SubRecord *srec = req->_subRecords[at];
		DNSServiceErrorType err = DNSServiceUpdateRecord(
			*(req->_sdref->data()), *(srec->_sdref->data()), 0,
			rec.rdata.size(), rec.rdata.data(), rec.ttl);
		if(err != kDNSServiceErr_NoError)
		{
			return false;
		}

		return true;
	}

	void recordRemove(int rec_id)
	{
		// FIXME: optimize...

		int reg_id = regIdForRecId(rec_id);
		if(reg_id == -1)
			return;

		// this can't fail
		Request *req = _requestsById.value(reg_id);

		// this can't fail either
		int at = -1;
		for(int n = 0; n < req->_subRecords.count(); ++n)
		{
			if(req->_subRecords[n]->_id == rec_id)
			{
				at = n;
				break;
			}
		}

		SubRecord *srec = req->_subRecords[at];
		DNSServiceRemoveRecord(*(req->_sdref->data()), *(srec->_sdref->data()), 0);
	}

	void stop(int id)
	{
		Request *req = _requestsById.value(id);
		if(req)
			removeRequest(req);
	}

private slots:
	void sn_activated()
	{
		QSocketNotifier *sn_read = (QSocketNotifier *)sender();
		Request *req = _requestsBySocket.value(sn_read);
		if(!req)
			return;

		if(req->_type == Request::Query)
		{
			DNSServiceErrorType err = DNSServiceProcessResult(*(req->_sdref->data()));
			if(err != kDNSServiceErr_NoError)
			{
				int id = req->_id;

				removeRequest(req);

				QDnsSd::QueryResult r;
				r.success = false;
				emit q->queryResult(id, r);
				return;
			}

			if(req->_doSignal)
			{
				int id = req->_id;

				if(req->_callbackError)
				{
					removeRequest(req);

					QDnsSd::QueryResult r;
					r.success = false;
					emit q->queryResult(id, r);
					return;
				}

				QDnsSd::QueryResult r;
				r.success = true;
				r.added = req->_recordsAdd;
				r.removed = req->_recordsRemove;
				req->_recordsAdd.clear();
				req->_recordsRemove.clear();
				req->_doSignal = false;

				emit q->queryResult(id, r);
			}
		}
		else if(req->_type == Request::Browse)
		{
			DNSServiceErrorType err = DNSServiceProcessResult(*(req->_sdref->data()));
			if(err != kDNSServiceErr_NoError)
			{
				int id = req->_id;

				removeRequest(req);

				QDnsSd::BrowseResult r;
				r.success = false;
				emit q->browseResult(id, r);
				return;
			}

			if(req->_doSignal)
			{
				int id = req->_id;

				if(req->_callbackError)
				{
					removeRequest(req);

					QDnsSd::BrowseResult r;
					r.success = false;
					emit q->browseResult(id, r);
					return;
				}

				QDnsSd::BrowseResult r;
				r.success = true;
				r.added = req->_browseAdd;
				r.removed = req->_browseRemove;
				req->_browseAdd.clear();
				req->_browseRemove.clear();
				req->_doSignal = false;

				emit q->browseResult(id, r);
			}
		}
		else if(req->_type == Request::Resolve)
		{
			DNSServiceErrorType err = DNSServiceProcessResult(*(req->_sdref->data()));
			if(err != kDNSServiceErr_NoError)
			{
				int id = req->_id;

				removeRequest(req);

				QDnsSd::ResolveResult r;
				r.success = false;
				emit q->resolveResult(id, r);
				return;
			}

			if(req->_doSignal)
			{
				int id = req->_id;

				if(req->_callbackError)
				{
					removeRequest(req);

					QDnsSd::ResolveResult r;
					r.success = false;
					emit q->resolveResult(id, r);
					return;
				}

				QDnsSd::ResolveResult r;
				r.success = true;
				r.fullName = req->_resolveFullName;
				r.hostTarget = req->_resolveHost;
				r.port = req->_resolvePort;
				r.txtRecord = req->_resolveTxtRecord;
				req->_doSignal = false;

				// there is only one response
				removeRequest(req);

				emit q->resolveResult(id, r);
			}
		}
		else // Reg
		{
			DNSServiceErrorType err = DNSServiceProcessResult(*(req->_sdref->data()));
			if(err != kDNSServiceErr_NoError)
			{
				int id = req->_id;

				removeRequest(req);

				QDnsSd::RegResult r;
				r.success = false;
				r.errorCode = QDnsSd::RegResult::ErrorGeneric;
				emit q->regResult(id, r);
				return;
			}

			if(req->_doSignal)
			{
				int id = req->_id;

				if(req->_callbackError)
				{
					removeRequest(req);

					QDnsSd::RegResult r;
					r.success = false;
					if(req->_regConflict)
						r.errorCode = QDnsSd::RegResult::ErrorConflict;
					else
						r.errorCode = QDnsSd::RegResult::ErrorGeneric;
					emit q->regResult(id, r);
					return;
				}

				QDnsSd::RegResult r;
				r.success = true;
				r.domain = req->_regDomain;
				req->_doSignal = false;

				emit q->regResult(id, r);
			}
		}
	}

	void doError()
	{
		QTimer *t = (QTimer *)sender();
		Request *req = _requestsByTimer.value(t);
		if(!req)
			return;

		int id = req->_id;
		int type = req->_type;
		removeRequest(req);

		if(type == Request::Query)
		{
			QDnsSd::QueryResult r;
			r.success = false;
			emit q->queryResult(id, r);
		}
		else if(type == Request::Browse)
		{
			QDnsSd::BrowseResult r;
			r.success = false;
			emit q->browseResult(id, r);
		}
		else if(type == Request::Resolve)
		{
			QDnsSd::ResolveResult r;
			r.success = false;
			emit q->resolveResult(id, r);
		}
		else // Reg
		{
			QDnsSd::RegResult r;
			r.success = false;
			r.errorCode = QDnsSd::RegResult::ErrorGeneric;
			emit q->regResult(id, r);
		}
	}

private:
	static void cb_queryRecordReply(DNSServiceRef ref,
		DNSServiceFlags flags, uint32_t interfaceIndex,
		DNSServiceErrorType errorCode, const char *fullname,
		uint16_t rrtype, uint16_t rrclass, uint16_t rdlen,
		const void *rdata, uint32_t ttl, void *context)
	{
		Q_UNUSED(ref);
		Q_UNUSED(interfaceIndex);
		Q_UNUSED(rrclass);

		Request *req = static_cast<Request *>(context);
		req->_self->handle_queryRecordReply(req, flags, errorCode,
			fullname, rrtype, rdlen, (const char *)rdata, ttl);
	}

	static void cb_browseReply(DNSServiceRef ref,
		DNSServiceFlags flags, uint32_t interfaceIndex,
		DNSServiceErrorType errorCode, const char *serviceName,
		const char *regtype, const char *replyDomain, void *context)
	{
		Q_UNUSED(ref);
		Q_UNUSED(interfaceIndex);

		Request *req = static_cast<Request *>(context);
		req->_self->handle_browseReply(req, flags, errorCode,
			serviceName, regtype, replyDomain);
	}

	static void cb_resolveReply(DNSServiceRef ref,
		DNSServiceFlags flags, uint32_t interfaceIndex,
		DNSServiceErrorType errorCode, const char *fullname,
		const char *hosttarget, uint16_t port, uint16_t txtLen,
		const char *txtRecord, void *context)
	{
		Q_UNUSED(ref);
		Q_UNUSED(flags);
		Q_UNUSED(interfaceIndex);

		Request *req = static_cast<Request *>(context);
		req->_self->handle_resolveReply(req, errorCode, fullname,
			hosttarget, port, txtLen, txtRecord);
	}

	static void cb_regReply(DNSServiceRef ref,
		DNSServiceFlags flags, DNSServiceErrorType errorCode,
		const char *name, const char *regtype, const char *domain,
		void *context)
	{
		Q_UNUSED(ref);
		Q_UNUSED(flags);

		Request *req = static_cast<Request *>(context);
		req->_self->handle_regReply(req, errorCode, name, regtype,
			domain);
	}

	void handle_queryRecordReply(Request *req, DNSServiceFlags flags,
		DNSServiceErrorType errorCode, const char *fullname,
		uint16_t rrtype, uint16_t rdlen, const char *rdata,
		uint16_t ttl)
	{
		if(errorCode != kDNSServiceErr_NoError)
		{
			req->_doSignal = true;
			req->_callbackError = true;
			return;
		}

		QDnsSd::Record rec;
		rec.name = QByteArray(fullname);
		rec.rrtype = rrtype;
		rec.rdata = QByteArray(rdata, rdlen);
		rec.ttl = ttl;

		if(flags & kDNSServiceFlagsAdd)
			req->_recordsAdd += rec;
		else
			req->_recordsRemove += rec;

		if(!(flags & kDNSServiceFlagsMoreComing))
			req->_doSignal = true;
	}

	void handle_browseReply(Request *req, DNSServiceFlags flags,
		DNSServiceErrorType errorCode, const char *serviceName,
		const char *regtype, const char *replyDomain)
	{
		if(errorCode != kDNSServiceErr_NoError)
		{
			req->_doSignal = true;
			req->_callbackError = true;
			return;
		}

		QDnsSd::BrowseEntry e;
		e.serviceName = QByteArray(serviceName);
		e.serviceType = QByteArray(regtype);
		e.replyDomain = QByteArray(replyDomain);

		if(flags & kDNSServiceFlagsAdd)
			req->_browseAdd += e;
		else
			req->_browseRemove += e;

		if(!(flags & kDNSServiceFlagsMoreComing))
			req->_doSignal = true;
	}

	void handle_resolveReply(Request *req, DNSServiceErrorType errorCode,
		const char *fullname, const char *hosttarget, uint16_t port,
		uint16_t txtLen, const char *txtRecord)
	{
		if(errorCode != kDNSServiceErr_NoError)
		{
			req->_doSignal = true;
			req->_callbackError = true;
			return;
		}

		req->_resolveFullName = QByteArray(fullname);
		req->_resolveHost = QByteArray(hosttarget);
		req->_resolvePort = ntohs(port);
		req->_resolveTxtRecord = QByteArray(txtRecord, txtLen);

		// note: we do this after the callback
		// cancel connection
		//delete req->_sn_read;
		//req->_sn_read = 0;
		//delete req->_sdref;
		//req->_sdref = 0;

		req->_doSignal = true;
	}

	void handle_regReply(Request *req, DNSServiceErrorType errorCode,
		const char *name, const char *regtype, const char *domain)
	{
		Q_UNUSED(name);
		Q_UNUSED(regtype);

		if(errorCode != kDNSServiceErr_NoError)
		{
			req->_doSignal = true;
			req->_callbackError = true;

			if(errorCode == kDNSServiceErr_NameConflict)
				req->_regConflict = true;
			else
				req->_regConflict = false;
			return;
		}

		req->_regDomain = QByteArray(domain);
		req->_doSignal = true;
	}
};

QDnsSd::QDnsSd(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

QDnsSd::~QDnsSd()
{
	delete d;
}

int QDnsSd::query(const QByteArray &name, int qType)
{
	return d->query(name, qType);
}

int QDnsSd::browse(const QByteArray &serviceType, const QByteArray &domain)
{
	return d->browse(serviceType, domain);
}

int QDnsSd::resolve(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain)
{
	return d->resolve(serviceName, serviceType, domain);
}

int QDnsSd::reg(const QByteArray &serviceName, const QByteArray &serviceType, const QByteArray &domain, int port, const QByteArray &txtRecord)
{
	return d->reg(serviceName, serviceType, domain, port, txtRecord);
}

int QDnsSd::recordAdd(int reg_id, const Record &rec)
{
	return d->recordAdd(reg_id, rec);
}

bool QDnsSd::recordUpdate(int rec_id, const Record &rec)
{
	int reg_id = d->regIdForRecId(rec_id);
	if(reg_id == -1)
		return false;

	return d->recordUpdate(reg_id, rec_id, rec);
}

bool QDnsSd::recordUpdateTxt(int reg_id, const QByteArray &txtRecord)
{
	Record rec;
	rec.rrtype = kDNSServiceType_TXT;
	rec.rdata = txtRecord;
	rec.ttl = 4500;
	return d->recordUpdate(reg_id, -1, rec);
}

void QDnsSd::recordRemove(int rec_id)
{
	d->recordRemove(rec_id);
}

void QDnsSd::stop(int id)
{
	d->stop(id);
}

QByteArray QDnsSd::createTxtRecord(const QList<QByteArray> &strings)
{
	// split into var/val and validate
	QList<QByteArray> vars;
	QList<QByteArray> vals; // null = no value, empty = empty value
	foreach(const QByteArray &i, strings)
	{
		QByteArray var;
		QByteArray val;
		int n = i.indexOf('=');
		if(n != -1)
		{
			var = i.mid(0, n);
			val = i.mid(n + 1);
		}
		else
			var = i;

		for(int n = 0; n < var.size(); ++n)
		{
			unsigned char c = var[n];
			if(c < 0x20 || c > 0x7e)
				return QByteArray();
		}

		vars += var;
		vals += val;
	}

	TXTRecordRef ref;
	QByteArray buf(256, 0);
	TXTRecordCreate(&ref, buf.size(), buf.data());
	for(int n = 0; n < vars.count(); ++n)
	{
		int valueSize = vals[n].size();
		char *value;
		if(!vals[n].isNull())
			value = vals[n].data();
		else
			value = 0;

		DNSServiceErrorType err = TXTRecordSetValue(&ref,
			vars[n].data(), valueSize, value);
		if(err != kDNSServiceErr_NoError)
		{
			TXTRecordDeallocate(&ref);
			return QByteArray();
		}
	}
	QByteArray out((const char *)TXTRecordGetBytesPtr(&ref), TXTRecordGetLength(&ref));
	TXTRecordDeallocate(&ref);
	return out;
}

QList<QByteArray> QDnsSd::parseTxtRecord(const QByteArray &txtRecord)
{
	QList<QByteArray> out;
	int count = TXTRecordGetCount(txtRecord.size(), txtRecord.data());
	for(int n = 0; n < count; ++n)
	{
		QByteArray keyBuf(256, 0);
		uint8_t valueLen;
		void *value;
		DNSServiceErrorType err = TXTRecordGetItemAtIndex(
			txtRecord.size(), txtRecord.data(), n, keyBuf.size(),
			keyBuf.data(), &valueLen, &value);
		if(err != kDNSServiceErr_NoError)
			return QList<QByteArray>();

		keyBuf.resize(qstrlen(keyBuf.data()));

		QByteArray entry = keyBuf;
		if(value)
		{
			entry += '=';
			entry += QByteArray((const char *)value, valueLen);
		}
		out += entry;
	}
	return out;
}

#include "qdnssd.moc"
