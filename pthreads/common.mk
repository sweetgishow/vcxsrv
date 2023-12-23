# Common makefile definitions
!if "$(IS64)" == "1"
SUFFIX=64
!else
SUFFIX=
!endif

!ifdef DEBUG
OUTDIR=obj$(SUFFIX)_d
!else
OUTDIR=obj$(SUFFIX)
!endif

RESOURCE_OBJS	= \
		version.$(RESEXT)

# pthread.c aggregates all source into a single compilation unit for inlinability
DLL_OBJS	= \
		$(OUTDIR)\pthread.$(OBJEXT)

STATIC_OBJS	= \
		$(OUTDIR)\pthread.$(OEXT)

# Separate modules for minimising the size of statically linked images
STATIC_OBJS_SMALL	= \
		cleanup.$(OBJEXT) \
		create.$(OBJEXT) \
		dll.$(OBJEXT) \
		errno.$(OBJEXT) \
		global.$(OBJEXT) \
		pthread_attr_destroy.$(OBJEXT) \
		pthread_attr_getaffinity_np.$(OBJEXT) \
		pthread_attr_getdetachstate.$(OBJEXT) \
		pthread_attr_getinheritsched.$(OBJEXT) \
		pthread_attr_getname_np.$(OBJEXT) \
		pthread_attr_getschedparam.$(OBJEXT) \
		pthread_attr_getschedpolicy.$(OBJEXT) \
		pthread_attr_getscope.$(OBJEXT) \
		pthread_attr_getstackaddr.$(OBJEXT) \
		pthread_attr_getstacksize.$(OBJEXT) \
		pthread_attr_init.$(OBJEXT) \
		pthread_attr_setaffinity_np.$(OBJEXT) \
		pthread_attr_setdetachstate.$(OBJEXT) \
		pthread_attr_setinheritsched.$(OBJEXT) \
		pthread_attr_setname_np.$(OBJEXT) \
		pthread_attr_setschedparam.$(OBJEXT) \
		pthread_attr_setschedpolicy.$(OBJEXT) \
		pthread_attr_setscope.$(OBJEXT) \
		pthread_attr_setstackaddr.$(OBJEXT) \
		pthread_attr_setstacksize.$(OBJEXT) \
		pthread_barrier_destroy.$(OBJEXT) \
		pthread_barrier_init.$(OBJEXT) \
		pthread_barrier_wait.$(OBJEXT) \
		pthread_barrierattr_destroy.$(OBJEXT) \
		pthread_barrierattr_getpshared.$(OBJEXT) \
		pthread_barrierattr_init.$(OBJEXT) \
		pthread_barrierattr_setpshared.$(OBJEXT) \
		pthread_cancel.$(OBJEXT) \
		pthread_cond_destroy.$(OBJEXT) \
		pthread_cond_init.$(OBJEXT) \
		pthread_cond_signal.$(OBJEXT) \
		pthread_cond_wait.$(OBJEXT) \
		pthread_condattr_destroy.$(OBJEXT) \
		pthread_condattr_getpshared.$(OBJEXT) \
		pthread_condattr_init.$(OBJEXT) \
		pthread_condattr_setpshared.$(OBJEXT) \
		pthread_delay_np.$(OBJEXT) \
		pthread_detach.$(OBJEXT) \
		pthread_equal.$(OBJEXT) \
		pthread_exit.$(OBJEXT) \
		pthread_getconcurrency.$(OBJEXT) \
		pthread_getname_np.$(OBJEXT) \
		pthread_getschedparam.$(OBJEXT) \
		pthread_getspecific.$(OBJEXT) \
		pthread_getunique_np.$(OBJEXT) \
		pthread_getw32threadhandle_np.$(OBJEXT) \
		pthread_join.$(OBJEXT) \
		pthread_timedjoin_np.$(OBJEXT) \
		pthread_tryjoin_np.$(OBJEXT) \
		pthread_key_create.$(OBJEXT) \
		pthread_key_delete.$(OBJEXT) \
		pthread_kill.$(OBJEXT) \
		pthread_mutex_consistent.$(OBJEXT) \
		pthread_mutex_destroy.$(OBJEXT) \
		pthread_mutex_init.$(OBJEXT) \
		pthread_mutex_lock.$(OBJEXT) \
		pthread_mutex_timedlock.$(OBJEXT) \
		pthread_mutex_trylock.$(OBJEXT) \
		pthread_mutex_unlock.$(OBJEXT) \
		pthread_mutexattr_destroy.$(OBJEXT) \
		pthread_mutexattr_getkind_np.$(OBJEXT) \
		pthread_mutexattr_getpshared.$(OBJEXT) \
		pthread_mutexattr_getrobust.$(OBJEXT) \
		pthread_mutexattr_gettype.$(OBJEXT) \
		pthread_mutexattr_init.$(OBJEXT) \
		pthread_mutexattr_setkind_np.$(OBJEXT) \
		pthread_mutexattr_setpshared.$(OBJEXT) \
		pthread_mutexattr_setrobust.$(OBJEXT) \
		pthread_mutexattr_settype.$(OBJEXT) \
		pthread_num_processors_np.$(OBJEXT) \
		pthread_once.$(OBJEXT) \
		pthread_rwlock_destroy.$(OBJEXT) \
		pthread_rwlock_init.$(OBJEXT) \
		pthread_rwlock_rdlock.$(OBJEXT) \
		pthread_rwlock_timedrdlock.$(OBJEXT) \
		pthread_rwlock_timedwrlock.$(OBJEXT) \
		pthread_rwlock_tryrdlock.$(OBJEXT) \
		pthread_rwlock_trywrlock.$(OBJEXT) \
		pthread_rwlock_unlock.$(OBJEXT) \
		pthread_rwlock_wrlock.$(OBJEXT) \
		pthread_rwlockattr_destroy.$(OBJEXT) \
		pthread_rwlockattr_getpshared.$(OBJEXT) \
		pthread_rwlockattr_init.$(OBJEXT) \
		pthread_rwlockattr_setpshared.$(OBJEXT) \
		pthread_self.$(OBJEXT) \
		pthread_setaffinity.$(OBJEXT) \
		pthread_setcancelstate.$(OBJEXT) \
		pthread_setcanceltype.$(OBJEXT) \
		pthread_setconcurrency.$(OBJEXT) \
		pthread_setname_np.$(OBJEXT) \
		pthread_setschedparam.$(OBJEXT) \
		pthread_setspecific.$(OBJEXT) \
		pthread_spin_destroy.$(OBJEXT) \
		pthread_spin_init.$(OBJEXT) \
		pthread_spin_lock.$(OBJEXT) \
		pthread_spin_trylock.$(OBJEXT) \
		pthread_spin_unlock.$(OBJEXT) \
		pthread_testcancel.$(OBJEXT) \
		pthread_timechange_handler_np.$(OBJEXT) \
		pthread_win32_attach_detach_np.$(OBJEXT) \
		ptw32_MCS_lock.$(OBJEXT) \
		ptw32_callUserDestroyRoutines.$(OBJEXT) \
		ptw32_calloc.$(OBJEXT) \
		ptw32_cond_check_need_init.$(OBJEXT) \
		ptw32_getprocessors.$(OBJEXT) \
		ptw32_is_attr.$(OBJEXT) \
		ptw32_mutex_check_need_init.$(OBJEXT) \
		ptw32_new.$(OBJEXT) \
		ptw32_processInitialize.$(OBJEXT) \
		ptw32_processTerminate.$(OBJEXT) \
		ptw32_relmillisecs.$(OBJEXT) \
		ptw32_reuse.$(OBJEXT) \
		ptw32_rwlock_cancelwrwait.$(OBJEXT) \
		ptw32_rwlock_check_need_init.$(OBJEXT) \
		ptw32_semwait.$(OBJEXT) \
		ptw32_spinlock_check_need_init.$(OBJEXT) \
		ptw32_threadDestroy.$(OBJEXT) \
		ptw32_threadStart.$(OBJEXT) \
		ptw32_throw.$(OBJEXT) \
		ptw32_timespec.$(OBJEXT) \
		ptw32_tkAssocCreate.$(OBJEXT) \
		ptw32_tkAssocDestroy.$(OBJEXT) \
		sched_get_priority_max.$(OBJEXT) \
		sched_get_priority_min.$(OBJEXT) \
		sched_getscheduler.$(OBJEXT) \
		sched_setaffinity.$(OBJEXT) \
		sched_setscheduler.$(OBJEXT) \
		sched_yield.$(OBJEXT) \
		sem_close.$(OBJEXT) \
		sem_destroy.$(OBJEXT) \
		sem_getvalue.$(OBJEXT) \
		sem_init.$(OBJEXT) \
		sem_open.$(OBJEXT) \
		sem_post.$(OBJEXT) \
		sem_post_multiple.$(OBJEXT) \
		sem_timedwait.$(OBJEXT) \
		sem_trywait.$(OBJEXT) \
		sem_unlink.$(OBJEXT) \
		sem_wait.$(OBJEXT) \
		w32_CancelableWait.$(OBJEXT)

