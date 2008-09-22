/*
 * Copyright (C) 2008  Remko Troncon
 * See COPYING for license details.
 */

#ifndef UNITTEST_H
#define UNITTEST_H

#define BEGIN_UNITTESTS \
	int result = 0;

#define RUN_UNITTEST(TestObject) { \
    TestObject tc; \
    result |= QTest::qExec(&tc, argc, argv); \
  }

#define END_UNITTESTS \
	return result;

#endif
