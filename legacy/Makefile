# libWebKitLegacy.so.1: the legacy BWebView API (WebPositive's) on Summit's
# engine. Runs on Haiku.
#
#   make ENGINE_HEADERS=DIR ENGINE_LIB=DIR
#
# ENGINE_HEADERS holds WebKit/WebKitView.h and the rest of the engine's public
# headers; ENGINE_LIB holds libWebKit.so.1 to link against. The defaults are
# those of the installed summit_webkit package, where the library also finds
# the engine at run time.
#
# The web inspector is Summit's developer tools window, built from Summit's
# own sources (SUMMIT_SRC, with nlohmann/json from SUMMIT_VENDOR); it needs
# the Scintilla and Lexilla headers (scintilla_devel, lexilla_devel) and
# libraries. The cookie window's jar is reached through the static
# libnetservices (Haiku's private network services, as WebPositive's).

ENGINE_HEADERS ?= /boot/system/develop/headers/summit-webkit
ENGINE_LIB ?= /boot/system/lib/summit-webkit/lib
ENGINE_RUNPATH ?= /boot/system/lib/summit-webkit/lib
PRIVATE_HEADERS ?= /boot/system/develop/headers/private
SCINTILLA_HEADERS ?= /boot/system/develop/headers/scintilla /boot/system/develop/headers/lexilla
# Where libnetservices.a, libshared.a and libcolumnlistview.a are.
STATIC_LIBS ?= /boot/system/develop/lib
SUMMIT_SRC ?= ../src
SUMMIT_VENDOR ?= ../vendor
VERSION ?= 1.10.0
OUT ?= build

CXX ?= g++
CXXFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Wno-multichar -Wno-unused-parameter
FLAGS = $(CXXFLAGS) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden $(WARNINGS)
# As Summit is built (tools/build-modern-browser.py).
SUMMIT_CPPFLAGS = $(CPPFLAGS) -DSUMMIT_MODERN_WEBKIT=1 -I$(ENGINE_HEADERS) -I$(SUMMIT_SRC) -I$(SUMMIT_VENDOR) \
	-I$(PRIVATE_HEADERS)/netservices -I$(PRIVATE_HEADERS) \
	$(addprefix -I,$(SCINTILLA_HEADERS))
LEGACY_CPPFLAGS = -Iheaders $(SUMMIT_CPPFLAGS)
LINKFLAGS = $(LDFLAGS) -shared -Wl,-soname,libWebKitLegacy.so.1 -Wl,--disable-new-dtags -Wl,-rpath,$(ENGINE_RUNPATH) -L$(ENGINE_LIB)
LIBS = -lWebKit -lbe -ltranslation -lnetwork -lbnetapi -ltracker -lscintilla -llexilla \
	$(STATIC_LIBS)/libnetservices.a $(STATIC_LIBS)/libshared.a $(STATIC_LIBS)/libcolumnlistview.a

SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:src/%.cpp=$(OUT)/%.o)
# Summit's developer tools (src/ui/DevToolsWindow.h) and what they use.
DEVTOOLS_SOURCES = core/DevTools.cpp core/DevToolsElements.cpp core/DevToolsFormat.cpp core/McpInspector.cpp \
	core/PageSource.cpp ui/DevToolsWindow.cpp ui/DevToolsConsole.cpp ui/DevToolsElements.cpp \
	ui/DevToolsNetwork.cpp ui/DevToolsPerformance.cpp ui/DevToolsSources.cpp ui/DevToolsStorage.cpp \
	ui/SourceView.cpp ui/PageSourceWindow.cpp
DEVTOOLS_OBJECTS = $(DEVTOOLS_SOURCES:%.cpp=$(OUT)/summit/%.o)
LIBRARY = $(OUT)/libWebKitLegacy.so.$(VERSION)

all: $(LIBRARY)

$(OUT)/%.o: src/%.cpp src/Private.h $(wildcard headers/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(LEGACY_CPPFLAGS) $(FLAGS) -std=c++23 -c $< -o $@

$(OUT)/summit/%.o: $(SUMMIT_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SUMMIT_CPPFLAGS) $(FLAGS) -std=c++23 -MMD -c $< -o $@

-include $(DEVTOOLS_OBJECTS:.o=.d)

$(LIBRARY): $(OBJECTS) $(DEVTOOLS_OBJECTS)
	$(CXX) $(LINKFLAGS) $(OBJECTS) $(DEVTOOLS_OBJECTS) -o $@ $(LIBS)
	ln -sf libWebKitLegacy.so.$(VERSION) $(OUT)/libWebKitLegacy.so.1
	ln -sf libWebKitLegacy.so.1 $(OUT)/libWebKitLegacy.so

clean:
	rm -rf $(OUT)

.PHONY: all clean
