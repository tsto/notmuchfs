notmuchfs - A virtual maildir file system for notmuch queries
=============================================================

Notmuchfs is free software, released under the GNU General Public
License version 3 (or later).

Copyright © 2012 Tim Stoakes


Quick start - using notmuchfs with mutt
---------------------------------------
Super quick start:
* Add 'notmuchfs/mutt/bin' to $PATH.
* Add 'source /path/to/notmuchfs/mutt/.muttrc' to the top of your '.muttrc', and
  edit that file to taste, etc.
* Mount notmuchfs, being sure to pass the option '-o mutt_2476_workaround'.
* Run mutt!


Longer - using notmuchfs with mutt
----------------------------------
Notmuchfs was developed because I wanted to use mutt with notmuch, but the mutt
codebase was... difficult.

Mutt understands maildirs, so the simplest thing I could think of was to
emulate maildirs on top of notmuch, and point mutt at them. See mutt/.muttrc
for a complete example.

It is assumed that notmuchfs is up and running already. If not, get started
by reading README.md, for example:

~~~ sh
  $ mkdir ~/my_notmuchfs_backing
  $ cd ~/my_notmuchfs_mountpoint
  $ mkdir "tag:unread and not tag:spam"
  $ ln -s "tag:unread and not tag:spam" inbox
  $ notmuchfs ~/my_notmuchfs_mountpoint \
      -o backing_dir=~/my_notmuchfs_backing \
      -o mail_dir=~/.maildir \
      -o mutt_2476_workaround
~~~

Now, just like any other maildir, simply point the 'folder' variable at
notmuchfs and our new notmuchfs inbox:

~~~
  set folder=~/my_notmuchfs_mountpoint/
  set spoolfile=+inbox/
~~~

When the search results have changed e.g. delivery of new mail, mutt will not
automatically notice. So we need to reload the inbox - with a handy macro such
as:
~~~
  macro index "#" '<sync-mailbox><change-folder>^<enter>' "Reload mailbox"
~~~

It may be nice to colorise messages with particular flags in the index,
perhaps something like:
~~~
  color index    red      default        "~h '^X-Label: .*interesting_tag.*$'"
~~~


This is however, essentially read-only - not so useful.


(The following section assumes that notmuchfs/mutt/bin/ is in your $PATH, and
'formail' is installed on the system.)

Remembering that notmuchfs queries are just directories, making a new search is
simple. With a macro like this, use the included 'mutt/bin/prompt_mkdir' script
to create new query directories from within mutt:

~~~
  macro index "S" "<shell-escape>prompt_mkdir $folder <enter><change-folder>?" "Create a new notmuchfs query mailbox"
~~~



Now we need a way to modify the tags on messages. Again, with a macro like
this, use the included 'notmuch_tag' script to alter the tags on a single
message, or a (mutt) tagged set of messages interactively:

~~~
  macro index,pager ",T" "<pipe-message>formail -d -xMessage-id: -s | tr -d \"<>\" | notmuch_tag<enter>" "Manage notmuch tags"
~~~

Find yourself entering the same interactive tag modifications over and over?
Perhaps add new macros to taste:

~~~
  macro index,pager ",tw"   "<pipe-message>formail -d -xMessage-id: -s | tr -d \"<>\" | notmuch_tag +watch<enter>" "Add 'watch' tag"
~~~



Mutt bug 2476
-------------
Mutt is not compliant with the maildir specification, see:
* http://dev.mutt.org/trac/ticket/2476
* http://notmuchmail.org/pipermail/notmuch/2011/004833.html

Notmuchfs can work around this issue, if mounted with the
~~~ sh
  '-o mutt_2476_workaround'
~~~
mount option. If you use mutt, you want to use this option. Mutt is basically
unusable without it.

