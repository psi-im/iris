load(winlocal.prf)

# qca
CONFIG += crypto

# zlib
INCLUDEPATH += $$WINLOCAL_PREFIX/include
LIBS += $$WINLOCAL_PREFIX/lib
LIBS += -lz
