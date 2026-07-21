CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Isrc
LDFLAGS ?=
WINDRES ?= windres

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
CPPFLAGS += -D_WIN32_WINNT=0x0601
RESOURCE_OBJ := jvman-resource.o
LDLIBS += -ladvapi32 -lwinhttp -lshell32
else
EXEEXT :=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
RESOURCE_OBJ :=
endif

TARGET := jvman$(EXEEXT)
SOURCES := src/main.c src/manager.c src/download_source.c src/discovery.c src/update.c src/platform.c src/util.c src/i18n.c src/sha256.c
HEADERS := src/common.h src/manager.h src/download_source.h src/discovery.h src/update.h src/platform.h src/util.h src/i18n.h src/sha256.h
TEST_TARGET := test_unit$(EXEEXT)
TEST_SOURCES := tests/test_unit.c src/download_source.c src/discovery.c src/platform.c src/util.c src/sha256.c
UPDATE_TEST_TARGET := test_update$(EXEEXT)
UPDATE_TEST_SOURCES := tests/test_update.c src/update.c src/platform.c src/util.c src/i18n.c src/sha256.c
UPDATE_FLOW_TEST_TARGET := test_update_flow$(EXEEXT)
UPDATE_FLOW_TEST_SOURCES := tests/test_update_flow.c src/update.c src/util.c src/i18n.c
UPDATE_HELPER_TEST_TARGET := test_update_helper$(EXEEXT)

# The native setup bundle is intentionally a Windows-only target.  It consists
# of a small Win32 setup stub with the jvman executable appended as a verified
# payload; no installer runtime or third-party dependency is required.
INSTALLER_TARGET :=
INSTALLER_FILES :=
INSTALLER_TEST_TARGETS :=

ifeq ($(OS),Windows_NT)
SETUP_STUB := jvman-setup-stub$(EXEEXT)
PACK_SETUP := pack_setup$(EXEEXT)
SETUP_TARGET := jvman-setup$(EXEEXT)
SETUP_RESOURCE_OBJ := installer-resource.o
SETUP_SOURCES := installer/setup.c installer/environment.c installer/files.c \
                 installer/pathlist.c installer/package.c installer/lang.c src/sha256.c
SETUP_HEADERS := installer/environment.h installer/files.h \
                 installer/pathlist.h installer/package.h installer/lang.h \
                 installer/resource.h src/common.h src/sha256.h
PACKER_SOURCES := installer/pack_setup.c installer/package.c src/sha256.c
INSTALLER_TARGET := $(SETUP_TARGET)
INSTALLER_FILES := $(SETUP_STUB) $(PACK_SETUP) $(SETUP_TARGET) $(SETUP_RESOURCE_OBJ)
TEST_INSTALLER_PACKAGE := test_installer_package$(EXEEXT)
TEST_INSTALLER_PATHLIST := test_installer_pathlist$(EXEEXT)
TEST_INSTALLER_LANG := test_installer_lang$(EXEEXT)
TEST_INSTALLER_DATA_CLEANUP := test_installer_data_cleanup$(EXEEXT)
INSTALLER_TEST_TARGETS := $(TEST_INSTALLER_PACKAGE) $(TEST_INSTALLER_PATHLIST) \
                          $(TEST_INSTALLER_LANG) $(TEST_INSTALLER_DATA_CLEANUP)
endif

.PHONY: all clean test integration-test installer-test

all: $(TARGET) $(INSTALLER_TARGET)

$(TARGET): $(SOURCES) $(HEADERS) $(RESOURCE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(RESOURCE_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

ifeq ($(OS),Windows_NT)
$(RESOURCE_OBJ): resources/jvman.rc resources/jvman.manifest
	$(WINDRES) -Iresources resources/jvman.rc -O coff -o $@

$(SETUP_RESOURCE_OBJ): installer/installer.rc installer/installer.manifest \
		installer/resource.h src/common.h
	$(WINDRES) -Iinstaller -Isrc installer/installer.rc -O coff -o $@

$(SETUP_STUB): $(SETUP_SOURCES) $(SETUP_HEADERS) $(SETUP_RESOURCE_OBJ)
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) -municode -mwindows \
		$(SETUP_SOURCES) $(SETUP_RESOURCE_OBJ) $(LDFLAGS) \
		$(LDLIBS) -lshell32 -luser32 -lole32 -o $@

$(PACK_SETUP): $(PACKER_SOURCES) installer/package.h src/common.h src/sha256.h
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) $(PACKER_SOURCES) \
		$(LDFLAGS) $(LDLIBS) -o $@

$(SETUP_TARGET): $(TARGET) $(SETUP_STUB) $(PACK_SETUP)
	./$(PACK_SETUP) $(SETUP_STUB) $(TARGET) $@
endif

