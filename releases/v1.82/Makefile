# GPU Fan Curve Controller v1.82
#
# Compilación sin cabeceras de CUDA/NVML: se usa nvml_min.h y se enlaza
# directamente contra libnvidia-ml.so.1 que instala el driver.
#
#   make            -> set_fan_curve y vergpu
#   make test       -> pruebas sin hardware (mocks de NVML y MMIO falso)
#   sudo make install
#
# NVML_LIB se detecta automáticamente; si el driver está en otra ruta:
#   make NVML_LIB=/ruta/a/libnvidia-ml.so.1

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
LDFLAGS  ?=
PREFIX   ?= /usr/local
DESTDIR  ?=

NVML_LIB ?= $(firstword $(wildcard \
    /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1 \
    /usr/lib64/libnvidia-ml.so.1 \
    /usr/lib/libnvidia-ml.so.1 \
    /lib/x86_64-linux-gnu/libnvidia-ml.so.1))

SOURCES = set_fan_curve.cpp vergpu.cpp gpu_temps.c
HEADERS = nvml_min.h gpu_temps.h

.PHONY: all set_fan_curve vergpu test clean install

all: set_fan_curve vergpu

gpu_temps.o: gpu_temps.c gpu_temps.h
	$(CC) $(CFLAGS) -c gpu_temps.c -o $@

set_fan_curve: set_fan_curve.cpp gpu_temps.o $(HEADERS)
	@if [ -z "$(NVML_LIB)" ]; then \
	    echo "error: no se encontró libnvidia-ml.so.1; usa NVML_LIB=/ruta"; \
	    exit 1; \
	fi
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ set_fan_curve.cpp gpu_temps.o $(NVML_LIB)

vergpu: vergpu.cpp gpu_temps.o $(HEADERS)
	@if [ -z "$(NVML_LIB)" ]; then \
	    echo "error: no se encontró libnvidia-ml.so.1; usa NVML_LIB=/ruta"; \
	    exit 1; \
	fi
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ vergpu.cpp gpu_temps.o $(NVML_LIB) -ldl

# test_daemon incluye set_fan_curve.cpp en su propia unidad de traducción y
# aporta mocks de NVML y de gpu_temps; por eso NO enlaza gpu_temps.c.
tests/test_daemon: tests/test_daemon.cpp set_fan_curve.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ tests/test_daemon.cpp

# test_gpu_temps compila gpu_temps.c con GPU_TEMPS_TEST para usar sysfs y
# /dev/mem falsos.
tests/test_gpu_temps: tests/test_gpu_temps.c gpu_temps.c gpu_temps.h
	$(CC) $(CFLAGS) -DGPU_TEMPS_TEST -o $@ tests/test_gpu_temps.c gpu_temps.c

test: tests/test_daemon tests/test_gpu_temps
	./tests/test_daemon
	./tests/test_gpu_temps

install: all
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 set_fan_curve "$(DESTDIR)$(PREFIX)/bin/set_fan_curve"
	install -m 755 vergpu "$(DESTDIR)$(PREFIX)/bin/vergpu"

clean:
	rm -f gpu_temps.o set_fan_curve vergpu \
	      tests/test_daemon tests/test_gpu_temps
