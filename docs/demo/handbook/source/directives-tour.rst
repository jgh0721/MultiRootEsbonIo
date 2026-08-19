.. _directives-tour:

.. index::
   single: directives; tour
   pair: preview; scroll synchronisation

================
A Tour of Markup
================

.. contents:: Sections on this page
   :local:
   :depth: 2

This page is the long one. It exists to be scrolled. Every section below uses a
different construct, and between the constructs sit paragraphs long enough to
wrap into ten or twenty lines at a comfortable editor width, because that is
the situation in which a preview that merely jumps to the nearest line number
starts to feel wrong. When a single source line occupies a fifth of the visible
editor, "scroll to line 214" is no longer a useful instruction; what you want
is for the point you are reading to stay at the same height on both sides of
the window. Drag the editor scrollbar slowly while watching the preview, then
do the same in the preview and watch the editor, and the behaviour should be
symmetric in both directions.

.. epigraph::

   A preview that is merely approximately right is worse than no preview at
   all, because you stop checking it.

   -- attributed to nobody in particular

Paragraph shapes
================

Ordinary paragraphs
-------------------

The paragraph you are reading now is the baseline: no markup other than the
occasional *emphasis*, some **strong emphasis**, and an ``inline literal`` for
things that must be typed exactly. Nothing here is unusual, which is the
point. The vast majority of any real document is exactly this, and everything
else on this page is an exception that has to fit around it without disturbing
it. A document made entirely of admonitions and tables is a specification; a
document made entirely of paragraphs is a novel; technical writing lives
uncomfortably between the two and usually ends up closer to the novel than its
authors expect.

Consider what happens when a paragraph runs long, as this one deliberately
does. In the editor it occupies a single logical line that the view wraps
across many visual rows; in the rendered HTML it becomes a single paragraph
element whose height depends on the width of the preview pane. Neither side
knows how the other wrapped it. The only fact both sides agree on is that this
block of text starts at one source line and ends at another, and that is
exactly the fact the preview builder records for every element it emits. The
position within the block is then interpolated, which is why the two panes can
stay aligned even in the middle of a paragraph that neither side wraps the same
way. Resize the window and the arithmetic changes on one side only; the anchors
are recomputed rather than cached, so the alignment survives the resize.

There is a second reason for paragraphs of this length, and it has nothing to
do with synchronisation. A screenshot of a documentation tool that contains
only headings and bullet lists tells you nothing about how the tool behaves
with real material, because real material is mostly prose. Prose is what
exposes a bad monospace font, an awkward line height, a wrapping mode that
breaks in the middle of a word, and a preview whose measure is so wide that
reading it is unpleasant. So the paragraphs here are as long as the paragraphs
in a document somebody actually has to read, and they are here in quantity.

Line blocks
-----------

| Line blocks keep the breaks you typed,
|     including the indentation,
| which makes them right for addresses,
| for verse, and for the kind of terse
|     step-by-step note that a list
| would make look far too important.

Block quotes and their attributions
-----------------------------------

   A block quote is an indented paragraph and nothing more. It carries no
   semantics beyond "somebody else said this", and it is the oldest construct
   in the entire format.

   -- Anonymous

.. pull-quote::

   Pull quotes are the same idea with a different presentation: the theme is
   expected to make them stand out from the surrounding text rather than
   recede from it.

.. highlights::

   Highlights sit somewhere between the two. Themes rarely style them, which is
   an argument for using block quotes and being done with it.

Admonitions
===========

The whole family, in the order a document usually needs them.

.. note::

   A note is an aside that the reader may skip without consequence.

.. tip::

   A tip is advice that makes something easier but is not required.

.. hint::

   A hint points at an answer without giving it away. It is the rarest of the
   family and is almost always better written as a plain sentence.

.. important::

   Something that is easy to miss and expensive to have missed.

.. attention::

   Stop and read this before continuing with the surrounding procedure.

.. caution::

   Proceeding carelessly here produces a result that looks correct and is not.

.. warning::

   Proceeding here loses data. Warnings should be rare enough that readers
   still take them seriously; a document with a warning on every page has
   taught its readers to skip warnings.

