#include "qttestutil/qttestutil.h"

#include <QObject>
#include <QtTest/QtTest>

class MyFirstClassTest : public QObject
{
     Q_OBJECT

    private slots:
        void initTestCase() {
        }

        void cleanupTestCase() {
        }

        void testMyMethod() {
            QCOMPARE(1, 1); // Dummy test
        }
};

QTTESTUTIL_REGISTER_TEST(MyFirstClassTest);
#include "myfirstclasstest.moc"
