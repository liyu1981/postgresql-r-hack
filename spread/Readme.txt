SPREAD: A Reliable Multicast and Group Communication Toolkit
-----------------------------------------------------------

/===========================================================================\
| The Spread Group Communication Toolkit.                                   |
| Copyright (c) 1993-2009 Spread Concepts LLC                               |
| All rights reserved.                                                      |
|                                                                           |
| The Spread package is licensed under the Spread Open-Source License.      |
| You may only use this software in compliance with the License.            |
| A copy of the license can be found at http://www.spread.org/license       |
|                                                                           |
| This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF     |
| ANY KIND, either express or implied.                                      |
|                                                                           |
| Spread is developed at Spread Concepts LLC with the support of:	    |
|    The Distributed Systems and Networks Lab, Johns Hopkins University     |
|    The Experimental Networked Systems Lab, George Washington University   |
|                                                                           |
| Creators:                                                                 |
|    Yair Amir             yairamir@cs.jhu.edu                              |
|    Michal Miskin-Amir    michal@spreadconcepts.com                        |
|    Jonathan Stanton      jstanton@gwu.edu                                 |
|    John Schultz	   jschultz@spreadconcepts.com			    |
|                                                                           |
| Major Contributors:                                                       |
|    Ryan Caudy           rcaudy@gmail.com - contribution to process groups.|
|    Claudiu Danilov	  claudiu@acm.org - scalable, wide-area support.    |
|    Cristina Nita-Rotaru crisn@cs.purdue.edu - GC security.                |
|    Theo Schlossnagle    jesus@omniti.com - Perl, autoconf, old skiplist.  |
|    Dan Schoenblum       dansch@dsn.jhu.edu - Java interface.              |
|                                                                           |
| Contributors:                                                             |
|    Ben Laurie	       ben@algroup.co.uk - FreeBSD port and warning fixes   |
|    Daniel Rall       dlr@finemaltcoding.com - Java & networking fixes,    |
|                                               configuration improvements  |
|    Marc Zyngier                        - Windows fixes                    |
|    Jacob Green       jgreen@spreadconcepts.com - Windows support          |
|                                                                           |
| Special thanks to the following for discussions and ideas:                |
|    Ken Birman, Danny Dolev, Jacob Green, Mike Goodrich, Ben Laurie,       |
|    David Shaw, Gene Tsudik, Robbert VanRenesse.                           |
|                                                                           |
| Partial funding provided by the Defense Advanced Research Projects Agency |
| (DARPA) and The National Security Agency (NSA) 2000-2004. The Spread      |
| toolkit is not necessarily endorsed by DARPA or the NSA.                  |
|                                                                           |
| WWW    : http://www.spread.org  and  http://www.spreadconcepts.com        |
| Contact: info@spreadconcepts.com                                          |
|                                                                           |
| Version 4.1.0  built 18/June/2009                                         |
\===========================================================================/

June 18, 2009 Ver 4.1.0 
-----------------------------
This is mainly a bugfix release. Details of all changes can be found in the
daemon/Changelog file. 

Features:
1) Make location of unix domain socket configurable at compile time.

2) Add version numbers to the Spread library. 

3) Add runtime configuration option in spread.conf for the maximum number
of messages Spread should buffer for each client before disconnecting them.

4) Add high-precision timestamps to log file events. To activate enable
EventPreciseTimeStamp flag in spread.conf.

5) Spuser program can send multi-line message.

Internal API additions:
1) Make internal DL_recv/send functions thread-safe and add DL_recvfrom
function to provide source address of the packet.


Fixes:
1) Portability improved for Solaris 10, Windows, platforms with strict 
alignment of ints.

2) Fix crash caused by a wrapped sequence counter (32bit) by triggering
a quick membership when the value gets close so that the sequence value is reset. 

3) Fix crash bug when a membership configuration reload was triggered during
an existing membership change. 

4) Fix locking bug in spread library causing deadlock under certain error paths.

5) Support for newer autoconf versions with better documentation of defines.
Detailed changes in Changelog file.

6) Fix yacc warning in the config file parser.

7) Update to latest Stdutil release which incorporates several small bug and 
compile fixes. 

SOURCE INSTALL:
---------------

The source install uses autoconf and make to support many Unix and unix-like 
platforms as well as Windows 95+.

Windows is supported by a set of Visual Studio desktop and project files in
the win32 subdirectory. These files build the daemon, libraries, and user
programs.

Any not-specifically supported
platform that has reasonably close to normal sockets networking should also 
be easily portable. See the file PORTING for hints on porting.

From the directory where you unpacked the Spread source distribution do 
the following:

1) Run "./configure"

2) Run "make"

3) Run "make install" as a user with rights to write to the selected
installation location. (/usr/local/{bin,man,sbin,include,lib} by default)

4) Create a spread.conf file appropriate to your local configuration. For 
more info on how to do this look at the sample file "sample.spread.conf"
or below in the binary install instructions.

5) To build the java library and documentation see the Readme.txt file 
in the java directory.

6) To build the perl library see the README file in the perl directory.

BINARY INSTALL:
--------

Packaged binary releases of Spread are available from third parties
for several platforms, such as through the FreeBSD Ports system or as 
RPMs for Linux distributions. The Spread binary release consists of 
prebuilt binaries for several OS's that can be run without being installed
offially into the system.

We recommend that if you are experimenting with spread you create a special
'spread' directory (for example /usr/local/spread or /opt/spread) and keep
all the files together there so it is easy to find stuff.  That also makes 
it easier to run multiple architectures as the binaries for each are in their
own subdirectory. This is not necessary though. You can create that directory
anywhere (e.g. your own directory).

