=========================
Workspace scratch: TODO
=========================

This file sits directly in the workspace root. There is no ``conf.py`` above it
and none beside it, so it belongs to none of the five projects in this
workspace. The editor still previews it: it synthesises a minimal project for
the file alone, in a temporary directory, and builds that. The log panel says
so when the file is opened.

Why this matters
================

A workspace is rarely tidy. There is always a stray note, a snippet somebody
pasted, a file that will become a document once it is finished. A tool that
refuses to preview anything without a ``conf.py`` makes those files
second-class; a tool that guesses which project they belong to eventually
guesses wrong and builds them against the wrong configuration.

.. note::

   The synthesised project uses ``alabaster``, so this page looks different
   from all five real projects. That is a useful accident: the plain look is
   itself a signal that the file has no project.

Things to do
============

.. list-table::
   :header-rows: 1
   :widths: 12 58 30

   * - Done
     - Item
     - Notes
   * - [x]
     - Write the handbook tour page
     - Long on purpose.
   * - [x]
     - Two release-notes projects under one parent
     - Nearest-project mapping.
   * - [ ]
     - Decide whether the notes folder should become a project
     - Probably not; it is more useful as it is.

Directives still work here
==========================

.. warning::

   Everything reStructuredText offers is available in a file with no project.
   What is *not* available is anything a project's ``conf.py`` would have
   added: extensions, substitutions defined in ``rst_prolog``, and the theme.

.. code-block:: text

   $ orca --self-check
   result : ok
