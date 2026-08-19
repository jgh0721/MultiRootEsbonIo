.. _handbook-root:

=====================
Orca Engine Handbook
=====================

.. only:: html

   .. image:: _images/orca-logo.png
      :alt: Orca Engine
      :width: 120px
      :align: right

|product| is a fictional real-time rendering engine that exists for exactly one
reason: to give this editor something substantial to render. Every page in this
handbook is written to exercise a different corner of reStructuredText, so that
a screenshot of any page shows something worth looking at. If you are reading
this inside the editor, the pane on the right is a real Sphinx build of this
very file, produced by the bundled Python runtime rather than by a web service,
which is why the cross references, the figure numbering and the admonition
styling all match what a full ``sphinx-build`` would give you.

This project is one of five Sphinx projects that live under a single workspace
folder. The other four sit in sibling directories, each with its own
``conf.py`` and its own HTML theme, and the editor keeps them apart without
being told which file belongs to which project. Open a file from any of them
and the preview switches to that project's theme; the change in appearance is
the mapping becoming visible.

.. grid:: 3
   :gutter: 2

   .. grid-item-card:: Deterministic
      :text-align: center

      Frame timing is pinned to the display refresh, not to a wall clock.

   .. grid-item-card:: Portable
      :text-align: center

      One binary, no installer, no registry keys, no service to register.

   .. grid-item-card:: Inspectable
      :text-align: center

      Every pass writes a trace record you can read without a debugger.

Contents
========

.. toctree::
   :maxdepth: 2
   :caption: Handbook

   getting-started
   directives-tour
   glossary

What this handbook covers
=========================

The handbook is deliberately uneven. :doc:`getting-started` is short and
practical; :doc:`directives-tour` is long and dense, because its job is to be
long and dense — it is the page used to demonstrate that scrolling the editor
and scrolling the preview stay in agreement across hundreds of lines of text
and dozens of block-level constructs. If the two panes ever drift apart, that
page is where you would notice it first, which is precisely why it is shaped
the way it is.

.. note::

   Nothing in this workspace describes real software. Version numbers, API
   names and release dates are invented. The point of the text is its shape:
   long paragraphs, many lines, and a wide spread of directives.

Architecture at a glance
========================

.. mermaid::

   graph LR
       A[Scene graph] --> B[Culling]
       B --> C[Command buffer]
       C --> D[GPU submit]
       D --> E[Present]
       B -.-> F[(Trace log)]
       C -.-> F

The pipeline above is drawn by mermaid, which loads its renderer from a CDN.
The editor blocks outbound requests from the preview when
*Settings → Preview → load scripts, styles and images from the internet* is
turned off; with the switch off this block stays as plain text instead of
becoming a diagram. That is a deliberate choice rather than a rendering bug —
a preview that silently reaches out to the network is a preview you cannot
trust in an air-gapped office.

.. seealso::

   :doc:`directives-tour` walks through every construct used in this project,
   with the source and the rendered result side by side.

Release status
==============

.. versionadded:: 3.0
   Command buffers became immutable once submitted.

.. versionchanged:: 3.2
   The trace log moved from CSV to a length-prefixed binary format.

.. deprecated:: 3.2
   ``orca::legacy_present()`` remains for one more minor release.

.. list-table:: Supported targets
   :header-rows: 1
   :widths: 30 20 50

   * - Platform
     - Status
     - Notes
   * - Windows 11 (x64)
     - Supported
     - Primary development target; every build is tested here first.
   * - Windows 10 (x64)
     - Supported
     - Requires the 22H2 update for the newer presentation path.
   * - Linux (x64)
     - Experimental
     - Builds and runs; the trace viewer is not ported yet.
