######################
Plymouth OpenRC Plugin
######################

This is a plugin for OpenRC that displays a Plymouth splash screen during boot.

Installation
============

Users of this package are probably on Gentoo; just ``emerge sys-boot/plymouth-openrc-plugin``.

If you're not on Gentoo you'll just need to install OpenRC and Plymouth then build the plugin.

Building
========

You don't need to set any options on the ``meson setup`` invocation, they're just included here for
reference for those that want to change them.

.. code-block:: console

    user@workstation $ meson setup builddir --prefix=/usr \
        -Ddebug=true \
        -Dpid-file=/run/plymouth/plymouth-openrc-plugin.pid \
        -Drun-dir=/run/plymouth \
        -Dplymouth-binary=/bin/plymouth \
        -Dplymouthd-binary=/sbin/plymouthd
    user@workstation $ ninja -C builddir
    user@workstation $ sudo ninja -C builddir install

Configuration Options
====================

- ``debug``: Enable debug output (default: false)
- ``pid-file``: Path to PID file (default: /run/plymouth/plymouth-openrc-plugin.pid)
- ``run-dir``: Run directory for Plymouth (default: /run/plymouth)
- ``plymouth-binary``: Path to plymouth client binary (default: /bin/plymouth)
- ``plymouthd-binary``: Path to plymouthd daemon binary (default: /sbin/plymouthd)
