#!/usr/bin/env sh
set -e

chmod -R a+w build

service postgresql start

cd build

# first regular cmake
su postgres -p -c "cmake -S .."
su postgres -p -c "cmake --build ."
cmake --install . --component extension # sudo
su postgres -p -c "ctest --output-on-failure"

cd ..
rm -rf build

# also try makefile pgxn wrapper
#sudo -u postgres make all
#make install # sudo
#sudo -u postgres make test