1) Unpack the spread-bin-4.x.tar.gz file into the target directory, 
like /opt/spread

2) Look at the Readme.txt for any updates

3) Look in the bin/{archname} directory for the appropriate architecture for
your system. For example, for Mac OS X 10.3 the binaries to run will be in 
bin/powerpc-apple-darwin7.9.0/

The libraries to link with your application will be found in the
/lib/{archname} directory

The include files to link with are in include/ and the documentation
can be found in docs/

4) To run spread you need to modify the 'docs/sample.spread.conf' file for
   your network configuration.  Comments in the sample file explain this.
   Then cp -p sample.spread.conf /usr/local/bin/spread.conf
   (Notice the name change.  The config file must be named that to be found.
    Alternatively you can run spread -c <config_file_name> )

    You can run "spread usage" to see the spread running options.


To use the Java classes and examples you need to have a copy of the 
main 'spread' daemon running.  Then the class files give you the 
equivelent of the libspread.a as a set of java classes.  The user.java,  
and user.class files give you a demonstration application and source code.  
Under the java/docs directory is a full verison of the Spread Java API
provided by javadocs. 

For Microsoft Windows systems use the spread.exe daemon and the libspread.lib 
to link with your programs.


OVERVIEW:
---------

Spread consists of two parts, an executable daemon which is executed on 
each machine which is part of the heavyweight 'machine' group, and a library
 which provides a programming interface for writing group multicast 
applications and which is linked into the application.

	The daemon, called "spread", should be run as a non-priveledged 
user (we created a 'spread' user) and only needs permissions to create a 
socket in /tmp and read its config file which should be in the same 
directory as the executable.  By default the daemon binds to and runs 
off the non-priveledged port 4803 unless the config file indicates otherwise.
	Each daemon can be started independently and if it does not find 
any other members of its defined configuration currently active it runs as 
a machine group of 1 machine.  When other daemons are started they will join
this machine group.

	The machines which are running a daemon with a common config file 
form a 'machine group' (in contrast to a 'process group').  The daemons 
handle multicasting mesages between each other,providing ordering and 
delivery guarantees, detecting and handling failures of nodes or links, and 
managing all applications which are connected to each daemon.

	Each application utilizing the spread system needs to link with 
the 'libspread.a', 'libtspread.a', 'libtspread.lib' or 'libspread.lib' 
library and needs to use the calls defined in the 'sp.h' and 'sp_func.h' 
header files. Documentation for the interface is currenly incomplete, but 
a basic application must do at least the following:

1) Before using any other spread calls you need to 'connect' to a daemon 
   by calling the
	SP_connect(...) call.  This will set a mbox which is an integer 
representing your connection to the daemon.  You use this when making other
spread calls.  You CAN connect to daemons multiple times from the same 
application (and we know of times when this is very useful).

2) If you want to receive messages from a group you need to call SP_join().
Groups are named with standard alphanumeric strings.

3) To send to a group you do NOT need to join it, just call SP_multicast() 
with an appropriate group name.  You will not receive the messages back if 
you are not a member of the group.

4) To be nice to everyone else you should call SP_disconnect() when your 
program is finished using the spread system, if you do not your program's 
disconnection will eventually be detected. 

Some important observations when writing spread programs:

1) All the messages for a particular connection are received during an 
SP_receive() call.  This includes messages from different groups and 
membership messages(like group leave and join), so you must demultiplex 
them yourself.  This is a feature. You can opt to avoid getting membership 
messages all together by indicating that to SP_connect().

2) Spread handles endian differences correctly for metadata and our headers 
but does NOT convert your data for you.  It DOES tell you in the SP_receive 
call whether or not the data originated at machine with a different 
endianness then yours so you can easily use this to convert it yourself 
if necessary. 

3) Each connection to a daemon has a unique 'private group' name which 
can be used to send a message to a particular, and only to a particular, 
application connection.  

4) Spread supports the following ordering/delivery guarantees for messages:

	Unreliable	(least)
	Reliable	(will get there, no ordering)
	Fifo		(reliable and ordered fifo by source)
	Causal		(reliable and all mesg from any source of this level
			are causally ordered)
	Agreed		(reliable and all mesg from any source of this level 
			are totally ordered)
	Safe		(Agreed ordering and mesg will not be delivered to 
			application until the mesg has reached ALL 
			receipients' daemons)

For more detail, much of which IS important, see papers on 
Extended Virtual Sychrony, Transis and Totem.


UTILITY PROGRAMS:
-----------------

	Spread comes with several demonstration programs and useful tools.
They can be found in the main daemon directory as well as the example 
directory. 

1) spmonitor:
	This program has a special interface to spread which allows it to 
control a machine group.  It can terminate all the spread daemons in the 
group, change flow control parameters, and monitor the stats for one or all 
of the machines.  It needs to know the spread.conf file used for a particular
 set of spread daemons.

2) spuser:

	This program is provided with source and uses all the functions of 
the spread interface.  It also acts as a simple client which is useful for 
testing. Reading the source code to this program should show you how to use
all the features of spread, and answer many questions about syntax details.

3) simple_user:

	This is just about the simplest spread program possible and is 
provided with source.  It sends and receives text strings.

4) spflooder:
	
	This is used as one type of performace test.  It 'floods' a spread 
group with data messages. It is provided with source.

5) flush_user:
        This provides a demonstration of using the FL_ interface that provides
Virtual Synchrony semantics to your application. Just like the spuser program
it allows you to join groups and send and receive messages. 

CONTACT:
--------

If you have any questions please feel free to contact:

   spread@spread.org
