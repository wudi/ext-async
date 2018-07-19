/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>                 |
  +----------------------------------------------------------------------+
*/


#include "php.h"
#include "zend.h"
#include "zend_API.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"

#include "php_async.h"

ZEND_DECLARE_MODULE_GLOBALS(async)

zend_class_entry *async_task_scheduler_ce;
zend_class_entry *async_loop_task_scheduler_ce;

static zend_object_handlers async_task_scheduler_handlers;
static zend_object_handlers async_loop_task_scheduler_handlers;

static zend_string *str_activate;
static zend_string *str_runloop;
static zend_string *str_stoploop;


static inline void enqueue(async_task_queue *q, async_task *task)
{
	task->next = NULL;

	if (q->last == NULL) {
		task->prev = NULL;

		q->first = task;
		q->last = task;
	} else {
		task->prev = q->last;

		q->last->next = task;
		q->last = task;
	}

	q->size++;
}

static inline async_task *dequeue(async_task_queue *q)
{
	async_task *task;

	if (q->first == NULL) {
		return NULL;
	}

	task = q->first;
	q->first = task->next;

	if (q->first != NULL) {
		q->first->prev = NULL;
	}

	if (q->last == task) {
		q->last = NULL;
	}

	q->size--;

	task->next = NULL;
	task->prev = NULL;

	return task;
}

static inline void detach(async_task_queue *q, async_task *task)
{
	if (task->prev != NULL) {
		task->prev->next = task->next;
	}

	if (task->next != NULL) {
		task->next->prev = task->prev;
	}

	if (q->first == task) {
		q->first = task->next;
	}

	if (q->last == task) {
		q->last = task->prev;
	}

	task->next = NULL;
	task->prev = NULL;

	q->size--;
}


static async_task_scheduler *async_task_scheduler_obj(zend_object *obj)
{
	return (async_task_scheduler *)((char *)obj - obj->handlers->offset);
}

static void async_task_scheduler_dispose(async_task_scheduler *scheduler)
{
	async_task_scheduler *prev;
	async_task *task;

	prev = ASYNC_G(current_scheduler);
	ASYNC_G(current_scheduler) = scheduler;

	if (!scheduler->running && scheduler->modified) {
		async_task_scheduler_run_loop(scheduler);
	}

	while (scheduler->ready.size > 0) {
		task = dequeue(&scheduler->ready);

		async_task_dispose(task);

		OBJ_RELEASE(&task->fiber.std);
	}

	while (scheduler->suspended.size > 0) {
		task = dequeue(&scheduler->suspended);

		async_task_dispose(task);

		OBJ_RELEASE(&task->fiber.std);
	}

	ASYNC_G(current_scheduler) = prev;
}

async_task_scheduler *async_task_scheduler_get()
{
	async_task_scheduler *scheduler;
	async_task_scheduler_stack *stack;

	scheduler = ASYNC_G(current_scheduler);

	if (scheduler != NULL) {
		return scheduler;
	}

	stack = ASYNC_G(scheduler_stack);

	if (stack != NULL && stack->top != NULL) {
		return stack->top->scheduler;
	}

	scheduler = ASYNC_G(scheduler);

	if (scheduler != NULL) {
		return scheduler;
	}

	scheduler = emalloc(sizeof(async_task_scheduler));
	ZEND_SECURE_ZERO(scheduler, sizeof(async_task_scheduler));

	zend_object_std_init(&scheduler->std, async_task_scheduler_ce);
	object_properties_init(&scheduler->std, async_task_scheduler_ce);

	scheduler->std.handlers = &async_task_scheduler_handlers;

	ASYNC_G(scheduler) = scheduler;

	return scheduler;
}

