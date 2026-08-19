=============================
Orca Engine 2.x Release Notes
=============================

The sibling of the ``v1`` project. Same layout, same content shape, a different
HTML theme. Switching between a file here and a file there is the shortest way
to see that the editor is not applying one project's configuration to another
project's file.

.. contents::
   :local:

2.4.1
=====

:Released: 2026-06-30

.. rubric:: Fixed

* Descriptor heaps leaked one allocation per resized swap chain. On a machine
  that never resizes its window this was invisible; on a laptop that docks and
  undocks it was a crash after about forty cycles.

2.4.0
=====

:Released: 2026-05-14

.. rubric:: Added

* Sparse residency on the Vulkan backend.
* ``renderer.frames_in_flight`` may now be set to 3.

.. rubric:: Changed

.. versionchanged:: 2.4
   Budgets are per stage rather than per frame. A configuration written for
   2.3 still loads; the old single number becomes the total and the stages are
   divided in the historical proportion.

2.0.0
=====

:Released: 2026-01-30

.. rubric:: Added

* Command buffers became immutable once submitted.

.. rubric:: Removed

.. deprecated:: 2.0
   ``orca::present()`` in favour of ``orca::end_frame()``. The old name
   remained through the whole 2.x series and is gone in 3.0.

.. note::

   Everything here is invented. The dates, the version numbers and the bugs
   are all made up for the sake of having a second release-notes project.
