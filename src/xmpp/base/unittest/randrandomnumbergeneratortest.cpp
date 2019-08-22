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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "qttestutil/qttestutil.h"
#include "xmpp/base/randrandomnumbergenerator.h"

#include <QObject>
#include <QtTest/QtTest>

using namespace XMPP;

class RandRandomNumberGeneratorTest : public QObject {
        Q_OBJECT

    private slots:
        void testGenerateNumber() {
            RandRandomNumberGenerator testling;

            double a = testling.generateNumberBetween(0.0,100.0);
            double b = testling.generateNumberBetween(0.0,100.0);

            QVERIFY(a != b);
        }
 };

QTTESTUTIL_REGISTER_TEST(RandRandomNumberGeneratorTest);
#include "randrandomnumbergeneratortest.moc"
