mysql_slowlogd - Serves MySQL slow logs via HTTP
================================================

**mysql_slowlogd** is a daemon that makes MySQL's slow query
log available via HTTP as a streaming download. It is intended
to be used in conjunction with [Percona Playback] [1] as part of
larger system to keep the buffer pool of standby MySQL servers
warm. It can also be used with Percona Toolkit's pt-query-digest
utility to run digest reports on a host different than the
database server.

Getting Started
---------------

To build from source:

    $ ./configure
    $ make
    $ make install

Usage
-----

Start the daemon:

    mysql_slowlogd -f /path/to/slow_query.log

Fetch the slow log:

    curl -s http://localhost:3307/slow

Keep your MySQL replica buffer pool warm:

    # Requires Percona Playback version >= 0.6

    wget -q -O - http://master_server:3307/slow | percona-playback --mysql-host 127.0.0.1 --mysql-username playback --mysql-password PaSSwOrd --mysql-schema schema_name --query-log-stdin --dispatcher-plugin thread-pool --thread-pool-threads-count 100 --session-init-query \"set innodb_fake_changes=1\" > /var/log/playback.log 2>&1 &

Description
-----------

Many applications use MySQL's built-in replication to provide
high-availability with manual or automated failover. One issue with
such architectures is that, after failover, the former slave's buffer
pool [may be cold] [2] and the server [becomes IO bound] [3].

![Playback Architecture](https://raw.github.com/groupon/mysql_slowlogd/master/doc/playback_architecture.png)

mysql_slowlogd can be used as part of a solution to keep the buffer
pool of the standby database warm. It serves the MySQL slow log via
HTTP, making the slow log accessible from the slave via curl or
wget. It has a mechanism, like <i>xtail</i>, that continues streaming
after log rotation.

[Percona Playback] [1] (version >= 0.6) is a tool for replaying MySQL
slow query log captures against a different database server. Version
0.6 includes the ability to stream the slow query log from standard
input. It also includes another important feature in this context:
a new thread pool impelementation that handles ungracefully closed
connections. The slave buffer pool can be kept warm by running Percona
Playback using the slow log streamed from the master via
mysql_slowlogd.

### Rate Limiting ###

Another use case for mysql_slowlogd is generating performance reports
on a different host. [Percona Toolkit] [4] provides pt-query-digest, a
great tool for summarizing slow logs. Down-sampling slow logs is
usually necessary on databases with large QPS. For this reason,
mysql_slowlogd includes rate limiting in a similar fashion to Percona
Server's [log_slow_rate_limit] [5]
option.

The <i>rate_limit</i> query string parameter specifies that only a
fraction of queries should be output. Output is enabled for every
<i>n</i>th query. By default, <i>n</i> is 1, so every query in the
slow query log will be output. For example, if <i>rate_limit> is set
to 100, then one percent of queries would be output.

    curl -s http://localhost:3307/slow?rate_limit=100

License
-------

    Copyright (c) 2012, Groupon, Inc.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the distribution.

    Neither the name of Groupon nor the names of its contributors may be
    used to endorse or promote products derived from this software without
    specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
    OF THE POSSIBILITY OF SUCH DAMAGE.

Meta
----

* Home: <https://github.com/groupon/mysql_slowlogd>
* Bugs: <https://github.com/groupon/mysql_slowlogd/issues>

[1]: https://launchpad.net/percona-playback "Percona Playback"
[2]: http://techcrunch.com/2012/09/14/github-explains-this-weeks-outage-and-poor-performance/ "Techcrunch coverage of Github's September 10, 2012 outage due to cold MySQL buffer pool"
[3]: https://fosdem.org/2013/schedule/event/bp_hot_slave/ "Peter Boros' Talk at FOSDEM 2013"
[4]: http://www.percona.com/software/percona-toolkit "Percona Toolkit"
[5]: http://www.percona.com/doc/percona-server/5.5/diagnostics/slow_extended_55.html#log_slow_rate_limit