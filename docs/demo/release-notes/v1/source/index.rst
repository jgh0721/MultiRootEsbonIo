=============================
Orca Engine 1.x Release Notes
=============================

This project and its sibling under ``v2`` are two separate Sphinx projects that
share a parent directory. Opening a file from either one maps to the nearer
``conf.py``, which is the case that a naive "one project per workspace" tool
gets wrong.

.. contents::
   :local:

1.9.4
=====

:Released: 2026-02-11

.. rubric:: Fixed

* The trace ring reported its size in records rather than in bytes, so a
  configuration that asked for 32 MB got 32 MB times the record size instead.
* ``end_frame`` could return before the fence was signalled on the null
  backend, which made the null backend useless for the very timing tests it
  exists to make cheap.

1.9.3
=====

:Released: 2026-01-08

.. rubric:: Changed

* Culling now runs on the render thread rather than on a worker. The worker
  version was faster in isolation and slower in every real scene, because the
  hand-off cost more than the traversal it was hiding.

.. rubric:: Known issues

.. warning::

   Shader cache entries written by 1.9.2 and earlier are not read by 1.9.3.
   The first frame after upgrading rebuilds them, which takes a few seconds.

1.9.0
=====

:Released: 2025-11-20

.. rubric:: Added

* A ``--replay`` switch that plays back a captured trace.
* ``orca::net::listen`` for streaming traces to an attached viewer.

.. rubric:: Removed

* The XML trace writer. It had one known user, and that user asked for it to
  be removed.