PTHREAD_SRCS	= \
		ptw32_MCS_lock.c \
		ptw32_is_attr.c \
		ptw32_processInitialize.c \
		ptw32_processTerminate.c \
		ptw32_threadStart.c \
		ptw32_threadDestroy.c \
		ptw32_tkAssocCreate.c \
		ptw32_tkAssocDestroy.c \
		ptw32_callUserDestroyRoutines.c \
		ptw32_semwait.c \
		ptw32_timespec.c \
		ptw32_throw.c \
		ptw32_getprocessors.c \
		ptw32_calloc.c \
		ptw32_new.c \
		ptw32_reuse.c \
		ptw32_relmillisecs.c \
		ptw32_cond_check_need_init.c \
		ptw32_mutex_check_need_init.c \
		ptw32_rwlock_check_need_init.c \
		ptw32_rwlock_cancelwrwait.c \
		ptw32_spinlock_check_need_init.c \
		pthread_attr_init.c \
		pthread_attr_destroy.c \
		pthread_attr_getaffinity_np.c \
		pthread_attr_setaffinity_np.c \
		pthread_attr_getdetachstate.c \
		pthread_attr_setdetachstate.c \
		pthread_attr_getname_np.c \
		pthread_attr_setname_np.c \
		pthread_attr_getscope.c \
		pthread_attr_setscope.c \
		pthread_attr_getstackaddr.c \
		pthread_attr_setstackaddr.c \
		pthread_attr_getstacksize.c \
		pthread_attr_setstacksize.c \
		pthread_barrier_init.c \
		pthread_barrier_destroy.c \
		pthread_barrier_wait.c \
		pthread_barrierattr_init.c \
		pthread_barrierattr_destroy.c \
		pthread_barrierattr_setpshared.c \
		pthread_barrierattr_getpshared.c \
		pthread_setcancelstate.c \
		pthread_setcanceltype.c \
		pthread_testcancel.c \
		pthread_cancel.c \
		pthread_condattr_destroy.c \
		pthread_condattr_getpshared.c \
		pthread_condattr_init.c \
		pthread_condattr_setpshared.c \
		pthread_cond_destroy.c \
		pthread_cond_init.c \
		pthread_cond_signal.c \
		pthread_cond_wait.c \
		create.c \
		cleanup.c \
		dll.c \
		errno.c \
		pthread_exit.c \
		global.c \
		pthread_equal.c \
		pthread_getconcurrency.c \
		pthread_kill.c \
		pthread_once.c \
		pthread_self.c \
		pthread_setconcurrency.c \
		w32_CancelableWait.c \
		pthread_mutex_init.c \
		pthread_mutex_destroy.c \
		pthread_mutexattr_init.c \
		pthread_mutexattr_destroy.c \
		pthread_mutexattr_getpshared.c \
		pthread_mutexattr_setpshared.c \
		pthread_mutexattr_settype.c \
		pthread_mutexattr_gettype.c \
		pthread_mutexattr_setrobust.c \
		pthread_mutexattr_getrobust.c \
		pthread_mutex_lock.c \
		pthread_mutex_timedlock.c \
		pthread_mutex_unlock.c \
		pthread_mutex_trylock.c \
		pthread_mutex_consistent.c \
		pthread_mutexattr_setkind_np.c \
		pthread_mutexattr_getkind_np.c \
		pthread_getw32threadhandle_np.c \
		pthread_getunique_np.c \
		pthread_setaffinity.c \
		pthread_delay_np.c \
		pthread_num_processors_np.c \
		pthread_win32_attach_detach_np.c \
		pthread_timechange_handler_np.c \
		pthread_rwlock_init.c \
		pthread_rwlock_destroy.c \
		pthread_rwlockattr_init.c \
		pthread_rwlockattr_destroy.c \
		pthread_rwlockattr_getpshared.c \
		pthread_rwlockattr_setpshared.c \
		pthread_rwlock_rdlock.c \
		pthread_rwlock_timedrdlock.c \
		pthread_rwlock_wrlock.c \
		pthread_rwlock_timedwrlock.c \
		pthread_rwlock_unlock.c \
		pthread_rwlock_tryrdlock.c \
		pthread_rwlock_trywrlock.c \
		pthread_attr_setschedpolicy.c \
		pthread_attr_getschedpolicy.c \
		pthread_attr_setschedparam.c \
		pthread_attr_getschedparam.c \
		pthread_attr_setinheritsched.c \
		pthread_attr_getinheritsched.c \
		pthread_getname_np.c \
		pthread_setname_np.c \
		pthread_setschedparam.c \
		pthread_getschedparam.c \
		sched_get_priority_max.c \
		sched_get_priority_min.c \
		sched_setscheduler.c \
		sched_getscheduler.c \
		sched_yield.c \
		sched_setaffinity.c \
		sem_init.c \
		sem_destroy.c \
		sem_trywait.c \
		sem_timedwait.c \
		sem_wait.c \
		sem_post.c \
		sem_post_multiple.c \
		sem_getvalue.c \
		sem_open.c \
		sem_close.c \
		sem_unlink.c \
		pthread_spin_init.c \
		pthread_spin_destroy.c \
		pthread_spin_lock.c \
		pthread_spin_unlock.c \
		pthread_spin_trylock.c \
		pthread_detach.c \
		pthread_join.c \
		pthread_timedjoin_np.c \
		pthread_tryjoin_np.c \
		pthread_key_create.c \
		pthread_key_delete.c \
		pthread_setspecific.c \
		pthread_getspecific.c

INCL		= \
		config.h \
		implement.h \
		need_errno.h \
		pthread.h \
		semaphore.h \
		need_errno.h

# Output useful info if no target given. I.e. the first target that "make" sees is used in this case.
default_target: help

pthread.$(OBJEXT): pthread.c $(PTHREAD_SRCS)

# end common.mk
