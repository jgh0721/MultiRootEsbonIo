==========
Scratchpad
==========

Another file with no project above it. It lives one directory down from the
workspace root, which changes nothing: the search for a ``conf.py`` walks
upwards and finds none before the root, so this file gets a synthesised project
of its own, separate from the one made for ``TODO.rst``.

Notes from the profiling session
================================

.. csv-table::
   :header: "Scene", "Frames", "p50 (us)", "p99 (us)"

   "atrium", "600", "4180", "5240"
   "corridor", "600", "3110", "3980"
   "exterior", "600", "6890", "9120"

The exterior scene is the interesting one. Its p99 is more than a third above
its median, and every one of the slow frames lands immediately after the
streaming system admits a new texture page. That is not a rendering problem at
all; it is an I/O problem wearing a rendering problem's clothes, and no amount
of staring at the draw call list was ever going to show it.

.. tip::

   When the median is fine and the tail is not, look for something that happens
   occasionally rather than something that happens every frame.

Open questions
==============

* Does the ``exterior`` tail follow the page size, or the number of pages?
* Is the admission threshold worth making adaptive, or is a fixed number fine
  as long as it is the *right* fixed number?
* Would recording the streaming events into the same trace ring make the
  correlation obvious instead of inferred?