zend_bool async_task_scheduler_enqueue(async_task *task)
{
	async_task_scheduler *scheduler;

	zval obj;
	zval retval;

	scheduler = task->scheduler;

	ZEND_ASSERT(scheduler != NULL);

	if (task->fiber.status == ASYNC_FIBER_STATUS_INIT) {
		task->operation = ASYNC_TASK_OPERATION_START;

		GC_ADDREF(&task->fiber.std);
	} else if (task->fiber.status == ASYNC_FIBER_STATUS_SUSPENDED) {
		task->operation = ASYNC_TASK_OPERATION_RESUME;

		detach(&scheduler->suspended, task);
	} else {
		return 0;
	}

	scheduler->modified = 1;

	if (scheduler->loop && scheduler->activate && !scheduler->dispatching) {
		scheduler->activate = 0;
		scheduler->activating = 1;

		ZVAL_OBJ(&obj, &scheduler->std);
		GC_ADDREF(&scheduler->std);

		zval *entry = zend_hash_find_ex(&scheduler->std.ce->function_table, str_activate, 1);
		zend_function *func = Z_FUNC_P(entry);

		zend_call_method_with_0_params(&obj, Z_OBJCE_P(&obj), &func, "activate", &retval);
		zval_ptr_dtor(&obj);
		zval_ptr_dtor(&retval);

		scheduler->activating = 0;

		if (UNEXPECTED(EG(exception))) {
			scheduler->activate = 1;

			if (task->fiber.status == ASYNC_FIBER_STATUS_INIT) {
				ZVAL_OBJ(&task->result, EG(exception));
				EG(exception) = NULL;

				zend_clear_exception();

				zend_fcall_info_args_clear(&task->fiber.fci, 1);
				zval_ptr_dtor(&task->fiber.fci.function_name);

				task->operation = ASYNC_TASK_OPERATION_NONE;
				task->fiber.status = ASYNC_FIBER_STATUS_FAILED;

				async_awaitable_trigger_continuation(&task->continuation, &task->result, 0);
			} else if (task->fiber.status == ASYNC_FIBER_STATUS_SUSPENDED) {
				ZVAL_OBJ(&task->error, EG(exception));
				EG(exception) = NULL;

				zend_clear_exception();

				async_task_dispose(task);
			}

			OBJ_RELEASE(&task->fiber.std);

			return 0;
		}
	}

	enqueue(&scheduler->ready, task);

	return 1;
}

void async_task_scheduler_dequeue(async_task *task)
{
	async_task_scheduler *scheduler;

	scheduler = task->scheduler;

	ZEND_ASSERT(scheduler != NULL);

	if (task->fiber.status == ASYNC_FIBER_STATUS_SUSPENDED) {
		detach(&scheduler->suspended, task);
	} else {
		detach(&scheduler->ready, task);
	}
}

static void async_task_scheduler_dispatch(async_task_scheduler *scheduler)
{
	async_task *task;

	ZEND_ASSERT(scheduler != NULL);

	scheduler->dispatching = 1;

	while (scheduler->ready.size > 0) {
		task = dequeue(&scheduler->ready);

		ZEND_ASSERT(task->operation != ASYNC_TASK_OPERATION_NONE);

		if (task->operation == ASYNC_TASK_OPERATION_START) {
			async_task_start(task);
		} else {
			async_task_continue(task);
		}

		if (UNEXPECTED(EG(exception))) {
			ZVAL_OBJ(&task->result, EG(exception));
			EG(exception) = NULL;

			task->fiber.status = ASYNC_FIBER_STATUS_FAILED;
		}

		if (task->fiber.status == ASYNC_OP_RESOLVED || task->fiber.status == ASYNC_OP_FAILED) {
			async_task_dispose(task);

			OBJ_RELEASE(&task->fiber.std);
		} else if (task->fiber.status == ASYNC_FIBER_STATUS_SUSPENDED) {
			enqueue(&scheduler->suspended, task);
		}
	}

	scheduler->dispatching = 0;
}