$(TEST_TARGET): $(TEST_SOURCES) src/common.h src/download_source.h src/util.h src/sha256.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(UPDATE_TEST_TARGET): $(UPDATE_TEST_SOURCES) src/common.h src/update.h \
		src/platform.h src/util.h src/i18n.h src/sha256.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPDATE_TEST_SOURCES) \
		$(LDFLAGS) $(LDLIBS) -o $@

$(UPDATE_FLOW_TEST_TARGET): $(UPDATE_FLOW_TEST_SOURCES) src/common.h \
		src/update.h src/platform.h src/util.h src/i18n.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPDATE_FLOW_TEST_SOURCES) \
		$(LDFLAGS) $(LDLIBS) -o $@

$(UPDATE_HELPER_TEST_TARGET): tests/test_update_helper.c src/platform.c \
		src/util.c src/sha256.c src/common.h src/platform.h src/util.h \
		src/sha256.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_update_helper.c \
		src/platform.c src/util.c src/sha256.c $(LDFLAGS) $(LDLIBS) -o $@

ifeq ($(OS),Windows_NT)
$(TEST_INSTALLER_PACKAGE): tests/test_installer_package.c installer/package.c \
		installer/package.h src/common.h src/sha256.c src/sha256.h
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) tests/test_installer_package.c \
		installer/package.c src/sha256.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_INSTALLER_PATHLIST): tests/test_installer_pathlist.c installer/pathlist.c \
		installer/pathlist.h
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) -municode \
		tests/test_installer_pathlist.c installer/pathlist.c \
		$(LDFLAGS) $(LDLIBS) -o $@

$(TEST_INSTALLER_LANG): tests/test_lang.c installer/lang.c installer/lang.h \
		installer/resource.h installer/environment.h installer/files.h \
		installer/package.h
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) tests/test_lang.c installer/lang.c \
		$(LDFLAGS) $(LDLIBS) -luser32 -o $@

$(TEST_INSTALLER_DATA_CLEANUP): tests/test_installer_data_cleanup.c \
		installer/files.c installer/files.h installer/package.c \
		installer/package.h src/platform.c src/platform.h src/util.c src/util.h \
		src/sha256.c src/sha256.h src/common.h
	$(CC) -Iinstaller $(CPPFLAGS) $(CFLAGS) \
		tests/test_installer_data_cleanup.c installer/files.c \
		installer/package.c src/platform.c src/util.c src/sha256.c \
		$(LDFLAGS) $(LDLIBS) -lole32 -o $@
endif

test: $(TEST_TARGET) $(UPDATE_TEST_TARGET) $(UPDATE_FLOW_TEST_TARGET) \
		$(UPDATE_HELPER_TEST_TARGET) $(INSTALLER_TEST_TARGETS)
	./$(TEST_TARGET)
	./$(UPDATE_TEST_TARGET)
	./$(UPDATE_FLOW_TEST_TARGET)
ifeq ($(OS),Windows_NT)
	powershell.exe -NoProfile -ExecutionPolicy Bypass \
		-File tests/test_update_helper.ps1 \
		-Helper ./$(UPDATE_HELPER_TEST_TARGET)
	./$(TEST_INSTALLER_PACKAGE)
	./$(TEST_INSTALLER_PATHLIST)
	./$(TEST_INSTALLER_LANG)
	./$(TEST_INSTALLER_DATA_CLEANUP)
else
	sh tests/test_update_helper.sh ./$(UPDATE_HELPER_TEST_TARGET)
endif

integration-test: $(TARGET)
ifeq ($(OS),Windows_NT)
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/test_cli.ps1 -Binary ./$(TARGET)
else
	sh tests/test_cli.sh ./$(TARGET)
endif

installer-test: $(SETUP_TARGET) $(TARGET)
ifeq ($(OS),Windows_NT)
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/test_installer.ps1 \
		-Setup ./$(SETUP_TARGET) -Binary ./$(TARGET)
else
	@echo "installer-test is only available on Windows" >&2
	@false
endif

clean:
ifeq ($(OS),Windows_NT)
	-rm -f $(TARGET) $(TEST_TARGET) $(UPDATE_TEST_TARGET) \
		$(UPDATE_FLOW_TEST_TARGET) $(UPDATE_HELPER_TEST_TARGET) \
		$(RESOURCE_OBJ) $(INSTALLER_FILES) \
		$(TEST_INSTALLER_PACKAGE) $(TEST_INSTALLER_PATHLIST) \
		$(TEST_INSTALLER_LANG) $(TEST_INSTALLER_DATA_CLEANUP)
else
	rm -f $(TARGET) $(TEST_TARGET) $(UPDATE_TEST_TARGET) \
		$(UPDATE_FLOW_TEST_TARGET) $(UPDATE_HELPER_TEST_TARGET)
endif
