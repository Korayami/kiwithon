.. image:: assets/icon.png
   :alt: Kiwithon Logo
   :align: center
   :width: 96px

This is Kiwithon version 3.16.0 alpha 1+dev (CPython 3.16 Optimization Fork)
===========================================================================

.. image:: https://img.shields.io/badge/version-3.16.0a1%2Bdev%20%7C%203.15%20Optimized-brightgreen.svg?style=flat-square
   :alt: Version 3.16.0a1+dev | 3.15 Optimized
   :target: https://github.com/Korayami/kiwithon/releases

.. image:: https://img.shields.io/badge/architecture-ARM64%20(Cortex--A76)%20%7C%20x86__64-blue.svg?style=flat-square
   :alt: ARM64 & x86_64
   :target: https://github.com/Korayami/kiwithon

.. image:: https://img.shields.io/badge/acceleration-Tier--2%20JIT%20%7C%20C11%20SIMD%20%7C%20Immortal%20Heap-orange.svg?style=flat-square
   :alt: Tier-2 JIT & C11 SIMD
   :target: https://github.com/Korayami/kiwithon

.. image:: https://img.shields.io/badge/ecosystem-KiwiSpaceONE%20Integrated-success.svg?style=flat-square
   :alt: KiwiSpaceONE Integrated
   :target: https://github.com/Varesine/KiwiSpaceONE

.. image:: https://img.shields.io/badge/compatibility-100%25%20CPython%20%26%20pip%20ABI-informational.svg?style=flat-square
   :alt: 100% CPython & pip ABI
   :target: https://pypi.org/

.. image:: https://img.shields.io/badge/license-PSF--2.0-blueviolet.svg?style=flat-square
   :alt: PSF-2.0
   :target: LICENSE

.. image:: assets/kiwi_logo_render.png
   :alt: Kiwithon 3D Logo
   :align: center
   :width: 700px


Copyright © 2001 Python Software Foundation.  All rights reserved.

See the end of this file for further copyright and license information.

For the full GitHub-formatted documentation with architecture details, benchmarks,
and guides, see `README.md <README.md>`_.

.. contents::

General Information
-------------------

- Website: https://github.com/Korayami/kiwithon
- Source code: https://github.com/Korayami/kiwithon
- Issue tracker: https://github.com/Korayami/kiwithon/issues
- Upstream Python: https://www.python.org
- Documentation: https://docs.python.org
- Developer's Guide: https://devguide.python.org/

Contributing to Kiwithon
------------------------

For instructions on contributing to CPython and Kiwithon development,
see the `Developer Guide`_ and `AGENTS.md <AGENTS.md>`_.

.. _Developer Guide: https://devguide.python.org/

Using Kiwithon
--------------

Kiwithon provides a drop-in replacement for Python with built-in SIMD acceleration,
immortalized heap allocation, Tier-2 JIT tuning for Raspberry Pi 5 / ARM Cortex-A76,
and native integrations for KiwiSpaceONE. Installed binaries can be invoked as either
``kiwithon`` / ``kiwithon3`` or ``python`` / ``python3``.

Quick Installation
------------------

Install Kiwithon with a single command::

    curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh

To build directly from git source during installation::

    curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- --raw

Updating Kiwithon
-----------------

Once Kiwithon is installed, you can keep it up to date directly from the command line:

- **Binary update**: Fetch and install the latest prebuilt release binary into your system::

    kiwithon --update

- **Source rebuild**: Pull the latest git changes, recompile, and install the freshly built binary::

    kiwithon --raw-update

Build Instructions
------------------

On Unix, Linux, BSD, macOS, and Cygwin::

    ./configure
    make
    make test
    sudo make install

This will install Python as ``python3``.

You can pass many options to the configure script; run ``./configure --help``
to find out more.  On macOS case-insensitive file systems and on Cygwin,
the executable is called ``python.exe``; elsewhere it's just ``python``.

Building a complete Python installation requires the use of various
additional third-party libraries, depending on your build platform and
configure options.  Not all standard library modules are buildable or
usable on all platforms.  Refer to the
`Install dependencies <https://devguide.python.org/getting-started/setup-building.html#build-dependencies>`_
section of the `Developer Guide`_ for current detailed information on
dependencies for various Linux distributions and macOS.

On macOS, there are additional configure and build options related
to macOS framework and universal builds.  Refer to `Mac/README.rst
<https://github.com/python/cpython/blob/main/Mac/README.rst>`_.

On Windows, see `PCbuild/readme.txt
<https://github.com/python/cpython/blob/main/PCbuild/readme.txt>`_.

To build Windows packages, see `PC/layout/README.md
<https://github.com/python/cpython/blob/main/PC/layout/README.md>`_.

If you wish, you can create a subdirectory and invoke configure from there.
For example::

    mkdir debug
    cd debug
    ../configure --with-pydebug
    make
    make test

(This will fail if you *also* built at the top-level directory.  You should do
a ``make clean`` at the top-level first.)

