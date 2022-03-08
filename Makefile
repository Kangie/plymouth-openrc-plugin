PLUGIN_NAME  := plymouth
PLUGINS_PATH := $(DESTDIR)/usr/lib/rc/plugins
PKG_NAME     := $(PLUGIN_NAME)-openrc-plugin
PKG_VERSION  := 0.1.3
PKG          := $(PKG_NAME)-$(PKG_VERSION)

BZIP2        := bzip2 -f -9
GZIP         := gzip -f -9
CFLAGS 	     += -fPIC -Wall
LDLIBS       := -leinfo -lrc
LDFLAGS      += -fPIC -shared

INSTALL      := install -D


%.tar.bz2: %.tar
	$(BZIP2) $<

%.tar.gz: %.tar
	$(GZIP) $<


.PHONY: archive clean install uninstall upload


$(PLUGIN_NAME).so: $(PLUGIN_NAME).o
	$(CC) $(LDFLAGS) -Wl,-soname,$@ $+ -o $@ $(LDLIBS)

clean:
	$(RM) *.o *.so

install: $(PLUGIN_NAME).so
	$(INSTALL) $< $(PLUGINS_PATH)/$<

uninstall:
	$(RM) $(PLUGINS_PATH)/$(PLUGIN_NAME).so


$(PKG).tar: .git
	git archive --format=tar --prefix=$(PKG)/ v$(PKG_VERSION) > $@

archive: $(PKG).tar.bz2

upload: $(PKG).tar.bz2
	scp $< dev.gentoo.org:~/public_html/distfiles/sys-boot/$(PKG_NAME)/
