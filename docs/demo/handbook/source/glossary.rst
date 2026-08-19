.. _handbook-glossary:

========
Glossary
========

The editor harvests these entries directly out of the ``glossary`` directive
and offers them as completions after you type ``:term:``. Hovering a
``:term:`` reference in the body shows the definition without leaving the
editor.

.. glossary::
   :sorted:

   command buffer
      A recorded, immutable list of GPU commands. Once submitted it cannot be
      edited; a change means recording a new one. Immutability is what makes
      replaying a captured frame produce the same picture twice.

   culling
      Deciding which objects cannot possibly affect the final image and
      skipping them before any GPU work is recorded. Cheap culling that is
      slightly wrong costs more than expensive culling that is exactly right.

   frame budget
      The time each stage of a frame is allowed to take, expressed in
      microseconds. Budgets are advisory during development and enforced in
      the continuous-integration job, where exceeding one fails the build.

   present
      Handing a finished image to the display. The call blocks until the
      compositor accepts the image, which is why it is the last thing measured
      and the first thing blamed.

   render pass
      A group of draw calls that share the same set of attachments. Splitting
      a pass is occasionally a win and usually a mistake.

   shader cache
      Compiled shader binaries kept between runs. A cold cache makes the first
      frame after an upgrade far slower than every frame after it; this is
      expected and is not a regression.

   swap chain
      The rotating set of images the engine draws into while the display shows
      the previous one. Two images are enough on a fixed-refresh display;
      three help when the refresh rate varies.

   trace record
      One length-prefixed binary entry describing a single event: which stage,
      which thread, when it started and how long it took. Records are written
      to a ring buffer, so a long session keeps only the recent past.

   viewport
      The rectangle of the target image that a pass is allowed to write to.
      Everything outside it is left untouched rather than cleared.