void async_task_scheduler_run_loop(async_task_scheduler *scheduler)
{
	async_task_scheduler *prev;

	zend_function *func;
	zval *entry;
	zval obj;
	zval retval;

	ASYNC_CHECK_FATAL(scheduler->running, "Duplicate scheduler loop run detected");
	ASYNC_CHECK_FATAL(scheduler->dispatching, "Cannot run loop while dispatching");

	prev = ASYNC_G(current_scheduler);
	ASYNC_G(current_scheduler) = scheduler;

	scheduler->running = 1;

	if (scheduler->loop) {
		ZVAL_OBJ(&obj, &scheduler->std);
		GC_ADDREF(&scheduler->std);

		entry = zend_hash_find_ex(&scheduler->std.ce->function_table, str_runloop, 1);
		func = Z_FUNC_P(entry);

		zend_call_method_with_0_params(&obj, Z_OBJCE_P(&obj), &func, "runloop", &retval);
		zval_ptr_dtor(&obj);
		zval_ptr_dtor(&retval);

		if (UNEXPECTED(EG(exception))) {
			async_task_scheduler_dispose(scheduler);
		}
	} else {
		async_task_scheduler_dispatch(scheduler);
	}

	scheduler->running = 0;
	scheduler->modified = 0;

	ASYNC_G(current_scheduler) = prev;
}

void async_task_scheduler_stop_loop(async_task_scheduler *scheduler)
{
	zend_function *func;
	zval *entry;
	zval obj;
	zval retval;

	if (scheduler->loop) {
		ASYNC_CHECK_FATAL(scheduler->running == 0, "Cannot stop scheduler loop that is not running");

		ZVAL_OBJ(&obj, &scheduler->std);
		GC_ADDREF(&scheduler->std);

		entry = zend_hash_find_ex(&scheduler->std.ce->function_table, str_stoploop, 1);
		func = Z_FUNC_P(entry);

		zend_call_method_with_0_params(&obj, Z_OBJCE_P(&obj), &func, "stoploop", &retval);
		zval_ptr_dtor(&obj);
		zval_ptr_dtor(&retval);
	}
}

static zend_object *async_task_scheduler_object_create(zend_class_entry *ce)
{
	async_task_scheduler *scheduler;
	zend_ulong size;

	size = sizeof(async_task_scheduler) + zend_object_properties_size(ce);

	scheduler = emalloc(size);
	ZEND_SECURE_ZERO(scheduler, size);

	scheduler->activate = 1;
	scheduler->modified = 1;

	zend_object_std_init(&scheduler->std, ce);
	object_properties_init(&scheduler->std, ce);

	scheduler->std.handlers = &async_task_scheduler_handlers;

	return &scheduler->std;
}

static void async_task_scheduler_object_destroy(zend_object *object)
{
	async_task_scheduler *scheduler;

	scheduler = async_task_scheduler_obj(object);

	async_task_scheduler_dispose(scheduler);

	zend_object_std_dtor(object);
}

ZEND_METHOD(TaskScheduler, count)
{
	async_task_scheduler *scheduler;

	ZEND_PARSE_PARAMETERS_NONE();

	scheduler = async_task_scheduler_obj(Z_OBJ_P(getThis()));

	RETURN_LONG(scheduler->ready.size);
}

ZEND_METHOD(TaskScheduler, run)
{
	async_task_scheduler *scheduler;
	async_task *task;
	zend_uchar status;

	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	uint32_t count;

	zval *params;
	zval retval;

	scheduler = async_task_scheduler_obj(Z_OBJ_P(getThis()));

	ASYNC_CHECK_ERROR(scheduler->running, "Scheduler is already running");

	params = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, -1)
		Z_PARAM_FUNC_EX(fci, fcc, 1, 0)
		Z_PARAM_OPTIONAL
		Z_PARAM_VARIADIC('+', params, count)
	ZEND_PARSE_PARAMETERS_END();

	fci.no_separation = 1;

	if (count == 0) {
		fci.param_count = 0;
	} else {
		zend_fcall_info_argp(&fci, count, params);
	}

	Z_TRY_ADDREF_P(&fci.function_name);

	task = async_task_object_create(scheduler, async_context_get());
	task->fiber.fci = fci;
	task->fiber.fcc = fcc;

	async_task_scheduler_enqueue(task);
	async_task_scheduler_dispose(scheduler);

	status = task->fiber.status;
	ZVAL_COPY(&retval, &task->result);

	OBJ_RELEASE(&task->fiber.std);

	if (status == ASYNC_FIBER_STATUS_FINISHED) {
		RETURN_ZVAL(&retval, 1, 1);
	}

	if (status == ASYNC_FIBER_STATUS_FAILED) {
		execute_data->opline--;
		zend_throw_exception_internal(&retval);
		execute_data->opline++;
		return;
	}

	zval_ptr_dtor(&retval);

	if (EXPECTED(EG(exception) == NULL)) {
		zend_throw_error(NULL, "Awaitable has not been resolved");
	}
}

