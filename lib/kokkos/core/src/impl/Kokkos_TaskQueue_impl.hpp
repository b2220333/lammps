/*
//@HEADER
// ************************************************************************
// 
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
// 
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact  H. Carter Edwards (hcedwar@sandia.gov)
// 
// ************************************************************************
//@HEADER
*/

#if defined( KOKKOS_ENABLE_TASKPOLICY )

namespace Kokkos {
namespace Impl {

//----------------------------------------------------------------------------

template< typename ExecSpace >
void TaskQueue< ExecSpace >::Destroy::destroy_shared_allocation()
{
  m_queue->~TaskQueue();
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
TaskQueue< ExecSpace >::TaskQueue
  ( const TaskQueue< ExecSpace >::memory_space & arg_space
  , unsigned const arg_memory_pool_capacity
  , unsigned const arg_memory_pool_superblock_capacity_log2
  )
  : m_memory( arg_space
            , arg_memory_pool_capacity
            , arg_memory_pool_superblock_capacity_log2 )
  , m_ready()
  , m_accum_alloc(0)
  , m_max_alloc(0)
  , m_ready_count(0)
{
  for ( int i = 0 ; i < NumQueue ; ++i ) {
    m_ready[i][0] = (task_root_type *) task_root_type::EndTag ;
    m_ready[i][1] = (task_root_type *) task_root_type::EndTag ;
  }
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
TaskQueue< ExecSpace >::~TaskQueue()
{
  // Verify that queues are empty and ready count is zero

  for ( int i = 0 ; i < NumQueue ; ++i ) {
    for ( int j = 0 ; j < 2 ; ++j ) {
      if ( m_ready[i][j] != (task_root_type *) task_root_type::EndTag ) {
        Kokkos::abort("TaskQueue::~TaskQueue ERROR: has ready tasks");
      }
    }
  }

  if ( 0 != m_ready_count ) {
    Kokkos::abort("TaskQueue::~TaskQueue ERROR: has ready or executing tasks");
  }
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
void TaskQueue< ExecSpace >::decrement
  ( TaskQueue< ExecSpace >::task_root_type * task )
{
  const int count = Kokkos::atomic_fetch_add(&(task->m_ref_count),-1);

#if 0
  if ( 1 == count ) {
    printf( "decrement-destroy( 0x%lx { 0x%lx %d %d } )\n"
          , uintptr_t( task )
          , uintptr_t( task->m_next )
          , int( task->m_task_type )
          , int( task->m_ref_count )
          );
  }
#endif

  if ( ( 1 == count ) && 
       ( task->m_next == (task_root_type *) task_root_type::LockTag ) ) {
    // Reference count is zero and task is complete, deallocate.
    task->m_queue->deallocate( task , task->m_alloc_size );
  }   
  else if ( count <= 1 ) { 
    Kokkos::abort("TaskPolicy task has negative reference count or is incomplete" );
  }   
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
size_t TaskQueue< ExecSpace >::allocate_block_size( size_t n )
{
  return m_memory.allocate_block_size( n );
}

template< typename ExecSpace >
KOKKOS_FUNCTION
void * TaskQueue< ExecSpace >::allocate( size_t n )
{
  void * const p = m_memory.allocate(n);

  if ( p ) {
    Kokkos::atomic_increment( & m_accum_alloc );
    Kokkos::atomic_increment( & m_count_alloc );

    if ( m_max_alloc < m_count_alloc ) m_max_alloc = m_count_alloc ;
  }

  return p ;
}

template< typename ExecSpace >
KOKKOS_FUNCTION
void TaskQueue< ExecSpace >::deallocate( void * p , size_t n )
{
  m_memory.deallocate( p , n );
  Kokkos::atomic_decrement( & m_count_alloc );
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
bool TaskQueue< ExecSpace >::push_task
  ( TaskQueue< ExecSpace >::task_root_type * volatile * const queue
  , TaskQueue< ExecSpace >::task_root_type * const task
  )
{
  // Push task into a concurrently pushed and popped queue.
  // The queue is a linked list where 'task->m_next' form the links.
  // Fail the push attempt if the queue is locked;
  // otherwise retry until the push succeeds.

#if 0
  printf( "push_task( 0x%lx { 0x%lx } 0x%lx { 0x%lx 0x%lx %d %d %d } )\n"
        , uintptr_t(queue)
        , uintptr_t(*queue)
        , uintptr_t(task)
        , uintptr_t(task->m_wait)
        , uintptr_t(task->m_next)
        , task->m_task_type
        , task->m_priority
        , task->m_ref_count );
#endif

  task_root_type * const zero = (task_root_type *) 0 ;
  task_root_type * const lock = (task_root_type *) task_root_type::LockTag ;

  task_root_type * volatile * const next = & task->m_next ;

  if ( zero != *next ) {
    Kokkos::abort("TaskQueue::push_task ERROR: already a member of another queue" );
  }

  task_root_type * y = *queue ;

  while ( lock != y ) {

    *next = y ;

    // Do not proceed until '*next' has been stored.
    Kokkos::memory_fence();

    task_root_type * const x = y ;

    y = Kokkos::atomic_compare_exchange(queue,y,task);

    if ( x == y ) return true ;
  }

  // Failed, replace 'task->m_next' value since 'task' remains
  // not a member of a queue.

  *next = zero ;

  // Do not proceed until '*next' has been stored.
  Kokkos::memory_fence();

  return false ;
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
typename TaskQueue< ExecSpace >::task_root_type *
TaskQueue< ExecSpace >::pop_task
  ( TaskQueue< ExecSpace >::task_root_type * volatile * const queue )
{
  // Pop task from a concurrently pushed and popped queue.
  // The queue is a linked list where 'task->m_next' form the links.

  task_root_type * const zero = (task_root_type *) 0 ;
  task_root_type * const lock = (task_root_type *) task_root_type::LockTag ;
  task_root_type * const end  = (task_root_type *) task_root_type::EndTag ;

  // *queue is
  //   end   => an empty queue
  //   lock  => a locked queue
  //   valid

  // Retry until the lock is acquired or the queue is empty.

  task_root_type * task = *queue ;

  while ( end != task ) {

    // The only possible values for the queue are
    // (1) lock, (2) end, or (3) a valid task.
    // Thus zero will never appear in the queue.
    //
    // If queue is locked then just read by guaranteeing
    // the CAS will fail.

    if ( lock == task ) task = 0 ;

    task_root_type * const x = task ;

    task = Kokkos::atomic_compare_exchange(queue,task,lock);

    if ( x == task ) break ; // CAS succeeded and queue is locked
  }

  if ( end != task ) {

    // This thread has locked the queue and removed 'task' from the queue.
    // Extract the next entry of the queue from 'task->m_next'
    // and mark 'task' as popped from a queue by setting
    // 'task->m_next = lock'.

    task_root_type * const next =
      Kokkos::atomic_exchange( & task->m_next , lock );

    // Place the next entry in the head of the queue,
    // which also unlocks the queue.

    task_root_type * const unlock =
      Kokkos::atomic_exchange( queue , next );

    if ( next == zero || next == lock || lock != unlock ) {
      Kokkos::abort("TaskQueue::pop_task ERROR");
    }
  }

#if 0
  if ( end != task ) {
    printf( "pop_task( 0x%lx 0x%lx { 0x%lx 0x%lx %d %d %d } )\n"
          , uintptr_t(queue)
          , uintptr_t(task)
          , uintptr_t(task->m_wait)
          , uintptr_t(task->m_next)
          , int(task->m_task_type)
          , int(task->m_priority)
          , int(task->m_ref_count) );
  }
#endif

  return task ;
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
void TaskQueue< ExecSpace >::schedule
  ( TaskQueue< ExecSpace >::task_root_type * const task )
{
  // Schedule a runnable or when_all task upon construction / spawn
  // and upon completion of other tasks that 'task' is waiting on.

  // Precondition on runnable task state:
  //   task is either constructing or executing
  //
  //   Constructing state:
  //     task->m_wait == 0
  //     task->m_next == dependence
  //   Executing-respawn state:
  //     task->m_wait == head of linked list
  //     task->m_next == dependence
  //
  //  Task state transition:
  //     Constructing      ->  Waiting
  //     Executing-respawn ->  Waiting
  //
  //  Postcondition on task state:
  //     task->m_wait == head of linked list
  //     task->m_next == member of linked list

#if 0
  printf( "schedule( 0x%lx { 0x%lx 0x%lx %d %d %d }\n"
        , uintptr_t(task)
        , uintptr_t(task->m_wait)
        , uintptr_t(task->m_next)
        , task->m_task_type
        , task->m_priority
        , task->m_ref_count );
#endif

  task_root_type * const zero = (task_root_type *) 0 ;
  task_root_type * const lock = (task_root_type *) task_root_type::LockTag ;
  task_root_type * const end  = (task_root_type *) task_root_type::EndTag ;

  //----------------------------------------
  {
    // If Constructing then task->m_wait == 0
    // Change to waiting by task->m_wait = EndTag

    task_root_type * const init =
      Kokkos::atomic_compare_exchange( & task->m_wait , zero , end );

    // Precondition

    if ( lock == init ) {
      Kokkos::abort("TaskQueue::schedule ERROR: task is complete");
    }

    // if ( init == 0 ) Constructing       ->  Waiting
    // else             Executing-Respawn  ->  Waiting
  }
  //----------------------------------------

  if ( task_root_type::Aggregate != task->m_task_type ) {

    // Scheduling a runnable task which may have a depencency 'dep'.
    // Extract dependence, if any, from task->m_next.
    // If 'dep' is not null then attempt to push 'task'
    // into the wait queue of 'dep'.
    // If the push succeeds then 'task' may be
    // processed or executed by another thread at any time.
    // If the push fails then 'dep' is complete and 'task'
    // is ready to execute.

    task_root_type * dep = Kokkos::atomic_exchange( & task->m_next , zero );

    const bool is_ready = 
      ( 0 == dep ) || ( ! push_task( & dep->m_wait , task ) );

    // Reference count for dep was incremented when assigned
    // to task->m_next so that if it completed prior to the
    // above push_task dep would not be destroyed.
    // dep reference count can now be decremented,
    // which may deallocate the task.
    TaskQueue::assign( & dep , (task_root_type *)0 );

    if ( is_ready ) {

      // No dependence or 'dep' is complete so push task into ready queue.
      // Increment the ready count before pushing into ready queue
      // to track number of ready + executing tasks.
      // The ready count will be decremented when the task is complete.

      Kokkos::atomic_increment( & m_ready_count );

      task_root_type * volatile * const queue =
        & m_ready[ task->m_priority ][ task->m_task_type ];

      // A push_task fails if the ready queue is locked.
      // A ready queue is only locked during a push or pop;
      // i.e., it is never permanently locked.
      // Retry push to ready queue until it succeeds.
      // When the push succeeds then 'task' may be
      // processed or executed by another thread at any time.

      while ( ! push_task( queue , task ) );
    }
  }
  //----------------------------------------
  else {
    // Scheduling a 'when_all' task with multiple dependences.
    // This scheduling may be called when the 'when_all' is
    // (1) created or
    // (2) being removed from a completed task's wait list.

    task_root_type ** const aggr = task->aggregate_dependences();

    // Assume the 'when_all' is complete until a dependence is
    // found that is not complete.

    bool is_complete = true ;

    for ( int i = task->m_dep_count ; 0 < i && is_complete ; ) {

      --i ;

      // Loop dependences looking for an incomplete task.
      // Add this task to the incomplete task's wait queue.

      // Remove a task 'x' from the dependence list.
      // The reference count of 'x' was incremented when
      // it was assigned into the dependence list.

      task_root_type * x = Kokkos::atomic_exchange( aggr + i , zero );

      if ( x ) {

        // If x->m_wait is not locked then push succeeds
        // and the aggregate is not complete.
        // If the push succeeds then this when_all 'task' may be
        // processed by another thread at any time.
        // For example, 'x' may be completeed by another
        // thread and then re-schedule this when_all 'task'.

        is_complete = ! push_task( & x->m_wait , task );

        // Decrement reference count which had been incremented
        // when 'x' was added to the dependence list.

        TaskQueue::assign( & x , zero );
      }
    }

    if ( is_complete ) {
      // The when_all 'task' was not added to a wait queue because
      // all dependences were complete so this aggregate is complete.
      // Complete the when_all 'task' to schedule other tasks
      // that are waiting for the when_all 'task' to complete.

      task->m_next = lock ;

      complete( task );

      // '*task' may have been deleted upon completion
    }
  }
  //----------------------------------------
  // Postcondition:
  //   A runnable 'task' was pushed into a wait or ready queue.
  //   An aggregate 'task' was either pushed to a wait queue
  //   or completed.
  // Concurrent execution may have already popped 'task'
  // from a queue and processed it as appropriate.
}

//----------------------------------------------------------------------------

template< typename ExecSpace >
KOKKOS_FUNCTION
void TaskQueue< ExecSpace >::complete
  ( TaskQueue< ExecSpace >::task_root_type * task )
{
  // Complete a runnable task that has finished executing
  // or a when_all task when all of its dependeneces are complete.

  task_root_type * const zero = (task_root_type *) 0 ;
  task_root_type * const lock = (task_root_type *) task_root_type::LockTag ;
  task_root_type * const end  = (task_root_type *) task_root_type::EndTag ;

#if 0
  printf( "complete( 0x%lx { 0x%lx 0x%lx %d %d %d }\n"
        , uintptr_t(task)
        , uintptr_t(task->m_wait)
        , uintptr_t(task->m_next)
        , task->m_task_type
        , task->m_priority
        , task->m_ref_count );
  fflush( stdout );
#endif

  const bool runnable = task_root_type::Aggregate != task->m_task_type ;

  //----------------------------------------

  if ( runnable && lock != task->m_next ) {
    // Is a runnable task has finished executing and requested respawn.
    // Schedule the task for subsequent execution.

    schedule( task );
  }
  //----------------------------------------
  else {
    // Is either an aggregate or a runnable task that executed
    // and did not respawn.  Transition this task to complete.

    // If 'task' is an aggregate then any of the runnable tasks that
    // it depends upon may be attempting to complete this 'task'.
    // Must only transition a task once to complete status.
    // This is controled by atomically locking the wait queue.

    // Stop other tasks from adding themselves to this task's wait queue
    // by locking the head of this task's wait queue.

    task_root_type * x = Kokkos::atomic_exchange( & task->m_wait , lock );

    if ( x != (task_root_type *) lock ) {

      // This thread has transitioned this 'task' to complete.
      // 'task' is no longer in a queue and is not executing
      // so decrement the reference count from 'task's creation.
      // If no other references to this 'task' then it will be deleted.

      TaskQueue::assign( & task , zero );

      // This thread has exclusive access to the wait list so
      // the concurrency-safe pop_task function is not needed.
      // Schedule the tasks that have been waiting on the input 'task',
      // which may have been deleted.

      while ( x != end ) {

        // Set x->m_next = zero  <=  no dependence

        task_root_type * const next =
          (task_root_type *) Kokkos::atomic_exchange( & x->m_next , zero );

        schedule( x );

        x = next ;
      }
    }
  }

  if ( runnable ) {
    // A runnable task was popped from a ready queue and executed.
    // If respawned into a ready queue then the ready count was incremented
    // so decrement whether respawned or not.
    Kokkos::atomic_decrement( & m_ready_count );
  }
}

//----------------------------------------------------------------------------

} /* namespace Impl */
} /* namespace Kokkos */


#endif /* #if defined( KOKKOS_ENABLE_TASKPOLICY ) */
