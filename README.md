notmuchfs - A virtual maildir file system for notmuch queries
=============================================================

Notmuchfs is free software, released under the GNU General Public
License version 3 (or later).

Copyright © 2012 Tim Stoakes



What is notmuch?
----------------
Notmuch is an awesome email indexing system:
http://notmuchmail.org/



What is notmuchfs?
------------------
Notmuchfs implements a virtual file system which creates maildirs from notmuch
mail query results. This is useful for using notmuch with tools which are not
aware of notmuch, only maildirs - such as mutt.



How does it work?
-----------------
A notmuchfs virtual file system is mounted referencing a particular existing
(real) directory called the 'backing store'.

Directories created within the backing store, when read, appear to have the
standard maildir format (e.g. cur/, new/, tmp/ sub-directories).

Notmuchfs interprets the names of these backing store directories as notmuch
queries, and fills the cur/ maildir sub-directory in the directory in the
virtual file system with corresponding name, to appear to contain messages
which are the result of executing that query (at the instant in time that the
directory is read).

Each virtual maildir message file, when read, appears to have the exact content
of the message referenced by the notmuch query, augmented with an 'X-Label'
header generated automatically by notmuchfs, containing the notmuch tags of
that message.

The name of each virtual maildir message is derived from the real name of
the maildir message, causing maildir flags to be 'passed through' from the
real maildir message to notmuchfs.

The renaming of virtual maildir messages within the same maildir
sub-directory e.g. cur/, is supported. This allows the modification of
maildir flags to be passed back through notmuchfs to the real message, by
renaming the real message.

Renaming virtual messages from new/ to cur/ is allowed, to support standard
maildir behavior. Renaming virtual messages from cur/ to new/ is optionally
also allowed, to support non-maildir-compliant MUAs such as mutt (see mount
option '-o mutt_2476_workaround').

In all rename cases, the notmuch database is informed of the rename, to keep
the database in sync with the real maildir message.

Symbolic links to directories have their targets interpreted as notmuch
queries, providing query 'aliases'.

The unlinking of virtual maildir messages is supported - the real message
file is unlinked.

In general, non-maildir operations such as mkdir() at the root level, rename of
non-maildir files, etc. which are executed within the virtual file system are
passed to the backing store.



Building notmuchfs
------------------
See the INSTALL file for notes on compiling and installing notmuchfs.



Quick start
-----------
First you need a backing store - this is just a directory that contains other
directories which are interpreted as queries.

~~~ sh
$ mkdir ~/my_notmuchfs_backing
~~~

Start notmuchfs, assuming your notmuch database is in ~/.maildir/.notmuch/.

~~~ sh
$ notmuchfs ~/my_notmuchfs_mountpoint \
    -o backing_dir=~/my_notmuchfs_backing \
    -o mail_dir=~/.maildir
~~~

Notmuchfs is not too interesting unless you create at least one query. Queries
are directories within the notmuchfs mount point (equally, within the backing
store).

~~~ sh
$ cd ~/my_notmuchfs_mountpoint
$ mkdir "tag:unread and not tag:spam"
~~~

That name is not too meaningful, so also create a handy alias to that
query directory.

~~~ sh
$ ln -s "tag:unread and not tag:spam" inbox
~~~

See that a virtual maildir is created.

~~~ sh
$ ls ~/my_notmuchfs_backing/inbox
<nothing>
~~~
But...

~~~ sh
$ ls ~/my_notmuchfs_mountpoint/inbox
cur/  new/  tmp/
$ ls ~/my_notmuchfs_mountpoint/inbox/cur/
#maildir#cur#1324430382_4.20193.somehost.net,U=4101:2,S
#maildir#cur#1324427822_1.5704.somehost.net,U=4101:2,S
...
~~~

These are the results of executing the notmuch query "tag:unread and not
tag:spam".


Since 'cat' is not a good MUA, now tell your preferred one to find its maildirs
at ~/my_notmuchfs_mountpoint/. E.g. with mutt, use:
~~~
  set folder=~/my_notmuchfs_mountpoint/
~~~

To unmount:

~~~ sh
$ fusermount -u ~/my_notmuchfs_mountpoint
~~~


Using notmuchfs with mutt
-------------------------
See the README.MUTT.md file for notes on using notmuchfs with mutt.


Debugging
---------
Mount notmuchfs with the '-d' option. Optionally, rebuild notmuchfs with
NOTMUCHFS_DEBUG set to 1 in notmuchfs.c.


Contact
-------
Tim Stoakes <tim@stoakes.net>
http://tim.stoakes.net/