ZEND_METHOD(TaskScheduler, runWithContext)
{
	async_task_scheduler *scheduler;
	async_task *task;
	zend_uchar status;

	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	uint32_t count;

	zval *ctx;
	zval *params;
	zval retval;

	scheduler = async_task_scheduler_obj(Z_OBJ_P(getThis()));

	ASYNC_CHECK_ERROR(scheduler->running, "Scheduler is already running");

	params = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 2, -1)
		Z_PARAM_ZVAL_DEREF(ctx)
		Z_PARAM_FUNC_EX(fci, fcc, 1, 0)
		Z_PARAM_OPTIONAL
		Z_PARAM_VARIADIC('+', params, count)
	ZEND_PARSE_PARAMETERS_END();

	fci.no_separation = 1;

	if (count == 0) {
		fci.param_count = 0;
	} else {
		zend_fcall_info_argp(&fci, count, params);
	}

	Z_TRY_ADDREF_P(&fci.function_name);

	task = async_task_object_create(scheduler, (async_context *) Z_OBJ_P(ctx));
	task->fiber.fci = fci;
	task->fiber.fcc = fcc;

	async_task_scheduler_enqueue(task);
	async_task_scheduler_dispose(scheduler);

	status = task->fiber.status;
	ZVAL_COPY(&retval, &task->result);

	OBJ_RELEASE(&task->fiber.std);

	if (status == ASYNC_FIBER_STATUS_FINISHED) {
		RETURN_ZVAL(&retval, 1, 1);
	}

	if (status == ASYNC_FIBER_STATUS_FAILED) {
		execute_data->opline--;
		zend_throw_exception_internal(&retval);
		execute_data->opline++;
		return;
	}

	zval_ptr_dtor(&retval);

	if (EXPECTED(EG(exception) == NULL)) {
		zend_throw_error(NULL, "Awaitable has not been resolved");
	}
}

ZEND_METHOD(TaskScheduler, push)
{
	async_task_scheduler *scheduler;
	async_task_scheduler_stack *stack;
	async_task_scheduler_stack_entry *entry;

	zval *val;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();

	scheduler = async_task_scheduler_obj(Z_OBJ_P(val));
	stack = ASYNC_G(scheduler_stack);

	if (stack == NULL) {
		stack = emalloc(sizeof(async_task_scheduler_stack));
		stack->size = 0;
		stack->top = NULL;

		ASYNC_G(scheduler_stack) = stack;
	}

	entry = emalloc(sizeof(async_task_scheduler_stack_entry));
	entry->scheduler = scheduler;
	entry->prev = stack->top;

	stack->top = entry;
	stack->size++;

	GC_ADDREF(&scheduler->std);
}

ZEND_METHOD(TaskScheduler, pop)
{
	async_task_scheduler *scheduler;
	async_task_scheduler_stack *stack;
	async_task_scheduler_stack_entry *entry;

	zval *val;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();

	stack = ASYNC_G(scheduler_stack);

	ASYNC_CHECK_ERROR(stack == NULL || stack->top == NULL, "Cannot pop task scheduler that has not been pushed");

	scheduler = async_task_scheduler_obj(Z_OBJ_P(val));

	ASYNC_CHECK_ERROR(scheduler != stack->top->scheduler, "Cannot pop task scheduler because it is not the active scheduler");

	entry = stack->top;
	stack->top = entry->prev;
	stack->size--;

	entry->scheduler->modified = 1;

	async_task_scheduler_dispose(entry->scheduler);

	OBJ_RELEASE(&entry->scheduler->std);

	efree(entry);
}

