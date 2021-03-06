--TEST--
Stream watcher can be awaited within a task.
--SKIPIF--
<?php
if (!extension_loaded('task')) echo 'Test requires the task extension to be loaded';
?>
--FILE--
<?php

namespace Concurrent;

function pair()
{
    return array_map(function ($s) {
        stream_set_blocking($s, false);
        stream_set_read_buffer($s, 0);
        stream_set_write_buffer($s, 0);
        
        return $s;
    }, stream_socket_pair((DIRECTORY_SEPARATOR == '\\') ? STREAM_PF_INET : STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP));
}

list ($a, $b) = pair();

Task::async(function () use ($b) {
    $watcher = new StreamWatcher($b);

    while (!feof($b)) {
        var_dump(stream_get_contents($b));

        $watcher->awaitReadable();
    }

    var_dump(stream_get_contents($b));
});

fwrite($a, 'Hello');

(new Timer(20))->awaitTimeout();

fwrite($a, 'World');

fclose($a);

--EXPECT--
string(5) "Hello"
string(5) "World"
string(0) ""