.. danger::

   The strongest of the built-in admonitions. Reserve it for irreversible
   physical or financial consequences, not for a configuration mistake.

.. error::

   Describes a failure state rather than advice about avoiding one.

.. admonition:: A custom title

   The generic form takes whatever title you give it, which is the right choice
   when none of the standard names fits the sentence you actually want to
   write.

.. seealso::

   :doc:`getting-started` for the short version, and :doc:`glossary` for the
   vocabulary used throughout.

.. versionadded:: 3.0
   Immutable command buffers.

.. versionchanged:: 3.2
   Binary trace format.

.. deprecated:: 3.2
   ``orca::legacy_present()`` remains for one more minor release.

Lists of every kind
===================

Bullet and enumerated lists
---------------------------

* A bullet item.
* Another one, this time long enough to wrap across more than a single line so
  that the continuation indentation is visible in the source and invisible in
  the rendered output, which is exactly the asymmetry that makes people
  distrust their own indentation.

  * A nested item.
  * A second nested item.

    * And a third level, which is one more than any document should need.

#. Automatically numbered items use the plain marker.
#. The numbering is assigned at render time.
#. Which means inserting an item in the middle costs nothing.

Definition lists
----------------

command buffer
   A recorded, immutable list of GPU commands.

culling
   Deciding what cannot possibly be visible, before recording any work. The
   definition body may run to several paragraphs.

   This is the second paragraph of the same definition, and it exists to show
   that definitions are not limited to a single line.

Field lists
-----------

:Author: The documentation team
:Status: Draft
:Reviewed: Not yet
:Applies to: 3.2 and later

Option lists
------------

-h, --help          Show usage and exit.
-v, --verbose       Print one line per stage instead of one line per frame.
--budget strict     Fail instead of warning when a stage exceeds its budget.

Horizontal lists
----------------

.. hlist::
   :columns: 3

   * d3d12
   * vulkan
   * metal
   * opengl
   * software
   * null

Code
====

Basic code blocks
-----------------

.. code-block:: cpp
   :caption: Recording a pass
   :linenos:
   :emphasize-lines: 6,7

   void RenderPass::record( CommandBuffer& cb ) const
   {
       cb.begin( attachments_ );
       for( const Draw& draw : draws_ )
       {
           if( !visible( draw ) )     // culling happens before recording,
               continue;              // never inside the recorded buffer
           cb.draw( draw );
       }
       cb.end();
   }

The ``linenos`` option adds line numbers to the rendered block only; the source
line numbers the preview uses for synchronisation are unaffected by it, which
is worth knowing the first time the two sets of numbers disagree on screen.

Setting a default language
--------------------------

.. highlight:: python

After a ``highlight`` directive, plain literal blocks are lexed with that
language::

    def total(budget):
        return budget.cull + budget.record + budget.submit

.. highlight:: default

Parsed literals
---------------

