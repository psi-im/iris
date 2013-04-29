/*
 * Copyright (C) 2008  Remko Troncon
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <QObject>
#include <QtTest/QtTest>
#include <QtCrypto>

#include "qttestutil/qttestutil.h"
#include "xmpp/sasl/digestmd5response.h"
#include "xmpp/base/unittest/incrementingrandomnumbergenerator.h"

using namespace XMPP;

class DIGESTMD5ResponseTest : public QObject
{
		Q_OBJECT

	private slots:
		void testConstructor_WithAuthzid() {
			DIGESTMD5Response response(
				"realm=\"example.com\","
				"nonce=\"O6skKPuaCZEny3hteI19qXMBXSadoWs840MchORo\","
				"qop=\"auth\",charset=\"utf-8\",algorithm=\"md5-sess\"",
				"xmpp", "jabber.example.com", "example.com", "myuser", "myuser_authz",
				"mypass",
				IncrementingRandomNumberGenerator(255));
			QByteArray expectedValue(
				"username=\"myuser\",realm=\"example.com\","
				"nonce=\"O6skKPuaCZEny3hteI19qXMBXSadoWs840MchORo\","
				"cnonce=\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\","
				"nc=00000001,"
				"digest-uri=\"xmpp/jabber.example.com\","
				"qop=auth,response=8fe15bc2aa31956b62d9de831b21a5d4,"
				"charset=utf-8,authzid=\"myuser_authz\"");

			QVERIFY(response.isValid());
			QCOMPARE(response.getValue(), expectedValue);
		}

		void testConstructor_WithoutAuthzid() {
			DIGESTMD5Response response(
				"realm=\"example.com\","
				"nonce=\"O6skKPuaCZEny3hteI19qXMBXSadoWs840MchORo\","
				"qop=\"auth\",charset=\"utf-8\",algorithm=\"md5-sess\"",
				"xmpp", "jabber.example.com", "example.com", "myuser", "",
				"mypass",
				IncrementingRandomNumberGenerator(255));
			QByteArray expectedValue(
				"username=\"myuser\",realm=\"example.com\","
				"nonce=\"O6skKPuaCZEny3hteI19qXMBXSadoWs840MchORo\","
				"cnonce=\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\","
				"nc=00000001,"
				"digest-uri=\"xmpp/jabber.example.com\","
				"qop=auth,response=564b1c1cc16d97b019f18b14c979964b,charset=utf-8");
			
			QVERIFY(response.isValid());
			QCOMPARE(response.getValue(), expectedValue);
		}
	
	private:
		QCA::Initializer initializer;
};

QTTESTUTIL_REGISTER_TEST(DIGESTMD5ResponseTest);
#include "digestmd5responsetest.moc"
