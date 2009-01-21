/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef STUNMESSAGE_H
#define STUNMESSAGE_H

#include <QByteArray>
#include <QList>
#include <QSharedDataPointer>

namespace XMPP {

class StunMessage
{
public:
	enum Class
	{
		Request,
		SuccessResponse,
		ErrorResponse,
		Indication
	};

	enum ValidationFlags
	{
		Fingerprint      = 0x01,

		// you must have the hmac(sha1) algorithm in QCA to use
		MessageIntegrity = 0x02
	};

	enum ConvertResult
	{
		ConvertGood,
		ErrorFormat,
		ErrorFingerprint,
		ErrorMessageIntegrity,
		ErrorConvertUnknown = 64
	};

	class Attribute
	{
	public:
		quint16 type;
		QByteArray value;
	};

	StunMessage();
	StunMessage(const StunMessage &from);
	~StunMessage();
	StunMessage & operator=(const StunMessage &from);

	bool isNull() const;
	Class mclass() const;
	quint16 method() const;
	const quint8 *magic() const; // 4 bytes
	const quint8 *id() const; // 12 bytes
	QList<Attribute> attributes() const;

	// returns the first instance or null
	QByteArray attribute(quint16 type) const;

	void setClass(Class mclass);
	void setMethod(quint16 method);
	void setMagic(const quint8 *magic); // 4 bytes
	void setId(const quint8 *id); // 12 bytes
	void setAttributes(const QList<Attribute> &attribs);

	QByteArray toBinary(int validationFlags = 0, const QByteArray &key = QByteArray()) const;
	static StunMessage fromBinary(const QByteArray &a, ConvertResult *result = 0, int validationFlags = 0, const QByteArray &key = QByteArray());

	// minimal 3-field check
	static bool isProbablyStun(const QByteArray &a);

private:
	class Private;
	QSharedDataPointer<Private> d;
};

}

#endif
