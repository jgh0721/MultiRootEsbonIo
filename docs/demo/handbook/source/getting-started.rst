.. _getting-started:

===============
Getting Started
===============

.. contents:: On this page
   :local:
   :depth: 2

Installing the toolchain
========================

|product| ships as a single archive with no installer. Unpack it anywhere you
have write access, run the binary once so that it can lay out its cache
directory, and you are finished. There is no service to register, no shell
extension, no entry written under ``HKEY_CLASSES_ROOT``, and nothing that
survives deleting the folder. This is a deliberate constraint rather than an
accident of packaging: the engine is used most often on machines that are
imaged nightly, where anything written outside the working directory is gone by
morning and anything that requires administrator rights is simply unavailable.
The cost of that constraint is that upgrades are your responsibility — the
engine will tell you a newer build exists, but it will not replace itself while
you are looking away.

.. code-block:: powershell
   :caption: Unpacking and first run
   :linenos:

   Expand-Archive orca-3.2.0-win64.zip -DestinationPath C:\tools\orca
   cd C:\tools\orca
   .\orca.exe --version
   .\orca.exe --self-check

.. include:: _snippets/support-note.rst

Checking the environment
========================

The ``--self-check`` switch is worth running before anything else. It walks the
same code path the engine uses at start-up, but it prints what it finds instead
of continuing, so a machine that is missing a driver feature tells you about it
in a single line rather than in a crash three minutes into a session.

.. code-block:: text
   :emphasize-lines: 4,5

   orca 3.2.0 (win64, msvc 19.44)
   adapter    : NVIDIA RTX A2000        12288 MB
   backend    : d3d12 (feature level 12_1)
   tracing    : enabled -> traces/
   shader cache: MISS (cold, will rebuild on first frame)
   result     : ok

The two highlighted lines are the ones that change between an ordinary run and
a puzzling one. A cold shader cache explains a long first frame; a disabled
trace explains why the profiler shows nothing.

Your first scene
================

A scene is a plain TOML file. The engine does not have a scene editor and does
not intend to grow one — the file below is the whole interface.

.. literalinclude:: _static/orca-config-sample.toml
   :language: toml
   :caption: orca-config-sample.toml
   :linenos:

Download the file directly with :download:`orca-config-sample.toml
<_static/orca-config-sample.toml>` if you would rather start from a copy than
retype it.

Frame budgets in code
=====================

The Python helper below is what the continuous-integration job uses to decide
whether a build regressed. It is included with ``literalinclude`` restricted to
a line range, which is the usual way to quote part of a file without letting
the quotation drift out of date.

.. literalinclude:: _static/orca_sample.py
   :language: python
   :lines: 6-20
   :caption: FrameBudget and its total

.. tab-set::

   .. tab-item:: Windows

      .. code-block:: powershell

         .\orca.exe --scene scenes\atrium.toml --frames 600

   .. tab-item:: Linux

      .. code-block:: bash

         ./orca --scene scenes/atrium.toml --frames 600

   .. tab-item:: CI

      .. code-block:: yaml

         - run: orca --scene scenes/atrium.toml --frames 600 --budget strict

What to read next
=================

.. dropdown:: If the preview looks wrong
   :color: warning
   :icon: alert

   Check whether the preview is allowed to load remote content. Diagrams that
   are rendered by a script fetched from a CDN cannot appear when the switch is
   off, and the block stays as source text.

.. dropdown:: If you want every directive at once
   :color: primary

   Read :doc:`directives-tour`. It is long on purpose.

Glossary terms used above — :term:`frame budget`, :term:`shader cache` and
:term:`trace record` — are defined in :doc:`glossary`.
