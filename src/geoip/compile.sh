gcc -Wall -fPIC -c main.c -o geoip.o
gcc -shared geoip.o -lGeoIP -o libgeoip.so
rm geoip.o
mv libgeoip.so ../../bin/modules
