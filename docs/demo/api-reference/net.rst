.. _api-net:

============
Network API
============

.. contents:: Members
   :local:

Trace streaming
===============

``orca::net::listen``
---------------------

:Header: ``<orca/net.hpp>``
:Thread: any thread
:Blocking: no

Opens a listening socket that a trace viewer can attach to. The engine writes
:term:`trace record` entries as they are produced; if no viewer is attached the
records go to the ring buffer only.

.. code-block:: cpp

   auto server = orca::net::listen( 0 );   // 0 = pick a free port
   log( "trace viewer port: {}", server.port() );

.. danger::

   The listener performs no authentication. Bind it to a loopback address on
   any machine that is not on an isolated network.

``orca::net::flush``
--------------------

:Header: ``<orca/net.hpp>``
:Thread: any thread
:Blocking: yes

Blocks until every record produced before the call has been handed to the
socket layer. Intended for use at shutdown, where losing the last few
milliseconds of a trace is exactly the part you wanted.

Errors
======

=========================  ==================================================
Code                       Meaning
=========================  ==================================================
``net_error::in_use``      The requested port is taken.
``net_error::refused``     The viewer rejected the protocol version.
``net_error::overflow``    Records were dropped because the viewer fell behind.
=========================  ==================================================