To get an optimized build of Python, run ``configure --enable-optimizations``
before you run ``make``.  This sets the default make targets up to enable
Profile Guided Optimization (PGO) and may be used to auto-enable Link Time
Optimization (LTO) on some platforms.  For more details, see the sections
below.

Profile Guided Optimization
^^^^^^^^^^^^^^^^^^^^^^^^^^^

PGO takes advantage of recent versions of the GCC or Clang compilers.  If used,
either via ``configure --enable-optimizations`` or by manually running
``make profile-opt`` regardless of configure flags, the optimized build
process will perform the following steps:

The entire Python directory is cleaned of temporary files that may have
resulted from a previous compilation.

An instrumented version of the interpreter is built, using suitable compiler
flags for each flavor. Note that this is just an intermediary step.  The
binary resulting from this step is not good for real-life workloads as it has
profiling instructions embedded inside.

After the instrumented interpreter is built, the Makefile will run a training
workload.  This is necessary in order to profile the interpreter's execution.
Note also that any output, both stdout and stderr, that may appear at this step
is suppressed.

The final step is to build the actual interpreter, using the information
collected from the instrumented one.  The end result will be a Python binary
that is optimized; suitable for distribution or production installation.


Link Time Optimization
^^^^^^^^^^^^^^^^^^^^^^

Enabled via configure's ``--with-lto`` flag.  LTO takes advantage of the
ability of recent compiler toolchains to optimize across the otherwise
arbitrary ``.o`` file boundary when building final executables or shared
libraries for additional performance gains.


What's New
----------

We have a comprehensive overview of the changes in the `What's new in Python
3.16 <https://docs.python.org/3.16/whatsnew/3.16.html>`_ document.  For a more
detailed change log, read `Misc/NEWS
<https://github.com/python/cpython/tree/main/Misc/NEWS.d>`_, but a full
accounting of changes can only be gleaned from the `commit history
<https://github.com/python/cpython/commits/main>`_.

If you want to install multiple versions of Python, see the section below
entitled "Installing multiple versions".


Documentation
-------------

`Documentation for Python 3.16 <https://docs.python.org/3.16/>`_ is online,
updated daily.

It can also be downloaded in many formats for faster access.  The documentation
is downloadable in HTML, EPUB, and reStructuredText formats; the latter version
is primarily for documentation authors, translators, and people with special
formatting requirements.

For information about building Python's documentation, refer to `Doc/README.rst
<https://github.com/python/cpython/blob/main/Doc/README.rst>`_.


Testing
-------

To test the interpreter, type ``make test`` in the top-level directory.  The
test set produces some output.  You can generally ignore the messages about
skipped tests due to optional features which can't be imported.  If a message
is printed about a failed test or a traceback or core dump is produced,
something is wrong.

By default, tests are prevented from overusing resources like disk space and
memory.  To enable these tests, run ``make buildbottest``.

If any tests fail, you can re-run the failing test(s) in verbose mode.  For
example, if ``test_os`` and ``test_gdb`` failed, you can run::

    make test TESTOPTS="-v test_os test_gdb"

If the failure persists and appears to be a problem with Python rather than
your environment, you can `file a bug report
<https://github.com/python/cpython/issues>`_ and include relevant output from
that command to show the issue.

See `Running & Writing Tests <https://devguide.python.org/testing/run-write-tests.html>`_
for more on running tests.

Installing multiple versions
----------------------------

On Unix and Mac systems if you intend to install multiple versions of Python
using the same installation prefix (``--prefix`` argument to the configure
script) you must take care that your primary python executable is not
overwritten by the installation of a different version.  All files and
directories installed using ``make altinstall`` contain the major and minor
version and can thus live side-by-side.  ``make install`` also creates
``${prefix}/bin/python3`` which refers to ``${prefix}/bin/python3.X``.  If you
intend to install multiple versions using the same prefix you must decide which
version (if any) is your "primary" version.  Install that version using
``make install``.  Install all other versions using ``make altinstall``.

For example, if you want to install Python 2.7, 3.6, and 3.15 with 3.15 being the
primary version, you would execute ``make install`` in your 3.15 build directory
and ``make altinstall`` in the others.


Release Schedule
----------------

See `PEP 826 <https://peps.python.org/pep-0826/>`__ for Python 3.16 release details.


Copyright and License Information
---------------------------------


Copyright © 2001 Python Software Foundation.  All rights reserved.

Copyright © 2000 BeOpen.com.  All rights reserved.

Copyright © 1995-2001 Corporation for National Research Initiatives.  All
rights reserved.

Copyright © 1991-1995 Stichting Mathematisch Centrum.  All rights reserved.

See the `LICENSE <https://github.com/python/cpython/blob/main/LICENSE>`_ for
information on the history of this software, terms & conditions for usage, and a
DISCLAIMER OF ALL WARRANTIES.

This Python distribution contains *no* GNU General Public License (GPL) code,
so it may be used in proprietary projects.  There are interfaces to some GNU
code but these are entirely optional.

All trademarks referenced herein are property of their respective holders.
