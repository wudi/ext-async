--TEST--
Task indicates if currently executed code is running inside a task.
--SKIPIF--
<?php
if (!extension_loaded('task')) echo 'Test requires the task extension to be loaded';
?>
--FILE--
<?php

namespace Concurrent;

$f = new Fiber(function () {
    var_dump(Fiber::isRunning(), Task::isRunning());
});

var_dump(Task::isRunning());

TaskScheduler::run(function () {
    var_dump(Fiber::isRunning(), Task::isRunning());
});

var_dump(Task::isRunning());

$f->start();

var_dump(Task::isRunning());

?>
--EXPECT--
bool(false)
bool(false)
bool(true)
bool(false)
bool(true)
bool(false)
bool(false)
