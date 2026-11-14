# Makefile for native Windows OMNeT++ 6.x + INET 4.5 build (Phase 5B)
# Invoke via OMNeT++ mingwenv shell or scripts/setup_omnetpp_windows.ps1 -BuildAndTest

INET_PROJ ?= $(INET_ROOT)
CXX_FLAGS ?= -std=c++14 -O2

all:
	@echo "Generating OMNeT++ Makefiles against INET_PROJ=$(INET_PROJ)..."
	opp_makemake -f --deep -O out -KINET_PROJ="$(INET_PROJ)" -DINET_IMPORT \
		-Isrc -Isrc/third_party -I"$(INET_PROJ)/src" \
		-L"$(INET_PROJ)/src" -lINET$$(D)
	$(MAKE) -f Makefile.omnet MODE=release

standalone-test:
	g++ $(CXX_FLAGS) -DOMNETPP_STANDALONE_TEST -Isrc -Isrc/third_party \
		src/TraceEventPlayer.cc tests/test_omnet_json_parser.cpp \
		-o tests/test_omnet_json_parser.exe
	tests/test_omnet_json_parser.exe

clean:
	rm -rf out Makefile.omnet tests/test_omnet_json_parser.exe