.. parsed-literal::

   orca --scene *your-scene*\ .toml --frames **600**
   orca --replay traces/*timestamp*\ .orcatrace

A parsed literal keeps the whitespace of a literal block while still
interpreting inline markup, which is the only reasonable way to write a command
template with parts the reader is expected to substitute.

Doctest blocks
--------------

.. doctest::

   >>> from orca import FrameBudget
   >>> FrameBudget().total
   4500
   >>> FrameBudget(cull=0).total
   3300

Including part of a file
------------------------

.. literalinclude:: _static/orca_sample.py
   :language: python
   :lines: 22-29
   :caption: The pacing helper
   :emphasize-lines: 3

Tables
======

Grid tables
-----------

+------------------+------------+------------------------------------------+
| Stage            | Budget     | What dominates it                        |
+==================+============+==========================================+
| Culling          | 1200 us    | Traversal of the scene graph, which is   |
|                  |            | dominated by cache misses rather than    |
|                  |            | by arithmetic.                           |
+------------------+------------+------------------------------------------+
| Recording        | 2400 us    | Descriptor churn. Sorting the draws by   |
|                  |            | material before recording is usually     |
|                  |            | worth more than any micro-optimisation   |
|                  |            | inside the loop.                         |
+------------------+------------+------------------------------------------+
| Submission       | 900 us     | Driver-side validation, which you cannot |
|                  |            | influence except by submitting less.     |
+------------------+------------+------------------------------------------+

Simple tables
-------------

=========  =========  ==================================
Backend    Status     Notes
=========  =========  ==================================
d3d12      shipping   The reference implementation.
vulkan     shipping   Behaviour matches d3d12 by test.
metal      partial    No sparse residency yet.
null       internal   Records commands, submits nothing.
=========  =========  ==================================

CSV tables
----------

.. csv-table:: Frame timings, atrium scene
   :header: "Build", "Cull", "Record", "Submit", "Total"
   :widths: 20, 15, 15, 15, 15

   "3.1.7", "1180", "2390", "870", "4440"
   "3.2.0", "1090", "2210", "880", "4180"
   "3.2.1", "1085", "2205", "875", "4165"

List tables
-----------

.. list-table:: Configuration keys
   :header-rows: 1
   :stub-columns: 1
   :widths: 25 15 60

   * - Key
     - Default
     - Meaning
   * - ``renderer.backend``
     - ``d3d12``
     - Which graphics API to use. Changing this invalidates the shader cache,
       so the first frame after the change is slow.
   * - ``renderer.frames_in_flight``
     - ``2``
     - How many frames may be recorded before waiting on the GPU.
   * - ``trace.ring_bytes``
     - ``33554432``
     - Size of the in-memory trace ring. Older records are overwritten rather
       than flushed, so a long session keeps only the recent past.

Figures and images
==================

.. _pipeline-figure:

.. figure:: _images/orca-logo.png
   :width: 200px
   :align: center
   :alt: The Orca Engine mark

   The Orca Engine mark.

   A figure differs from an image in that it has a caption and, optionally, a
   legend like this paragraph. The legend is where the detail goes that would
   make the caption too long to read at a glance.

An inline image sits in the flow of text like this |logo| and is usually a
badge or an icon rather than a picture.

.. |logo| image:: _images/orca-logo.png
   :width: 16px
   :alt: logo

Referring to :numref:`pipeline-figure` by number rather than by name keeps the
text correct when figures are inserted before it.

Mathematics
===========

Inline mathematics such as :math:`t_{frame} = t_{cull} + t_{record} +
t_{submit}` sits in a sentence. Displayed mathematics gets its own block:

.. math::
   :label: budget

   \sum_{i=1}^{n} t_i \le \frac{1000000}{f}

Equation :eq:`budget` says the obvious thing: the stages of a frame have to fit
inside the frame interval, where *f* is the refresh rate in hertz.

Substitutions and replacements
==============================

.. |release| replace:: 3.2.1
.. |dash| unicode:: U+2014
.. |today-is| date:: %Y-%m-%d

The current release is |release|, this page was built on |today-is|, and the
substitution mechanism can insert arbitrary characters |dash| like that em
dash |dash| without leaving the ASCII range in the source file.

Cross references and citations
==============================

Internal targets are defined with an explicit hyperlink target and referenced
with a role: see :ref:`directives-tour` for this page, :ref:`getting-started`
for the previous one, and :term:`frame budget` for a glossary entry. Whole
documents are referenced with :doc:`glossary`, and files that the reader is
meant to keep are offered with :download:`a configuration sample
<_static/orca-config-sample.toml>`.

External links work the way they always have: `the reStructuredText primer
<https://www.sphinx-doc.org/en/master/usage/restructuredtext/basics.html>`_ is
still the shortest useful description of this format.

Footnotes [#note1]_ and citations [CIT2026]_ both defer their content to the
bottom of the document.

.. [#note1] Footnote bodies may be as long as they need to be, and are usually
   longer than the author intended when starting them.

.. [CIT2026] A citation looks like a footnote but is referenced by name, which
   means several places in the document can point at the same entry.

Raw and conditional content
===========================

.. only:: html

   This paragraph exists only in the HTML build, which is the build the preview
   shows, so you will always see it here.

.. raw:: html

   <div style="border-left:3px solid #888;padding:0.4em 0.8em;margin:1em 0">
     Raw HTML is passed through untouched. It is the escape hatch of last
     resort, and it silently disappears in every builder that is not HTML.
   </div>

Containers, topics and sidebars
===============================

.. container:: custom-block

   A container wraps its contents in an element carrying the class you name,
   which is how a theme is given something to style without inventing a new
   directive.

.. topic:: A topic block

   A topic is a small self-contained aside with a title, set apart from the
   surrounding flow. Unlike an admonition it carries no severity.

.. sidebar:: A sidebar
   :subtitle: with a subtitle

   Sidebars float beside the text in themes that support them and fall back to
   a block in themes that do not.

The paragraph after the sidebar continues the main flow, and in a theme that
floats the sidebar it wraps around it. This one is written long enough to
actually wrap, because a sidebar demonstration in which the following paragraph
is two words long demonstrates nothing at all about how the sidebar behaves.

.. rubric:: A rubric is a heading that stays out of the table of contents

Rubrics are the right tool for a heading that would otherwise pollute the
navigation, such as the title of a short list of caveats at the end of a
section.

.. compound::

   A compound paragraph groups several paragraphs into one logical unit.

   This is the second paragraph of the compound, and a theme is entitled to run
   them together as though they were one.

Design elements
===============

The three constructs below come from ``sphinx-design`` rather than from Sphinx
itself, and they only render because this project lists that extension in its
``conf.py``. Open a file from one of the other four projects in this workspace
and the same markup would be reported as an unknown directive, which is exactly
the per-project behaviour the editor is built to keep straight.

.. grid:: 2
   :gutter: 3

   .. grid-item-card:: Budgets
      :link: getting-started
      :link-type: doc

      Advisory during development, enforced in CI.

   .. grid-item-card:: Traces
      :link: handbook-glossary
      :link-type: ref

      A ring buffer of length-prefixed records.

.. dropdown:: A collapsible block
   :color: info
   :icon: info

   Dropdowns start collapsed, which makes them useful for material that most
   readers should skip and a few readers need badly.

.. tab-set::

   .. tab-item:: Source

      .. code-block:: rst

         .. note::

            An admonition.

   .. tab-item:: Result

      .. note::

         An admonition.

Where the synchronisation is easiest to see
===========================================

Scroll from here to the end of the page slowly. Between this heading and the
last line there is nothing but prose, which means there are no headings for a
naive implementation to snap to, and the preview has to interpolate the
position continuously rather than jumping between anchors. If the two panes
stay together across the whole stretch, the mapping is doing its job. This is
also the stretch to watch after resizing the window, because a width change
alters the wrapping on both sides by different amounts, and an implementation
that cached its anchor positions rather than recomputing them will visibly
drift about a screen and a half from here.

The mechanism underneath is not complicated, which is the point. During the
build, every element that docutils produces carries the source line range it
came from, and those ranges are written into the HTML as attributes. The page
script reads them once, builds a table of ranges to vertical positions, and
answers two questions: given a source line, where is it on the page, and given
a vertical position, which source line is there. Everything else, including
the guard against the two panes chasing each other in a loop, is bookkeeping
around those two functions.

What makes it feel right rather than merely correct is the choice of reference
point. Instead of aligning the top of the viewport, both sides align the line
that sits at the same fraction of the window height, by default the middle.
Aligning tops means the thing you are reading, which is usually near the middle
of the window, moves around as you scroll; aligning the middle means it stays
where your eyes already are. The difference sounds academic and is immediately
obvious in use, in the same way that scrolling with the wrong inertia curve
feels wrong long before you can say why.

A last long paragraph, to give the scroll somewhere to go. There is nothing
being demonstrated here beyond length itself: no directive, no reference, no
image, just enough text that the scrollbar thumb is small and the travel is
long. Documentation tools are usually shown off with a page like the ones
above, full of variety, and then used on pages like this one, full of prose.
It seems only fair that the demonstration includes both, and that the boring
half is not hidden. When you are finished here, the sibling projects in this
workspace are worth opening for a different reason: not because their content
differs, but because their themes do, and watching the preview change
appearance as you move between tabs is the shortest possible explanation of
what this editor is for.