ZEND_METHOD(TaskScheduler, __wakeup)
{
	ZEND_PARSE_PARAMETERS_NONE();

	zend_throw_error(NULL, "Unserialization of a task scheduler is not allowed");
}

ZEND_BEGIN_ARG_INFO(arginfo_task_scheduler_count, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_task_scheduler_run, 0, 0, 1)
	ZEND_ARG_CALLABLE_INFO(0, callback, 0)
	ZEND_ARG_VARIADIC_INFO(0, arguments)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_task_scheduler_run_with_context, 0, 0, 2)
	ZEND_ARG_OBJ_INFO(0, context, Concurrent\\Context, 0)
	ZEND_ARG_CALLABLE_INFO(0, callback, 0)
	ZEND_ARG_VARIADIC_INFO(0, arguments)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_task_scheduler_push, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, scheduler, Concurrent\\TaskScheduler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_task_scheduler_pop, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, scheduler, Concurrent\\TaskScheduler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_task_scheduler_wakeup, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry task_scheduler_functions[] = {
	ZEND_ME(TaskScheduler, count, arginfo_task_scheduler_count, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
	ZEND_ME(TaskScheduler, run, arginfo_task_scheduler_run, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
	ZEND_ME(TaskScheduler, runWithContext, arginfo_task_scheduler_run_with_context, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
	ZEND_ME(TaskScheduler, push, arginfo_task_scheduler_push, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC | ZEND_ACC_FINAL)
	ZEND_ME(TaskScheduler, pop, arginfo_task_scheduler_pop, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC | ZEND_ACC_FINAL)
	ZEND_ME(TaskScheduler, __wakeup, arginfo_task_scheduler_wakeup, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
	ZEND_FE_END
};


static zend_object *async_loop_task_scheduler_object_create(zend_class_entry *ce)
{
	async_task_scheduler *scheduler;
	zend_ulong size;

	size = sizeof(async_task_scheduler) + zend_object_properties_size(ce);

	scheduler = emalloc(size);
	ZEND_SECURE_ZERO(scheduler, size);

	scheduler->loop = 1;
	scheduler->activate = 1;
	scheduler->modified = 1;

	zend_object_std_init(&scheduler->std, ce);
	object_properties_init(&scheduler->std, ce);

	scheduler->std.handlers = &async_loop_task_scheduler_handlers;

	return &scheduler->std;
}

ZEND_METHOD(LoopTaskScheduler, activate) { }

ZEND_METHOD(LoopTaskScheduler, runLoop) { }

ZEND_METHOD(LoopTaskScheduler, stopLoop) { }

ZEND_METHOD(LoopTaskScheduler, dispatch)
{
	async_task_scheduler *scheduler;
	async_task_scheduler *prev;

	// Left out on purpose to allow for simplified event-loop integration.
	// ZEND_PARSE_PARAMETERS_NONE();

	scheduler = async_task_scheduler_obj(Z_OBJ_P(getThis()));

	ASYNC_CHECK_ERROR(scheduler->dispatching, "Cannot dispatch tasks because the dispatcher is already running");
	ASYNC_CHECK_ERROR(scheduler->running == 0, "Cannot dispatch tasks while the task scheduler is not running");
	ASYNC_CHECK_ERROR(scheduler->activating, "Cannot dispatch in activator, use your event loop instead");

	prev = ASYNC_G(current_scheduler);
	ASYNC_G(current_scheduler) = scheduler;

	async_task_scheduler_dispatch(scheduler);

	ASYNC_G(current_scheduler) = prev;

	scheduler->activate = 1;
}

ZEND_BEGIN_ARG_INFO(arginfo_loop_task_scheduler_activate, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_loop_task_scheduler_run_loop, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_loop_task_scheduler_stop_loop, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_loop_task_scheduler_dispatch, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry loop_task_scheduler_functions[] = {
	ZEND_ME(LoopTaskScheduler, activate, arginfo_loop_task_scheduler_activate, ZEND_ACC_PROTECTED | ZEND_ACC_ABSTRACT)
	ZEND_ME(LoopTaskScheduler, runLoop, arginfo_loop_task_scheduler_run_loop, ZEND_ACC_PROTECTED | ZEND_ACC_ABSTRACT)
	ZEND_ME(LoopTaskScheduler, stopLoop, arginfo_loop_task_scheduler_stop_loop, ZEND_ACC_PROTECTED | ZEND_ACC_ABSTRACT)
	ZEND_ME(LoopTaskScheduler, dispatch, arginfo_loop_task_scheduler_dispatch, ZEND_ACC_PROTECTED | ZEND_ACC_FINAL)
	ZEND_FE_END
};

void async_task_scheduler_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\TaskScheduler", task_scheduler_functions);
	async_task_scheduler_ce = zend_register_internal_class(&ce);
	async_task_scheduler_ce->create_object = async_task_scheduler_object_create;
	async_task_scheduler_ce->serialize = zend_class_serialize_deny;
	async_task_scheduler_ce->unserialize = zend_class_unserialize_deny;

	zend_class_implements(async_task_scheduler_ce, 1, zend_ce_countable);

	memcpy(&async_task_scheduler_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_task_scheduler_handlers.offset = XtOffsetOf(async_task_scheduler, std);
	async_task_scheduler_handlers.free_obj = async_task_scheduler_object_destroy;
	async_task_scheduler_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Concurrent\\LoopTaskScheduler", loop_task_scheduler_functions);
	async_loop_task_scheduler_ce = zend_register_internal_class_ex(&ce, async_task_scheduler_ce);
	async_loop_task_scheduler_ce->ce_flags |= ZEND_ACC_ABSTRACT;
	async_loop_task_scheduler_ce->create_object = async_loop_task_scheduler_object_create;
	async_loop_task_scheduler_ce->serialize = zend_class_serialize_deny;
	async_loop_task_scheduler_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_loop_task_scheduler_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_loop_task_scheduler_handlers.offset = XtOffsetOf(async_task_scheduler, std);
	async_loop_task_scheduler_handlers.free_obj = async_task_scheduler_object_destroy;
	async_loop_task_scheduler_handlers.clone_obj = NULL;

	str_activate = zend_string_init("activate", sizeof("activate")-1, 1);
	str_runloop = zend_string_init("runloop", sizeof("runloop")-1, 1);
	str_stoploop = zend_string_init("stoploop", sizeof("stoploop")-1, 1);

	ZSTR_HASH(str_activate);
	ZSTR_HASH(str_runloop);
	ZSTR_HASH(str_stoploop);
}

void async_task_scheduler_ce_unregister()
{
	zend_string_free(str_activate);
	zend_string_free(str_runloop);
	zend_string_free(str_stoploop);
}

void async_task_scheduler_shutdown()
{
	async_task_scheduler *scheduler;
	async_task_scheduler_stack *stack;
	async_task_scheduler_stack_entry *entry;

	stack = ASYNC_G(scheduler_stack);

	if (stack != NULL) {
		ASYNC_G(scheduler_stack) = NULL;

		while (stack->top != NULL) {
			entry = stack->top;

			stack->top = entry->prev;
			stack->size--;

			entry->scheduler->modified = 1;

			async_task_scheduler_dispose(entry->scheduler);

			OBJ_RELEASE(&entry->scheduler->std);

			efree(entry);
		}

		efree(stack);
	}

	scheduler = ASYNC_G(scheduler);

	if (scheduler != NULL) {
		ASYNC_G(scheduler) = NULL;

		scheduler->modified = 1;

		async_task_scheduler_dispose(scheduler);

		OBJ_RELEASE(&scheduler->std);
	}
}


/*
 * vim: sw=4 ts=4
 * vim600: fdm=marker
 */
