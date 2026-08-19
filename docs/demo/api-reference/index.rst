.. _api-root:

============================
Orca Engine API Reference
============================

This project keeps its ``conf.py`` at the project root instead of under a
``source`` directory. It is the same kind of project as the handbook next door,
laid out differently, and the editor resolves both the same way: the nearest
directory containing a ``conf.py``, walking up from the file you opened.

.. toctree::
   :maxdepth: 2

   core
   net

Conventions
===========

Every entry below lists the header, the thread affinity, and whether the call
may block. Anything that may block says so; anything that does not say so must
not block, and a build that violates that is a bug rather than a slow path.

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Term
     - Meaning
     - Notes
   * - ``[[nodiscard]]``
     - Result must be used
     - Applied to every call that can fail.
   * - *render thread*
     - Affinity
     - The thread that recorded the current command buffer.
   * - *any thread*
     - Affinity
     - Safe to call concurrently from several threads.

.. note::

   Names in this reference are invented. The point of the project is to be a
   second real Sphinx project in the same workspace, with its own theme.

Glossary
========

.. glossary::

   frame budget
      The time each stage of a frame is allowed to take, in microseconds.
      Advisory during development, enforced in the continuous-integration job.

   trace record
      One length-prefixed binary entry describing a single event: which stage,
      which thread, when it started and how long it took.

   frames in flight
      How many frames may be recorded before the engine waits on the GPU.
