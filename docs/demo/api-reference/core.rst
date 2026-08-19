.. _api-core:

=========
Core API
=========

.. contents:: Members
   :local:

Frame control
=============

``orca::begin_frame``
---------------------

:Header: ``<orca/core.hpp>``
:Thread: render thread
:Blocking: no

Starts recording a frame and returns the handle used by every later call in
that frame. The handle is invalidated by ``end_frame`` and using it afterwards
is undefined; in debug builds the engine notices and asserts, which is the only
reason the mistake is ever found quickly.

.. code-block:: cpp

   const auto frame = orca::begin_frame( swapchain );
   scene.record( frame );
   orca::end_frame( frame );        // handle is dead after this line

``orca::end_frame``
-------------------

:Header: ``<orca/core.hpp>``
:Thread: render thread
:Blocking: yes, when all frames in flight are already queued

Closes the current frame and submits it. The call blocks only when the number
of frames already queued has reached ``renderer.frames_in_flight``, which is
the intended back pressure rather than a stall to be worked around.

.. warning::

   Calling ``end_frame`` from a thread other than the one that called
   ``begin_frame`` corrupts the command buffer without reporting an error in
   release builds.

Budgets
=======

``orca::set_budget``
--------------------

:Header: ``<orca/budget.hpp>``
:Thread: any thread
:Blocking: no

Installs a new :term:`frame budget`. Budgets take effect on the next frame, not
on the one being recorded.

.. seealso::

   :doc:`net` for the calls that stream a trace off the machine.
