PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin
SYSCONFDIR ?= /etc
LIBDIR ?= /usr/lib
CXX ?= g++
CXXFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

TARGETS = granian-wrapper granianenconf graniandisconf
WRAPPER_SRC = src/granian-wrapper.cpp
CONFCTL_SRC = src/granian-confctl.cpp
WRAPPER_OBJ = $(WRAPPER_SRC:.cpp=.o)
ENCONF_OBJ = src/granian-confctl-en.o
DISCONF_OBJ = src/granian-confctl-dis.o

override CPPFLAGS += -DGRANIAN_CONFIG_PATH=\"$(SYSCONFDIR)/granian/granian.conf\" \
	-DGRANIAN_CONFIG_DIR=\"$(SYSCONFDIR)/granian/conf.d\" \
	-DGRANIAN_APPS_ENABLED_DIR=\"$(SYSCONFDIR)/granian/apps-enabled\" \
	-DGRANIAN_APPS_AVAILABLE_DIR=\"$(SYSCONFDIR)/granian/apps-available\" \
	-DGRANIAN_BIN_PATH=\"$(BINDIR)/granian\"
override CXXFLAGS += -std=c++17 -Wall -Wextra -pedantic

.PHONY: all clean install

all: $(TARGETS)

granian-wrapper: $(WRAPPER_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(WRAPPER_OBJ)

granianenconf: $(ENCONF_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(ENCONF_OBJ)

graniandisconf: $(DISCONF_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(DISCONF_OBJ)

$(ENCONF_OBJ): $(CONFCTL_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DGRANIAN_ENABLE_CONF=1 -c -o $@ $<

$(DISCONF_OBJ): $(CONFCTL_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DGRANIAN_ENABLE_CONF=0 -c -o $@ $<

clean:
	rm -f $(TARGETS) $(WRAPPER_OBJ) $(ENCONF_OBJ) $(DISCONF_OBJ)

install: $(TARGETS)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 granian-wrapper $(DESTDIR)$(SBINDIR)/granian-wrapper
	install -m 0755 granianenconf $(DESTDIR)$(SBINDIR)/granianenconf
	install -m 0755 graniandisconf $(DESTDIR)$(SBINDIR)/graniandisconf
	install -d $(DESTDIR)$(SYSCONFDIR)/granian/conf.d
	install -d $(DESTDIR)$(SYSCONFDIR)/granian/apps-available
	install -d $(DESTDIR)$(SYSCONFDIR)/granian/apps-enabled
	install -m 0644 packaging/granian.conf $(DESTDIR)$(SYSCONFDIR)/granian/granian.conf
	install -d $(DESTDIR)$(SYSCONFDIR)/default
	install -m 0644 packaging/default-granian $(DESTDIR)$(SYSCONFDIR)/default/granian
	install -m 0644 packaging/example-app.conf $(DESTDIR)$(SYSCONFDIR)/granian/apps-available/example.conf
	install -d $(DESTDIR)$(SYSCONFDIR)/logrotate.d
	install -m 0644 packaging/granian.logrotate $(DESTDIR)$(SYSCONFDIR)/logrotate.d/granian
	install -d $(DESTDIR)$(LIBDIR)/systemd/system
	install -m 0644 packaging/granian.service $(DESTDIR)$(LIBDIR)/systemd/system/granian.service
