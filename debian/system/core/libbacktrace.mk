include /usr/share/dpkg/architecture.mk

NAME = libbacktrace

libbacktrace_SOURCES = \
  Backtrace.cpp \
  BacktraceCurrent.cpp \
  BacktraceMap.cpp \
  BacktracePtrace.cpp \
  ThreadEntry.cpp \
  UnwindStack.cpp \
  UnwindStackMap.cpp \

libunwindstack_SOURCES := \
  ArmExidx.cpp \
  DexFiles.cpp \
  DwarfCfa.cpp \
  DwarfEhFrameWithHdr.cpp \
  DwarfMemory.cpp \
  DwarfOp.cpp \
  DwarfSection.cpp \
  Elf.cpp \
  ElfInterface.cpp \
  ElfInterfaceArm.cpp \
  Global.cpp \
  JitDebug.cpp \
  Log.cpp \
  MapInfo.cpp \
  Maps.cpp \
  Memory.cpp \
  LocalUnwinder.cpp \
  Regs.cpp \
  RegsArm.cpp \
  RegsArm64.cpp \
  RegsX86.cpp \
  RegsX86_64.cpp \
  RegsMips.cpp \
  RegsMips64.cpp \
  RegsRiscv64.cpp \
  Unwinder.cpp \
  Symbols.cpp \

ifeq ($(DEB_HOST_ARCH), amd64)
  SOURCES_ASSEMBLY = libunwindstack/AsmGetRegsX86_64.S
else ifeq ($(DEB_HOST_ARCH), i386)
  SOURCES_ASSEMBLY = libunwindstack/AsmGetRegsX86.S
else ifeq ($(DEB_HOST_ARCH), mips64el)
  SOURCES_ASSEMBLY = libunwindstack/AsmGetRegsMips64.S
else ifeq ($(DEB_HOST_ARCH), mipsel)
  SOURCES_ASSEMBLY = libunwindstack/AsmGetRegsMips.S
endif

SOURCES = \
  $(foreach source, $(filter %.cpp, $(libbacktrace_SOURCES)), libbacktrace/$(source)) \
  $(foreach source, $(filter %.cpp, $(libunwindstack_SOURCES)), libunwindstack/$(source)) \

SOURCES := $(foreach source, $(SOURCES), system/core/$(source))
OBJECTS_CXX = $(SOURCES:.cpp=.o)
SOURCES_ASSEMBLY := $(foreach source, $(SOURCES_ASSEMBLY), system/core/$(source))
OBJECTS_ASSEMBLY := $(SOURCES_ASSEMBLY:.S=.o)

CXXFLAGS += -std=gnu++2a -fno-omit-frame-pointer
CPPFLAGS += \
  -Iexternal/libunwind/include \
  -Idebian/include/external/libunwind \
  -Isystem/core/include \
  -Isystem/core/base/include \
  -Isystem/core/liblog/include \
  -Isystem/core/libprocinfo/include \
  -Isystem/core/libunwindstack/include \

LDFLAGS += \
  -L/usr/lib/p7zip \
  -Ldebian/out/system/core \
  -Wl,-rpath=/usr/lib/p7zip \
  -Wl,-rpath=/usr/lib/$(DEB_HOST_MULTIARCH)/android \
  -Wl,-soname,$(NAME).so.0 \
  -l:7z.so \
  -lbase \
  -llog \
  -lpthread \
  -shared

# -latomic should be the last library specified
# https://github.com/android/ndk/issues/589
ifneq ($(filter armel mipsel,$(DEB_HOST_ARCH)),)
  LDFLAGS += -latomic
endif

build: $(OBJECTS_CXX) $(OBJECTS_ASSEMBLY)
	mkdir -p debian/out/system/core
	$(CXX) $^ -o debian/out/system/core/$(NAME).so.0 $(LDFLAGS)
	ln -sf $(NAME).so.0 debian/out/system/core/$(NAME).so

$(OBJECTS_CXX): %.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS) $(CPPFLAGS)

$(OBJECTS_ASSEMBLY): %.o: %.S
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS) -D__ASSEMBLY__
