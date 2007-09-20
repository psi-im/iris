# build appledns
CONFIG += appledns

# bundle appledns inside of irisnetcore
CONFIG += appledns_bundle

# bundle irisnetcore inside of iris
CONFIG += irisnetcore_bundle

# don't build iris, app will include iris.pri
CONFIG += iris_bundle
