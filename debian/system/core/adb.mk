NAME = adb

SOURCES = \
  client/adb_client.cpp \
  client/bugreport.cpp \
  client/commandline.cpp \
  client/file_sync_client.cpp \
  client/main.cpp \
  client/console.cpp \
  client/adb_install.cpp \
  client/line_printer.cpp \
  shell_service_protocol.cpp \

SOURCES := $(foreach source, $(SOURCES), system/core/adb/$(source))
OBJECTS = $(SOURCES:.cpp=.o)

CXXFLAGS += -std=gnu++2a
CPPFLAGS += \
  -D_GNU_SOURCE \
  -DADB_HOST=1 \
  -DADB_VERSION='"$(DEB_VERSION)"' \
  -I/usr/include/android \
  -Isystem/core/adb \
  -Isystem/core/base/include \
  -Isystem/core/include \

LDFLAGS += \
  -L/usr/lib/$(DEB_HOST_MULTIARCH)/android \
  -Ldebian/out/system/core \
  -Wl,-rpath=/usr/lib/$(DEB_HOST_MULTIARCH)/android \
  -lbase \
  -lcrypto \
  -lcutils \
  -llog \
  -lpthread \
  -lusb-1.0 \
  -pie \

STATIC_LIBS = \
  debian/out/system/core/libadb.a \
  debian/out/system/core/libcrypto_utils.a \

# -latomic should be the last library specified
# https://github.com/android/ndk/issues/589
ifneq ($(filter armel mipsel,$(DEB_HOST_ARCH)),)
  LDFLAGS += -latomic
endif

debian/out/system/core/$(NAME): $(OBJECTS)
	$(CXX) -o $@ $^ $(STATIC_LIBS) $(LDFLAGS)

$(OBJECTS): %.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS) $(CPPFLAGS)
